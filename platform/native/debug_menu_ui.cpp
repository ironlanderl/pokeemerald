// platform/native/debug_menu_ui.cpp
//
// The C++ half of the Dear ImGui debug overlay: it builds and draws the
// panels, and owns every ImGui call. The C half (debug_menu.c) owns the
// command queue and all reads of live game state -- it has to be C, because
// the game's headers are (include/sprite.h uses `template` as an identifier)
// while ImGui is C++ with no extern "C" guards.
//
// THREADING. Everything here runs on the present thread (video.c), which owns
// the GL context, after that frame's game GL work and before the swap. It is
// never concurrent with the PPU: the game's thread finishes rasterising into
// its framebuffer and hands it over before this runs, and the only game memory
// touched from here is read-only. State the overlay wants to change is posted
// to the game's thread through native_debug_post_command().
//
// Licensed under the same terms as the pokeemerald decompilation.

#include "debug_menu.h"
#include "native.h"

// GLEW for the GL types and entry points the framebuffer-capture texture needs.
// The same loader is forced into the imgui OpenGL3 backend (see the Makefile),
// so the whole program resolves GL through one set of pointers.
#define GL_GLEXT_PROTOTYPES 1
#include <GL/glew.h>

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_opengl3.h"

#include <SDL.h>
#include <stdio.h>

// The UI half cannot include the game's headers -- include/sprite.h uses
// `template` as an identifier, which is a C++ keyword -- so the few game
// constants it needs are mirrored here with their sources named. All of them
// are fixed by the ROM format or the linker script, not tunable.
namespace {
const int kDisplayWidth = 240;   // include/gba/defines.h
const int kDisplayHeight = 160;  // include/gba/defines.h
const int kSectorsCount = 32;    // include/save.h SECTORS_COUNT
}

namespace {

// --- Panel state ----------------------------------------------------------
//
// All of it is overlay-private and lives on the present thread, so none of it
// needs synchronising. The game's state (layer visibility and friends) is not
// duplicated here: the widgets read it back from the PPU's own flags, so what
// is drawn is what is actually in effect rather than what was asked for.

struct UiState
{
    bool visible = false;
    bool open_memory = true;
    bool open_registers = false;
    bool open_tiles = false;
    bool open_audio = false;
    bool open_timing = false;
    bool open_save = false;
    bool open_ppu = false;


    // Hex viewer. The address is resolved against the mapped regions by the C
    // half, which refuses anything outside them.
    uint32_t hex_addr = 0x02000000;
    uint32_t hex_region = 0;
    bool hex_follow = false;
    uint8_t hex_bytes[256] = {};

    // Framebuffer capture for the PPU inspector, uploaded as a texture.
    unsigned int fb_tex = 0;
    int fb_w = 0;
    int fb_h = 0;

    // Scanline-effect plot.
    int scanline_buffer = 0;
    float plot[0x3C0] = {};

    // Set once, from video.c, so the panels can show real per-layer coverage
    // rather than only the toggles.
    bool layers_primed = false;
    bool layer_visible[6] = {true, true, true, true, true, true};
    bool window_visible = true;
    bool blend_enabled = true;

    float fps = 0.0f;
    float cpu_ms = 0.0f;
};

// Cascades panel windows so several can be open at once. ImGui's FirstUseEver
// default puts every window at the same corner, so without this they stack and
// only the last one is readable.
//
// The grid is laid out against the actual window size rather than fixed pixel
// offsets: ImGui renders into the SDL window, not the X screen, so a panel
// placed at x=600 in a 480-wide window is simply clipped away and looks like
// the panel failed to open. Two columns of kPanelWidth, wrapped into as many
// rows as the window is tall enough for.
const float kPanelWidth = 292.0f;

void PlacePanel(int slot)
{
    ImVec2 avail = ImGui::GetIO().DisplaySize;

    // Leave the top-left for the toolbar window, which is always on screen.
    const float top = 64.0f;
    const float cols = avail.x > kPanelWidth * 2.0f + 32.0f ? 2.0f : 1.0f;
    float rowH = (avail.y - top - 16.0f) / 4.0f;
    if (rowH < 120.0f)
        rowH = 120.0f;

    float x = 8.0f + (slot % (int)cols) * (kPanelWidth + 12.0f);
    float y = top + (slot / (int)cols) * rowH;

    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(kPanelWidth, rowH - 8.0f), ImGuiCond_FirstUseEver);
}

UiState s_ui;

// The overlay only ever posts commands; it never writes these directly.
void PostLayerVisible(int layer, bool on)
{
    native_debug_post_command(DEBUG_CMD_SET_LAYER_VISIBLE, (uint32_t)layer, on ? 1u : 0u);
}

// ---------------------------------------------------------------------------
// Small formatting helpers
// ---------------------------------------------------------------------------

void Bar(float frac, const char *label)
{
    ImGui::TextUnformatted(label);
    ImGui::SameLine(90.0f);
    ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f));
}

// A register row: name and hex value. The values come from the C half, which
// reads the emulated IO page.
void RegTable(const char *id, int first, int count)
{
    int total = 0;
    const struct debug_reg *regs = native_debug_registers(&total);
    if (!regs)
        return;

    if (ImGui::BeginTable(id, 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("Register");
        ImGui::TableSetupColumn("Value");
        ImGui::TableHeadersRow();

        for (int i = first; i < first + count && i < total; i++)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(regs[i].name);
            ImGui::TableNextColumn();
            ImGui::Text("0x%04X", regs[i].value);
        }
        ImGui::EndTable();
    }
}

// ---------------------------------------------------------------------------
// Memory panel
// ---------------------------------------------------------------------------

void DrawMemoryPanel()
{
    PlacePanel(1);
    if (!ImGui::Begin("Memory map", &s_ui.open_memory))
    {
        ImGui::End();
        return;
    }

    if (ImGui::CollapsingHeader("Live regions", ImGuiTreeNodeFlags_DefaultOpen))
    {
        // A table rather than ImGui::Columns: columns divide the width evenly
        // and their contents do not line up when one of them wraps, which
        // "Used / Size" does in a narrow panel.
        if (ImGui::BeginTable("regions", 4,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("Region", ImGuiTableColumnFlags_WidthFixed, 66.0f);
            ImGui::TableSetupColumn("Base", ImGuiTableColumnFlags_WidthFixed, 78.0f);
            ImGui::TableSetupColumn("Used / Size", ImGuiTableColumnFlags_WidthFixed, 104.0f);
            ImGui::TableSetupColumn("Free", ImGuiTableColumnFlags_WidthStretch, 0.0f);
            ImGui::TableHeadersRow();

            int n = native_debug_region_count();
            for (int i = 0; i < n; i++)
            {
                const struct debug_region *r = native_debug_region(i);
                if (!r)
                    continue;

                uint32_t slack = native_debug_region_slack(i);

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r->name);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", r->base);
                ImGui::TableNextColumn();
                ImGui::Text("0x%06X/0x%06X", r->used, r->size);
                ImGui::TableNextColumn();
                if (slack == 0)
                    ImGui::TextDisabled("0x%06X", slack);
                else
                    ImGui::Text("0x%06X", slack);
            }
            ImGui::EndTable();
        }

        ImGui::TextDisabled("Used is the extent written so far (highest non-zero byte).");
        ImGui::TextDisabled("EWRAM/IWRAM are nearly empty by design: native.ld puts");
        ImGui::TextDisabled(".ewram_data/.common_data in the host image at 0x40000000.");
    }

    if (ImGui::CollapsingHeader("Address-pinning slack", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const struct debug_slack *s = native_debug_build_slack();

        ImGui::Text("EWRAM  used 0x%X of 0x%X   free 0x%X  (%.1f%%)",
                    s->ewram_used, s->ewram_size, s->ewram_free,
                    100.0f * s->ewram_free / s->ewram_size);
        Bar(1.0f - (float)s->ewram_free / s->ewram_size, "         ");

        ImGui::Text("IWRAM  used 0x%X of 0x%X   free 0x%X  (%.1f%%)",
                    s->iwram_used, s->iwram_size, s->iwram_free,
                    100.0f * s->iwram_free / s->iwram_size);
        Bar(1.0f - (float)s->iwram_free / s->iwram_size, "         ");

        ImGui::TextDisabled("Free IWRAM excludes the top 0x10 bytes reserved for");
        ImGui::TextDisabled("SOUND_INFO_PTR / INTR_CHECK / INTR_VECTOR.");
        ImGui::TextDisabled("This is the ROM build's figure: the emulated windows are");
        ImGui::TextDisabled("mapped at fixed GBA addresses and cannot be moved.");

        uint32_t total, used;
        native_debug_heap(&total, &used);
        ImGui::Separator();
        ImGui::Text("Heap    used 0x%X of 0x%X   free 0x%X", used, total, total - used);
        Bar((float)used / total, "        ");
    }

    if (ImGui::CollapsingHeader("Hex viewer", ImGuiTreeNodeFlags_DefaultOpen))
    {
        int n = native_debug_region_count();
        int region = (int)s_ui.hex_region;

        if (ImGui::Combo("Region", &region,
                         "EWRAM\0IWRAM\0IO\0Palette\0VRAM\0OAM\0ROM\0Flash\0", n))
        {
            s_ui.hex_region = (uint32_t)region;
            const struct debug_region *r = native_debug_region(region);
            if (r)
                s_ui.hex_addr = r->base;
        }

        ImGui::SameLine();
        char addrbuf[16];
        snprintf(addrbuf, sizeof(addrbuf), "0x%08X", s_ui.hex_addr);
        ImGui::TextUnformatted(addrbuf);

        ImGui::SameLine();
        if (ImGui::Button("<-"))
            s_ui.hex_addr -= 0x100;
        ImGui::SameLine();
        if (ImGui::Button("+"))
            s_ui.hex_addr += 0x100;
        ImGui::SameLine();
        if (ImGui::Button(">>"))
            s_ui.hex_addr += 0x1000;
        ImGui::SameLine();
        ImGui::Checkbox("follow", &s_ui.hex_follow);

        if (s_ui.hex_follow)
        {
            // Follow whatever the game is currently writing: the top of
            // EWRAM is gHeap, whose live tail is the allocator's cursor.
            const struct debug_region *r = native_debug_region(0);
            if (r && r->used > 0x100)
                s_ui.hex_addr = r->base + (r->used & ~0xFFu);
        }

        if (native_debug_read_hex(s_ui.hex_addr, s_ui.hex_bytes))
        {
            ImGui::BeginChild("hex", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
            for (int row = 0; row < 16; row++)
            {
                ImGui::Text("0x%08X  ", s_ui.hex_addr + row * 16);

                const uint8_t *line = s_ui.hex_bytes + row * 16;
                for (int col = 0; col < 16; col++)
                {
                    ImGui::Text("%02X ", line[col]);
                    if (col == 7)
                        ImGui::TextUnformatted(" ");
                }
                // The ASCII column needs a NUL-terminated run, so copy the
                // line rather than pointing into the snapshot.
                char ascii[17];
                for (int col = 0; col < 16; col++)
                {
                    uint8_t c = line[col];
                    ascii[col] = (c >= 32 && c < 127) ? (char)c : '.';
                }
                ascii[16] = 0;
                ImGui::TextUnformatted(ascii);
            }
            ImGui::EndChild();
        }
        else
        {
            ImGui::TextDisabled("0x%08X is not inside a mapped region.", s_ui.hex_addr);
        }
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Registers panel
// ---------------------------------------------------------------------------

void DrawRegistersPanel()
{
    PlacePanel(2);
    if (!ImGui::Begin("Registers", &s_ui.open_registers))
    {
        ImGui::End();
        return;
    }

    // Index order matches native_debug_registers(): DISPCNT, DISPSTAT,
    // BG0-3CNT, BLDCNT, BLDALPHA, BLDY, WIN0H..WINOUT, KEYINPUT, IE, IF,
    // BG0HOFS, BG0VOFS.
    if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextDisabled("mode / BG enables / OBJ / window enables");
        RegTable("regs_disp", 0, 9);
    }

    if (ImGui::CollapsingHeader("Windows", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextDisabled("WININ: w0 BG0-3/OBJ/CLR in bits 0-5, w1 in bits 8-13");
        RegTable("regs_win", 9, 6);
    }

    if (ImGui::CollapsingHeader("Interrupts", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextDisabled("KEYINPUT is active low: a set bit is released");
        RegTable("regs_int", 15, 3);
        ImGui::Text("VCOUNT   %u", native_vcount());
    }

    if (ImGui::CollapsingHeader("gpu_regs.c shadow buffer", ImGuiTreeNodeFlags_DefaultOpen))
    {
        // The shadow buffer and the pending-write queue, straight out of
        // src/gpu_regs.c. A non-empty queue means writes are being deferred
        // because VCOUNT is outside the free window (161..225), or forced
        // blank is set.
        const uint8_t *shadow = native_debug_gpu_shadow();
        const uint8_t *waiting = native_debug_gpu_waiting();
        uint32_t pending = native_debug_gpu_pending_count();

        ImGui::Text("pending writes: %u%s", pending,
                    native_debug_gpu_locked() ? "  (locked)" : "");
        if (pending > 0)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("VCOUNT=%u, free window 161..225", native_vcount());
        }

        // sGpuRegBuffer is indexed by the register's byte offset, and the
        // display registers are contiguous 0x00..0x24, so offset i*2 is the
        // i-th of them.
        static const char *const kNames[] = {
            "DISPCNT", "DISPSTAT", "VCOUNT", "BG0CNT", "BG1CNT", "BG2CNT", "BG3CNT",
            "BG0HOFS", "BG0VOFS", "BG1HOFS", "BG1VOFS", "BG2HOFS", "BG2VOFS",
            "BG3HOFS", "BG3VOFS", "BLDCNT", "BLDALPHA", "BLDY",
        };
        const int kShadowRegs = (int)(sizeof(kNames) / sizeof(kNames[0]));

        ImGui::Separator();
        if (ImGui::BeginTable("shadow", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("Reg");
            ImGui::TableSetupColumn("Shadow");
            ImGui::TableSetupColumn("Live");
            ImGui::TableSetupColumn("Queued");
            ImGui::TableHeadersRow();

            for (int i = 0; i < kShadowRegs; i++)
            {
                uint32_t off = i * 2;
                uint16_t sh;
                memcpy(&sh, shadow + off, sizeof(sh));
                uint16_t live = native_debug_io_read(off);

                bool queued = false;
                for (uint32_t q = 0; q < pending; q++)
                {
                    if (waiting[q] == off)
                    {
                        queued = true;
                        break;
                    }
                }

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(kNames[i]);
                ImGui::TableNextColumn();
                ImGui::Text("0x%04X", sh);
                ImGui::TableNextColumn();
                if (sh != live)
                    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "0x%04X", live);
                else
                    ImGui::Text("0x%04X", live);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(queued ? "yes" : "");
            }
            ImGui::EndTable();
        }
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Tiles and sprites panel
// ---------------------------------------------------------------------------

void DrawTilesPanel()
{
    PlacePanel(3);
    if (!ImGui::Begin("Tiles & sprites", &s_ui.open_tiles))
    {
        ImGui::End();
        return;
    }

    uint32_t oamLimit, reserved, count;
    native_debug_sprites(&oamLimit, &reserved, &count);

    ImGui::Text("gOamLimit               %u", oamLimit);
    ImGui::Text("gReservedSpriteTileCount 0x%X (%u)", reserved, reserved);
    ImGui::Text("sprites in use          %u / %u", count, oamLimit);
    ImGui::Text("OBJ tiles free          %u / %u", 1024u - reserved, 1024u);

    if (ImGui::CollapsingHeader("Active BG bases", ImGuiTreeNodeFlags_DefaultOpen))
    {
        for (int i = 0; i < 4; i++)
        {
            const struct debug_bg *bg = native_debug_bg(i);
            if (!bg)
                continue;

            ImGui::Text("BG%d  cnt 0x%04X  pri %u  char 0x%05X  screen 0x%05X  %s  %s",
                        i, bg->cnt, bg->priority, bg->charBase, bg->screenBase,
                        bg->color256 ? "256col" : " 16col",
                        bg->affine ? "affine" : "");
        }
    }

    if (ImGui::CollapsingHeader("OAM", ImGuiTreeNodeFlags_DefaultOpen))
    {
        // Raw OAM straight from the mapped window: 128 entries of four
        // halfwords, which is what the PPU walks.
        const uint8_t *oam = native_debug_oam();

        ImGui::BeginChild("oam", ImVec2(0.0f, 260.0f), ImGuiChildFlags_Borders);
        if (ImGui::BeginTable("oamtbl", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("#");
            ImGui::TableSetupColumn("Y");
            ImGui::TableSetupColumn("X");
            ImGui::TableSetupColumn("Tile");
            ImGui::TableSetupColumn("Pal");
            ImGui::TableSetupColumn("Pri");
            ImGui::TableSetupColumn("Shape/Sz");
            ImGui::TableSetupColumn("Mode");
            ImGui::TableHeadersRow();

            for (int i = 0; i < 128; i++)
            {
                const uint8_t *e = oam + i * 8;
                uint16_t a0, a1, a2;
                memcpy(&a0, e, 2);
                memcpy(&a1, e + 2, 2);
                memcpy(&a2, e + 4, 2);

                uint32_t y = a0 & 0xFF;
                uint32_t shape = (a0 >> 14) & 3;
                uint32_t size = (a1 >> 14) & 3;
                uint32_t x = a1 & 0x1FF;

                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%d", i);
                ImGui::TableNextColumn(); ImGui::Text("%u", y);
                ImGui::TableNextColumn(); ImGui::Text("%u", x);
                ImGui::TableNextColumn(); ImGui::Text("%u", a2 & 0x3FF);
                ImGui::TableNextColumn(); ImGui::Text("%u", (a2 >> 12) & 0xF);
                ImGui::TableNextColumn(); ImGui::Text("%u", (a2 >> 10) & 3);
                ImGui::TableNextColumn(); ImGui::Text("%u/%u", shape, size);
                ImGui::TableNextColumn(); ImGui::Text("%u", (a0 >> 8) & 3);
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
    }

    if (ImGui::CollapsingHeader("gSprites[]"))
    {
        // Read-only view of the live sprite pool. struct Sprite is private to
        // sprite.h, which the C++ half cannot include, so the C half hands the
        // array over as raw bytes and the few fields shown are read at their
        // documented offsets.
        ImGui::BeginChild("sprites", ImVec2(0.0f, 260.0f), ImGuiChildFlags_Borders);
        if (ImGui::BeginTable("spritetbl", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("#");
            ImGui::TableSetupColumn("X");
            ImGui::TableSetupColumn("Y");
            ImGui::TableSetupColumn("Tile");
            ImGui::TableSetupColumn("Pal");
            ImGui::TableSetupColumn("Pri");
            ImGui::TableSetupColumn("Sub");
            ImGui::TableSetupColumn("Flags");
            ImGui::TableHeadersRow();

            const uint8_t *base = native_debug_sprites_raw();
            for (int i = 0; i < 64; i++)
            {
                // struct OamData is the first 8 bytes of struct Sprite; x/y
                // follow at +0x20/+0x22, subpriority at +0x43, and the flag
                // bytes at +0x3E/+0x3F carry inUse/hFlip/vFlip.
                const uint8_t *sp = base + i * 0x44;
                uint16_t a0, a1, a2, x, y;
                memcpy(&a0, sp, 2);
                memcpy(&a1, sp + 2, 2);
                memcpy(&a2, sp + 4, 2);
                memcpy(&x, sp + 0x20, 2);
                memcpy(&y, sp + 0x22, 2);

                uint8_t flags0 = sp[0x3E];
                uint8_t flags1 = sp[0x3F];
                bool inUse = (flags0 & 0x01) != 0;

                ImGui::TableNextRow();
                if (!inUse)
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));

                ImGui::TableNextColumn(); ImGui::Text("%d", i);
                ImGui::TableNextColumn(); ImGui::Text("%d", (int16_t)x);
                ImGui::TableNextColumn(); ImGui::Text("%d", (int16_t)y);
                ImGui::TableNextColumn(); ImGui::Text("%u", a2 & 0x3FF);
                ImGui::TableNextColumn(); ImGui::Text("%u", (a2 >> 12) & 0xF);
                ImGui::TableNextColumn(); ImGui::Text("%u", (a2 >> 10) & 3);
                ImGui::TableNextColumn(); ImGui::Text("%u", sp[0x43]);

                ImGui::TableNextColumn();
                char fl[8];
                int n = 0;
                if (flags0 & 0x04) fl[n++] = 'i';
                if (flags1 & 0x01) fl[n++] = 'H';
                if (flags1 & 0x02) fl[n++] = 'V';
                if (flags1 & 0x20) fl[n++] = 'S';
                fl[n] = 0;
                ImGui::TextUnformatted(fl);

                if (!inUse)
                    ImGui::PopStyleColor();
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Audio panel
// ---------------------------------------------------------------------------

void DrawAudioPanel()
{
    PlacePanel(4);
    if (!ImGui::Begin("Audio", &s_ui.open_audio))
    {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("The m4a mixer is not ported yet (see platform/native/audio.c):");
    ImGui::TextDisabled("there is no PCM ring buffer to scope, so this panel shows the");
    ImGui::TextDisabled("sequencer state that MPlayMain does maintain.");

    int n = native_debug_player_count();
    if (ImGui::BeginTable("players", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("Player");
        ImGui::TableSetupColumn("Tracks");
        ImGui::TableSetupColumn("Prio");
        ImGui::TableSetupColumn("Status");
        ImGui::TableSetupColumn("Tempo U/I/D");
        ImGui::TableSetupColumn("Acc");
        ImGui::TableSetupColumn("ident");
        ImGui::TableSetupColumn("State");
        ImGui::TableHeadersRow();

        for (int i = 0; i < n; i++)
        {
            const struct debug_player *p = native_debug_player(i);
            if (!p)
                continue;

            // ident is the re-entrancy lock shared with SoundMain: it is set
            // to ID_NUMBER for the duration of a mix pass and restored after.
            // Anything else means a pass is running or was cut short.
            bool locked = (p->ident != 0x68736D53u);

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p->name);
            ImGui::TableNextColumn(); ImGui::Text("%u", p->numTracks);
            ImGui::TableNextColumn(); ImGui::Text("%u", p->priority);

            ImGui::TableNextColumn();
            ImGui::Text("0x%08X", p->status);
            if (p->status & 0x0000FFFFu)
                ImGui::SameLine(), ImGui::TextDisabled("track %u", p->status & 0xFFFFu);
            if (p->status & 0x80000000u)
                ImGui::SameLine(), ImGui::TextDisabled("paused");

            ImGui::TableNextColumn();
            ImGui::Text("%u/%u/%u", p->tempoU, p->tempoI, p->tempoD);
            ImGui::TableNextColumn(); ImGui::Text("%u", p->tempoC);

            ImGui::TableNextColumn();
            if (locked)
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "0x%08X", p->ident);
            else
                ImGui::TextDisabled("idle");

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(p->inUse ? "playing" : "stopped");

            ImGui::TableNextColumn();
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Timing panel
// ---------------------------------------------------------------------------

void DrawTimingPanel()
{
    PlacePanel(5);
    if (!ImGui::Begin("Timing", &s_ui.open_timing))
    {
        ImGui::End();
        return;
    }

    const struct debug_timing *t = native_debug_timing();

    ImGui::Text("virtual cycles    %llu", (unsigned long long)t->cycles);
    ImGui::Text("scanline          %u / 228", t->scanline);
    ImGui::Text("VCOUNT (live)     %u", t->vcount);
    ImGui::Text("vblankCounter1    %u", t->vblankCounter1);
    ImGui::Text("vblankCounter2    %u", t->vblankCounter2);
    ImGui::TextDisabled("counter2 - counter1 is frames the main loop has not caught up on.");

    uint32_t depth, capacity, bytes;
    native_debug_dma3(&depth, &capacity, &bytes);

    if (ImGui::CollapsingHeader("DMA3 deferred queue", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Text("queued requests   %u / %u", depth, capacity);
        Bar((float)depth / capacity, "                ");

        // ProcessDma3Requests stops once 40 KiB has moved in one VBlank, so
        // the pending total is the number that decides whether a frame's VRAM
        // and palette loads all land.
        ImGui::Text("pending bytes     %u / %u  (%.0f%% of budget)",
                    bytes, 40u * 1024u, 100.0f * bytes / (40.0f * 1024.0f));
        Bar(bytes / (40.0f * 1024.0f), "                ");
    }

    if (ImGui::CollapsingHeader("DMA channels", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::BeginTable("dma", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("Ch");
            ImGui::TableSetupColumn("SAD");
            ImGui::TableSetupColumn("DAD");
            ImGui::TableSetupColumn("CNT");
            ImGui::TableSetupColumn("Timing");
            ImGui::TableHeadersRow();

            for (int n = 0; n < 4; n++)
            {
                uint32_t sad, dad, cnt;
                native_dma_get_channel(n, &sad, &dad, &cnt);

                const char *timing = "off";
                if (cnt & 0x80000000u)
                {
                    switch (cnt & 0x30000000u)
                    {
                    case 0x00000000u: timing = "immediate"; break;
                    case 0x10000000u: timing = "vblank"; break;
                    case 0x20000000u: timing = "hblank"; break;
                    default:         timing = "prohibited"; break;
                    }
                }

                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%d", n);
                ImGui::TableNextColumn(); ImGui::Text("0x%08X", sad);
                ImGui::TableNextColumn(); ImGui::Text("0x%08X", dad);
                ImGui::TableNextColumn(); ImGui::Text("0x%08X", cnt);
                ImGui::TableNextColumn();
                if (cnt & 0x80000000u)
                    ImGui::TextUnformatted(timing);
                else
                    ImGui::TextDisabled("off");
            }
            ImGui::EndTable();
        }

        ImGui::TextDisabled("A repeating HBlank channel (DMA0 for scanline effects)");
        ImGui::TextDisabled("keeps its enable bit set and advances SAD every line.");
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Save panel
// ---------------------------------------------------------------------------

void DrawSavePanel()
{
    PlacePanel(6);
    if (!ImGui::Begin("Save", &s_ui.open_save))
    {
        ImGui::End();
        return;
    }

    ImGui::Text("host file: %s", native_debug_save_path());
    ImGui::TextDisabled("mapped MAP_SHARED at 0x%08X, %u bytes, %u sectors",
                        NATIVE_FLASH_BASE, NATIVE_FLASH_SIZE, kSectorsCount);

    int n = native_debug_sector_count();
    if (ImGui::BeginTable("sectors", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("Sector");
        ImGui::TableSetupColumn("Region");
        ImGui::TableSetupColumn("ID");
        ImGui::TableSetupColumn("Cksum");
        ImGui::TableSetupColumn("Signature");
        ImGui::TableSetupColumn("Counter");
        ImGui::TableHeadersRow();

        for (int i = 0; i < n; i++)
        {
            const struct debug_sector *s = native_debug_sector(i);
            if (!s)
                continue;

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%u", s->index);
            ImGui::TableNextColumn();

            if (s->valid)
            {
                ImGui::TextUnformatted(s->region ? s->region : "unknown");
            }
            else
            {
                ImGui::TextDisabled("(unused)");
            }

            ImGui::TableNextColumn(); ImGui::Text("0x%04X", s->id);
            ImGui::TableNextColumn(); ImGui::Text("0x%04X", s->checksum);
            ImGui::TableNextColumn(); ImGui::Text("0x%08X", s->signature);
            ImGui::TableNextColumn(); ImGui::Text("%u", s->counter);
        }
        ImGui::EndTable();
    }

    ImGui::TextDisabled("A sector is live when its signature is 0x%08X.", 0x08012025u);
    ImGui::TextDisabled("Slot 1 is sectors 0-13, slot 2 is 14-27, then HOF/Trainer Hill/");
    ImGui::TextDisabled("recorded battle. Each is %u data bytes + %u footer.",
                        3968u, 128u);

    ImGui::End();
}

// ---------------------------------------------------------------------------
// PPU inspector panel
// ---------------------------------------------------------------------------

// Upload the frame for the PPU inspector.
//
// This uses the same buffer the presenter just drew, not the PPU's own
// framebuffer. Reading native_ppu_framebuffer() from here would race the
// game's thread, which rewrites all 150 KB of it per frame, and the inspector
// would show a torn image; the presenter's slot is a private copy nobody
// writes until the next publish.
void UpdateFramebufferTexture(const uint32_t *fb)
{
    if (!fb)
        return;

    if (s_ui.fb_tex == 0)
    {
        glGenTextures(1, &s_ui.fb_tex);
        glBindTexture(GL_TEXTURE_2D, s_ui.fb_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        s_ui.fb_w = kDisplayWidth;
        s_ui.fb_h = kDisplayHeight;
    }

    glBindTexture(GL_TEXTURE_2D, s_ui.fb_tex);
    // GL_UNSIGNED_BYTE, not GL_UNSIGNED_INT: the framebuffer is a uint32_t
    // array, but an unsized internal format of GL_RGBA accepts only 8-bit
    // components, and passing a 32-bit type is a GL_INVALID_OPERATION that
    // silently leaves the texture undefined (which shows up as solid white).
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s_ui.fb_w, s_ui.fb_h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, fb);
}

void DrawPpuPanel(const uint32_t *fb)
{
    PlacePanel(7);
    if (!ImGui::Begin("PPU inspector", &s_ui.open_ppu))
    {
        ImGui::End();
        return;
    }

    if (ImGui::CollapsingHeader("Framebuffer", ImGuiTreeNodeFlags_DefaultOpen))
    {
        UpdateFramebufferTexture(fb);
        if (s_ui.fb_tex != 0)
        {
            // 2x so the 240x160 image is legible without the window being large.
            ImGui::Image((ImTextureID)(intptr_t)s_ui.fb_tex, ImVec2(480, 320),
                         ImVec2(0, 0), ImVec2(1, 1));
        }
        else
        {
            ImGui::TextDisabled("no frame presented yet");
        }
    }

    if (ImGui::CollapsingHeader("Layer isolation", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextDisabled("Toggles post a command; they take effect next frame.");
        ImGui::Separator();

        static const char *const kLayerNames[6] = {
            "BG0", "BG1", "BG2", "BG3", "OBJ", "Backdrop",
        };

        for (int i = 0; i < 6; i++)
        {
            // Read the live flag rather than a cached copy, so what is shown
            // is what the PPU is actually using even if a command was dropped.
            bool on = native_ppu_layer_visible[i];
            if (ImGui::Checkbox(kLayerNames[i], &on))
                PostLayerVisible(i, on);
        }

        bool win = native_ppu_window_visible;
        if (ImGui::Checkbox("Windows (WININ/WINOUT/OBJWIN)", &win))
            native_debug_post_command(DEBUG_CMD_SET_WINDOW_VISIBLE, win ? 1u : 0u, 0);

        bool blend = native_ppu_blend_enabled;
        if (ImGui::Checkbox("Blending (BLDCNT targets)", &blend))
            native_debug_post_command(DEBUG_CMD_SET_BLEND_ENABLED, blend ? 1u : 0u, 0);
    }

    if (ImGui::CollapsingHeader("Scanline effect", ImGuiTreeNodeFlags_DefaultOpen))
    {
        uint32_t dest = native_debug_scanline_dest();
        uint32_t ctrl = native_debug_scanline_control();

        ImGui::Text("dmaDest   0x%08X", dest);
        ImGui::Text("dmaControl 0x%08X", ctrl);
        ImGui::Text("active buffer %d", native_debug_scanline_active_buffer());

        // One value per scanline: the register value the HBlank DMA writes for
        // that line. Reading the whole 0x3C0-entry buffer as one float per
        // line is what makes a wipe or a wave visible as a shape.
        const uint16_t *vals = native_debug_scanline_values(native_debug_scanline_active_buffer());
        if (vals)
        {
            uint32_t n = native_debug_scanline_count();
            for (uint32_t i = 0; i < n; i++)
                s_ui.plot[i] = (float)vals[i];

            ImGui::Text("BG0HOFS-style values, lines 0..%u", n - 1);
            ImGui::PlotLines("scanline", s_ui.plot, (int)n, 0, NULL, 0.0f, 512.0f,
                             ImVec2(0.0f, 120.0f));

            ImGui::SliderInt("buffer", &s_ui.scanline_buffer, 0, 1);
        }
        ImGui::TextDisabled("dest 0 means no effect is armed.");
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

void DrawOverlay(const uint32_t *fb)
{
    ImGui::NewFrame();

    // The game quad is already on the framebuffer; the overlay goes on top of
    // it in the same frame, before the swap.
    ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.88f);

    if (ImGui::Begin("pokeemerald (native)", &s_ui.visible,
                     ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextDisabled("%.0f fps   overlay %.2f ms", s_ui.fps, s_ui.cpu_ms);
        ImGui::Separator();

        if (ImGui::Button("Memory"))      s_ui.open_memory = !s_ui.open_memory;
        ImGui::SameLine();
        if (ImGui::Button("Registers"))  s_ui.open_registers = !s_ui.open_registers;
        ImGui::SameLine();
        if (ImGui::Button("Tiles"))       s_ui.open_tiles = !s_ui.open_tiles;
        ImGui::SameLine();
        if (ImGui::Button("Audio"))       s_ui.open_audio = !s_ui.open_audio;
        ImGui::SameLine();
        if (ImGui::Button("Timing"))      s_ui.open_timing = !s_ui.open_timing;
        ImGui::SameLine();
        if (ImGui::Button("Save"))        s_ui.open_save = !s_ui.open_save;
        ImGui::SameLine();
        if (ImGui::Button("PPU"))         s_ui.open_ppu = !s_ui.open_ppu;
        ImGui::SameLine();

        if (ImGui::Button("Close (F1)"))
            s_ui.visible = false;
    }
    ImGui::End();

    // Each panel is gated on its own flag here rather than relying on Begin().
//
// ImGui's Begin(name, bool* p_open) does not use p_open to decide whether to
// show the window -- it only adds the close button and sets the pointed value
// to false when that is clicked. Begin() returns "visible and not collapsed",
// so a panel with open_* false still drew. The flag has to be tested at the
// call site, and passing &open_* to Begin() as well means the X button closes
// the panel the same way the toolbar button does.
if (s_ui.visible)
    {
        if (s_ui.open_memory)    DrawMemoryPanel();
        if (s_ui.open_registers) DrawRegistersPanel();
        if (s_ui.open_tiles)     DrawTilesPanel();
        if (s_ui.open_audio)     DrawAudioPanel();
        if (s_ui.open_timing)    DrawTimingPanel();
        if (s_ui.open_save)      DrawSavePanel();
        if (s_ui.open_ppu)       DrawPpuPanel(fb);
    }

    ImGui::Render();
}

} // namespace

// ---------------------------------------------------------------------------
// ImGui lifecycle (called from the present thread)
// ---------------------------------------------------------------------------

extern "C" void native_debug_menu_init(void)
{
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL; // never write a .ini next to the ROM
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    // The default font is embedded in imgui_draw.cpp, so there is no font file
    // to ship and nothing to go wrong if the CWD is read-only.
    ImGui_ImplSDL2_InitForOpenGL(native_video_window(), native_video_gl_context());
    ImGui_ImplOpenGL3_Init("#version 130");

    native_log("debug menu: ImGui %s up", IMGUI_VERSION);
}

extern "C" void native_debug_menu_set_visible(bool visible)
{
    s_ui.visible = visible;
}

// `fb` is the frame the presenter just drew (video.c's own slot), or NULL if
// this iteration had nothing new -- the panels still render, they just hold the
// last image.
extern "C" void native_debug_menu_render(const uint32_t *fb)
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();

    // The toggle is a request from the game's thread, consumed here because
    // this is the thread that owns the ImGui context and the window.
    if (native_debug_menu_toggle_requested())
        s_ui.visible = !s_ui.visible;

    uint64_t t0 = native_debug_now_us();
    DrawOverlay(fb);
    s_ui.cpu_ms = (float)(native_debug_now_us() - t0) / 1000.0f;

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

extern "C" void native_debug_menu_shutdown(void)
{
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
}

// Called by video.c's present thread for every event the game's thread
// dequeued. ImGui contexts are not thread-safe, so this is the only way an
// event reaches the overlay.
extern "C" void native_debug_menu_process_event(const void *sdlEvent)
{
    ImGui_ImplSDL2_ProcessEvent((const SDL_Event *)sdlEvent);
}

// Called by video.c once per presented frame, to keep the header's numbers live.
extern "C" void native_debug_menu_note_frame(float fps)
{
    s_ui.fps = fps;
}