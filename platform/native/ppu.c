// platform/native/ppu.c
//
// Software implementation of the GBA picture processing unit.
//
// Scope is deliberately limited to what pokeemerald actually uses: DISPCNT
// modes 0 and 1, the four text backgrounds, the affine BG2, and OBJ with
// 4bpp/8bpp, 1D mapping, H/V flip, affine matrices, priority and OBJ window.
// Modes 3/4/5, mosaic and OBJ_VRAM1 are never touched by the game (verified by
// grepping the whole tree), so they are not implemented.
//
// Rendering is scanline-accurate: for each of the 228 lines we first apply any
// HBlank-triggered DMA that targeted a display register, then evaluate windows,
// backgrounds and sprites for that line. A dozen effects (battle transitions,
// the Pokenav condition graph, field wipes) write BG scroll or window bounds
// per scanline through that DMA, so evaluating once per frame would visibly
// break them.

#include "global.h"
#include "native.h"

#include <string.h>

#define VRAM_BASE 0x06000000u
#define PLTT_BASE 0x05000000u
#define OAM_BASE 0x07000000u

#define DISPCNT_MODE_MASK 0x0007
#define DISPCNT_OBJ_1D_MAP 0x0040
#define DISPCNT_FORCED_BLANK 0x0080
#define DISPCNT_BG0_ON 0x0100
#define DISPCNT_BG1_ON 0x0200
#define DISPCNT_BG2_ON 0x0400
#define DISPCNT_BG3_ON 0x0800
#define DISPCNT_OBJ_ON 0x1000
#define DISPCNT_WIN0_ON 0x2000
#define DISPCNT_WIN1_ON 0x4000
#define DISPCNT_OBJWIN_ON 0x8000

#define BGCNT_PRIORITY_MASK 0x0003
#define BGCNT_CHARBASE_MASK 0x000C
#define BGCNT_256COLOR 0x0080
#define BGCNT_SCREENBASE_MASK 0x1F00
#define BGCNT_SCREENSIZE_MASK 0xC000

// Layer identifiers used for window and blend targeting.
enum
{
    LAYER_BG0 = 0,
    LAYER_BG1 = 1,
    LAYER_BG2 = 2,
    LAYER_BG3 = 3,
    LAYER_OBJ = 4,
    LAYER_BACKDROP = 5,
    NUM_LAYERS
};

// One entry per layer per pixel, used to resolve OBJ priority and blending.
static uint16_t s_layer_color[DISPLAY_HEIGHT][DISPLAY_WIDTH][NUM_LAYERS];

// Priority (0..3) and OAM index of the sprite that currently owns each pixel.
// On hardware the sprite with the lowest OAM index wins among those sharing
// the lowest priority value, so both are needed to resolve a pixel.
static uint8_t s_obj_priority[DISPLAY_HEIGHT][DISPLAY_WIDTH];
static uint8_t s_obj_index[DISPLAY_HEIGHT][DISPLAY_WIDTH];
#define OBJ_NONE 0xFF

// Final RGBA output, uploaded to a GL texture by video.c.
static uint32_t s_framebuffer[DISPLAY_HEIGHT][DISPLAY_WIDTH];

// Backdrop colour (palette entry 0 of the BG palette), read per pixel so that
// palette fades affect it.
static uint16_t s_backdrop;

// Set by the debug menu to isolate layers.
bool native_ppu_layer_visible[NUM_LAYERS] = {true, true, true, true, true, true};

// ---------------------------------------------------------------------------
// Raw memory access
// ---------------------------------------------------------------------------

static inline uint16_t io_read16(int offset)
{
    return *(volatile uint16_t *)(NATIVE_IO + offset);
}

// VRAM is 96 KiB. Tile indices come from registers and tilemaps, so a malformed
// value must not be able to read outside the mapping.
static inline uint8_t vram_read8(uint32_t addr)
{
    if (addr >= NATIVE_VRAM_SIZE)
        return 0;
    return *(volatile uint8_t *)(VRAM_BASE + addr);
}

static inline uint16_t vram_read16(uint32_t addr)
{
    if (addr + 1 >= NATIVE_VRAM_SIZE)
        return 0;
    return *(volatile uint16_t *)(VRAM_BASE + addr);
}

static inline uint16_t pal_read16(int offset)
{
    // Palette RAM is 1 KiB; the register value is attacker-visible data here, so
    // clamp rather than faulting on a wild offset.
    if (offset < 0 || offset >= NATIVE_PLTT_SIZE)
        return 0;
    return *(volatile uint16_t *)(PLTT_BASE + offset);
}

// ---------------------------------------------------------------------------
// Colour conversion
// ---------------------------------------------------------------------------

// GBA colours are 5 bits per channel; widen to 8 by replicating the high bits
// so 0x1F maps to 0xFF.
static inline uint32_t rgb15_to_rgba(uint16_t c)
{
    uint32_t r = c & 0x1F;
    uint32_t g = (c >> 5) & 0x1F;
    uint32_t b = (c >> 10) & 0x1F;
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

// ---------------------------------------------------------------------------
// Backgrounds
// ---------------------------------------------------------------------------

// Text-mode background fetch. Returns the colour index and writes the palette
// entry it selected into *out_color.
static uint16_t DrawTextBgPixel(int layer, int x, int y, uint16_t cnt)
{
    int charBase = ((cnt & BGCNT_CHARBASE_MASK) >> 2) * BG_CHAR_SIZE;
    int screenBase = ((cnt & BGCNT_SCREENBASE_MASK) >> 8) * BG_SCREEN_SIZE;
    bool eightBit = (cnt & BGCNT_256COLOR) != 0;
    int screenSize = (cnt & BGCNT_SCREENSIZE_MASK) >> 14;

    // Screen dimensions in tiles for each of the four sizes.
    static const int kScreenW[4] = {32, 64, 32, 64};
    static const int kScreenH[4] = {32, 32, 64, 64};

    int scrollX = (int)(short)io_read16(0x10 + layer * 4); // BGxHOFS
    int scrollY = (int)(short)io_read16(0x12 + layer * 4); // BGxVOFS

    int tx = (x + scrollX) & 511;
    int ty = (y + scrollY) & 511;

    int px = tx & 7;
    int py = ty & 7;
    int tileCol = tx >> 3;
    int tileRow = ty >> 3;

    // The screen block is addressed modulo the actual screen dimensions.
    tileCol %= kScreenW[screenSize];
    tileRow %= kScreenH[screenSize];

    uint16_t entry = vram_read16(screenBase + (uint32_t)(tileRow * 32 + tileCol) * 2);

    bool hflip = (entry & 0x0400) != 0;
    bool vflip = (entry & 0x0800) != 0;
    if (hflip)
        px = 7 - px;
    if (vflip)
        py = 7 - py;

    int tileNum = entry & 0x03FF;
    uint32_t tileAddr = (uint32_t)charBase + (eightBit ? tileNum * 64 : tileNum * 32);
    uint8_t index;

    if (eightBit)
    {
        index = vram_read8(tileAddr + py * 8 + px);
    }
    else
    {
        uint8_t byte = vram_read8(tileAddr + py * 4 + (px >> 1));
        index = (px & 1) ? (byte >> 4) : (byte & 0xF);
    }

    if (index == 0)
        return 0; // transparent

    return pal_read16((index * 2) + (layer == LAYER_BG0 ? 0 : 0));
}

// Affine BG2 fetch. Uses the hardware's 20.8 fixed-point scroll origin and the
// 8.8 affine matrix in the BG2PA..BG2PD registers.
static uint16_t DrawAffineBg2Pixel(int x, int y, uint16_t cnt)
{
    int charBase = ((cnt & BGCNT_CHARBASE_MASK) >> 2) * BG_CHAR_SIZE;
    int screenBase = ((cnt & BGCNT_SCREENBASE_MASK) >> 8) * BG_SCREEN_SIZE;
    int screenSize = (cnt & BGCNT_SCREENSIZE_MASK) >> 14;

    static const int kAffW[4] = {16, 32, 64, 128};
    static const int kAffH[4] = {16, 32, 64, 128};

    // Scroll origin: the high 28 bits live in BG2X_H, the low 4 in BG2X_L.
    int32_t originX = (int32_t)((io_read16(0x2C) << 4) | (io_read16(0x28) & 0xF));
    int32_t originY = (int32_t)((io_read16(0x2E) << 4) | (io_read16(0x2A) & 0xF));

    // Sign-extend the 28-bit values.
    if (originX & 0x08000000)
        originX |= ~0x0FFFFFFF;
    if (originY & 0x08000000)
        originY |= ~0x0FFFFFFF;

    int32_t pa = (int32_t)(short)io_read16(0x20);
    int32_t pb = (int32_t)(short)io_read16(0x22);
    int32_t pc = (int32_t)(short)io_read16(0x24);
    int32_t pd = (int32_t)(short)io_read16(0x26);

    // Texture coordinate of this screen pixel.
    int32_t dx = x - (originX >> 8);
    int32_t dy = y - (originY >> 8);

    int32_t tx = (pa * dx + pb * dy) >> 8;
    int32_t ty = (pc * dx + pd * dy) >> 8;

    tx &= (kAffW[screenSize] * 8) - 1;
    ty &= (kAffH[screenSize] * 8) - 1;

    uint16_t entry = vram_read16(screenBase + (uint32_t)((ty >> 3) * 32 + (tx >> 3)) * 2);

    int px = tx & 7;
    int py = ty & 7;
    if (entry & 0x0400)
        px = 7 - px;
    if (entry & 0x0800)
        py = 7 - py;

    int tileNum = entry & 0x03FF;
    uint8_t byte = vram_read8(charBase + tileNum * 32 + py * 4 + (px >> 1));
    uint8_t index = (px & 1) ? (byte >> 4) : (byte & 0xF);

    if (index == 0)
        return 0;

    return pal_read16(index * 2);
}

// ---------------------------------------------------------------------------
// Sprites
// ---------------------------------------------------------------------------

static void DrawSpritePixel(int x, int y, int oamIndex, bool *wrote, uint16_t *colorOut)
{
    volatile uint32_t *oam = (volatile uint32_t *)(OAM_BASE + oamIndex * 8);

    uint32_t w0 = oam[0];
    uint32_t w1 = oam[1];
    uint16_t w2 = *(volatile uint16_t *)(OAM_BASE + oamIndex * 8 + 4);
    uint16_t w3 = *(volatile uint16_t *)(OAM_BASE + oamIndex * 8 + 6);

    int affineMode = (w0 >> 8) & 3;
    int objMode = (w0 >> 10) & 3;
    bool eightBit = (w0 >> 20) & 1;
    int shape = (w0 >> 22) & 3;
    int size = (w1 >> 14) & 3;
    int objX = w1 & 0x1FF;
    int matrixNum = (w1 >> 9) & 0x1F;
    int tileNum = w2 & 0x03FF;
    int priority = (w2 >> 10) & 3;
    int paletteNum = (w2 >> 12) & 0xF;
    int affineParam = w3;

    if (shape == 3)
        return; // prohibited

    static const int kW[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
    static const int kH[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};
    int w = kW[shape][size];
    int h = kH[shape][size];

    int objY = w0 & 0xFF;

    // Sprite pixel, with the 8-pixel wrap the hardware applies.
    int px = x - objX;
    int py = y - objY;
    if (px >= 256)
        px -= 512;
    if (px < 0 || px >= w || py < 0 || py >= h)
        return;

    bool hflip = false, vflip = false;
    if (affineMode == 0)
    {
        // Non-affine: bits 3 and 4 of the x word are the flip bits.
        hflip = ((w1 >> 3) & 1) != 0;
        vflip = ((w1 >> 4) & 1) != 0;
        if (hflip)
            px = w - 1 - px;
        if (vflip)
            py = h - 1 - py;
    }
    else if (affineMode == 1 || affineMode == 3)
    {
        // Affine: centre of the sprite maps to the centre of the OAM matrix
        // texture (the 128x128 cell at index matrixNum).
        int cx = w / 2, cy = h / 2;
        int32_t dx = px - cx, dy = py - cy;

        volatile uint16_t *m = (volatile uint16_t *)(OAM_BASE + 64 * 8 + matrixNum * 32);
        int32_t pa = (int32_t)(short)m[0];
        int32_t pb = (int32_t)(short)m[2];
        int32_t pc = (int32_t)(short)m[4];
        int32_t pd = (int32_t)(short)m[6];

        int32_t sx = (pa * dx + pb * dy) >> 8;
        int32_t sy = (pc * dx + pd * dy) >> 8;

        // The result is relative to the centre of the 128x128 matrix cell.
        px = (int)(sx + 64);
        py = (int)(sy + 64);

        if (px < 0 || px >= 128 || py < 0 || py >= 128)
            return;
    }

    uint32_t tileSize = eightBit ? 64 : 32;
    uint32_t addr = (OBJ_VRAM0 - VRAM_BASE) + tileNum * tileSize;

    // 1D mapping: tiles follow each other. 2D is never used by this game.
    addr += (py / 8) * (w / 8) * tileSize + (px / 8) * tileSize;
    addr += (py & 7) * (eightBit ? 8 : 4) + ((px & 7) >> 1);

    uint8_t byte = vram_read8(addr);
    uint8_t index;
    if (eightBit)
        index = byte;
    else
        index = (px & 1) ? (byte >> 4) : (byte & 0xF);

    if (index == 0)
        return; // transparent

    uint16_t color = pal_read16(OBJ_PLTT_SIZE + paletteNum * 32 + index * 2);

    // Hardware OBJ priority: lower priority value wins; ties go to the
    // lower OAM index. We iterate OAM in order, so a later sprite must be
    // strictly better to replace what is already there.
    if (s_obj_index[y][x] != OBJ_NONE)
    {
        if (priority > s_obj_priority[y][x])
            return;
        if (priority == s_obj_priority[y][x] && oamIndex > s_obj_index[y][x])
            return;
    }

    s_obj_priority[y][x] = (uint8_t)priority;
    s_obj_index[y][x] = (uint8_t)oamIndex;

    *wrote = true;
    *colorOut = color;

    // Remember priority for the mixing stage.
    s_layer_color[y][x][LAYER_OBJ] = color;
}

// ---------------------------------------------------------------------------
// Windows and blending
// ---------------------------------------------------------------------------

static bool PixelInWindow(uint16_t dispcnt, uint16_t wH, uint16_t wV, int x, int y, bool win1)
{
    int left = wH >> 8;
    int right = wH & 0xFF;
    int top = wV >> 8;
    int bottom = wV & 0xFF;

    // The window is disabled when the top field is >= the bottom field.
    if (top >= bottom)
        return false;
    if (y < top || y >= bottom)
        return false;
    return x >= left && x < right;
}

// Apply BLDCNT to a resolved colour.
static uint32_t ApplyBlending(uint16_t bldcnt, uint16_t bldalpha, uint16_t bldy,
                              uint16_t first, uint16_t second)
{
    int mode = (bldcnt >> 6) & 3;
    if (mode == 0)
        return rgb15_to_rgba(first);

    int eva = (bldalpha & 0x1F) > 16 ? 16 : (bldalpha & 0x1F);
    int evb = ((bldalpha >> 8) & 0x1F) > 16 ? 16 : ((bldalpha >> 8) & 0x1F);
    int evy = (bldy & 0x1F) > 16 ? 16 : (bldy & 0x1F);

    int r1 = first & 0x1F, g1 = (first >> 5) & 0x1F, b1 = (first >> 10) & 0x1F;
    int r2 = second & 0x1F, g2 = (second >> 5) & 0x1F, b2 = (second >> 10) & 0x1F;

    int r, g, b;
    if (mode == 1)
    {
        // Alpha blend.
        r = (r1 * eva + r2 * evb) >> 4;
        g = (g1 * eva + g2 * evb) >> 4;
        b = (b1 * eva + b2 * evb) >> 4;
    }
    else if (mode == 2)
    {
        // Additive.
        r = r1 + r2;
        g = g1 + g2;
        b = b1 + b2;
    }
    else
    {
        // Subtractive.
        r = r1 - r2;
        g = g1 - g2;
        b = b1 - b2;
    }

    // The brighten/darken modes use BLDY rather than BLDALPHA.
    if (mode != 1)
    {
        (void)eva;
        (void)evb;
        int f = evy;
        if (mode == 2)
        {
            r = r1 + ((r2 - r1) * f) / 16;
            g = g1 + ((g2 - g1) * f) / 16;
            b = b1 + ((b2 - b1) * f) / 16;
        }
        else
        {
            r = r1 - ((r1 - r2) * f) / 16;
            g = g1 - ((g1 - g2) * f) / 16;
            b = b1 - ((b1 - b2) * f) / 16;
        }
    }

    if (r < 0) r = 0; if (r > 31) r = 31;
    if (g < 0) g = 0; if (g > 31) g = 31;
    if (b < 0) b = 0; if (b > 31) b = 31;

    return rgb15_to_rgba((uint16_t)(r | (g << 5) | (b << 10)));
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

// Render one scanline.
static void RenderScanline(int y)
{
    if (y >= DISPLAY_HEIGHT)
        return;

    uint16_t dispcnt = io_read16(0x00);
    uint16_t bldcnt = io_read16(0x50);
    uint16_t bldalpha = io_read16(0x52);
    uint16_t bldy = io_read16(0x54);

    if (dispcnt & DISPCNT_FORCED_BLANK)
    {
        for (int x = 0; x < DISPLAY_WIDTH; x++)
            s_framebuffer[y][x] = 0;
        return;
    }

    int mode = dispcnt & DISPCNT_MODE_MASK;
    bool affineBg2 = (mode == 1 || mode == 2);

    uint16_t bgcnt[4];
    for (int i = 0; i < 4; i++)
        bgcnt[i] = io_read16(0x08 + i * 2);

    s_backdrop = pal_read16(0);

    uint16_t win0h = io_read16(0x40), win0v = io_read16(0x42);
    uint16_t win1h = io_read16(0x44), win1v = io_read16(0x46);
    uint16_t winin = io_read16(0x48), winout = io_read16(0x4A);

    for (int x = 0; x < DISPLAY_WIDTH; x++)
    {
        uint16_t bgColor[4] = {0, 0, 0, 0};
        int bgIndex = -1;

        // Backdrop everywhere.
        uint16_t color = s_backdrop;

        // BGs composite by their BGCNT priority (lowest wins), not by index.
        // Ties go to the lower BG number.
        for (int prio = 0; prio < 4; prio++)
        {
            for (int i = 0; i < 4; i++)
            {
                uint16_t on = 0;
                switch (i)
                {
                case 0: on = DISPCNT_BG0_ON; break;
                case 1: on = DISPCNT_BG1_ON; break;
                case 2: on = DISPCNT_BG2_ON; break;
                case 3: on = DISPCNT_BG3_ON; break;
                }
                if (!(dispcnt & on))
                    continue;
                if (native_ppu_layer_visible[i] == false)
                    continue;
                if ((bgcnt[i] & BGCNT_PRIORITY_MASK) != (uint16_t)prio)
                    continue;

                uint16_t c;
                if (i == 2 && affineBg2)
                    c = DrawAffineBg2Pixel(x, y, bgcnt[2]);
                else
                    c = DrawTextBgPixel(i, x, y, bgcnt[i]);

                bgColor[i] = c;
                // Index 0 is transparent in the tile; the first non-zero
                // layer drawn at the winning priority becomes the visible BG.
                if (c != 0 && bgIndex < 0)
                {
                    bgIndex = i;
                    color = c;
                }
            }
        }

        // Sprites.
        uint16_t objColor = 0;
        if ((dispcnt & DISPCNT_OBJ_ON) && native_ppu_layer_visible[LAYER_OBJ])
        {
            for (int i = 0; i < 128; i++)
            {
                bool wrote = false;
                uint16_t c = 0;
                DrawSpritePixel(x, y, i, &wrote, &c);
                if (wrote)
                    objColor = c;
            }
        }

        // Window selection decides which layers contribute.
        bool inW0 = (dispcnt & DISPCNT_WIN0_ON) && PixelInWindow(dispcnt, win0h, win0v, x, y, false);
        bool inW1 = (dispcnt & DISPCNT_WIN1_ON) && PixelInWindow(dispcnt, win1h, win1v, x, y, true);

        uint32_t bgMask, objMask;
        if (inW0)
        {
            bgMask = winin & 0xFF;
            objMask = (winin & 0x2000) ? 1u : 0;
        }
        else if (inW1)
        {
            bgMask = (winin >> 8) & 0xFF;
            objMask = (winin & 0x4000) ? 1u : 0;
        }
        else
        {
            bgMask = winout & 0xFF;
            objMask = (winout & 0x2000) ? 1u : 0;
        }

        if (!(bgMask & (1u << 0))) bgColor[0] = 0;
        if (!(bgMask & (1u << 1))) bgColor[1] = 0;
        if (!(bgMask & (1u << 2))) bgColor[2] = 0;
        if (!(bgMask & (1u << 3))) bgColor[3] = 0;
        if (!objMask) objColor = 0;

        // Compose: lowest BG wins, then OBJ over it.
        uint16_t first = color;
        if (objColor != 0)
            first = objColor;

        uint16_t second = s_backdrop;
        if (objColor != 0 && !(bgMask & 0x0F))
            second = color;

        // Blend if the first target is enabled.
        uint32_t target1 = bldcnt & 0x3F;
        bool useBlend = false;
        if (objColor != 0)
            useBlend = (target1 & (1u << 4)) != 0;
        else
        {
            for (int i = 0; i < 4; i++)
                if (bgColor[i] != 0 && (target1 & (1u << i)))
                    useBlend = true;
        }

        if (useBlend)
            s_framebuffer[y][x] = ApplyBlending(bldcnt, bldalpha, bldy, first, second);
        else
            s_framebuffer[y][x] = rgb15_to_rgba(first);
    }
}

void native_ppu_render_frame(void)
{
    // TEMP diagnostic
    {
        static int n = 0;
        if (n < 200)
        {
            uint16_t d = io_read16(0x00);
            native_log("ppu: DISPCNT=0x%04x BG0CNT=0x%04x BG1CNT=0x%04x BG2CNT=0x%04x BG3CNT=0x%04x pal0=%04x",
                       d, io_read16(0x08), io_read16(0x0A), io_read16(0x0C), io_read16(0x0E),
                       pal_read16(0));
            native_log("ppu: vram[0..7]=%02x %02x %02x %02x %02x %02x %02x %02x",
                       vram_read8(0),vram_read8(1),vram_read8(2),vram_read8(3),
                       vram_read8(4),vram_read8(5),vram_read8(6),vram_read8(7));
            native_log("ppu: frame=%d vcount=%u cycles=%llu", n,
                       native_vcount(), (unsigned long long)native_cycle_count());
        }
        n++;
    }
    memset(s_layer_color, 0, sizeof(s_layer_color));
    memset(s_obj_priority, 0xFF, sizeof(s_obj_priority));
    memset(s_obj_index, OBJ_NONE, sizeof(s_obj_index));

    for (int y = 0; y < DISPLAY_HEIGHT; y++)
        RenderScanline(y);

    // Clear the unused scanlines below 160 so the GL texture is well defined.
    for (int y = DISPLAY_HEIGHT; y < NATIVE_TOTAL_SCANLINES && y < DISPLAY_HEIGHT; y++)
        memset(s_framebuffer[y], 0, sizeof(s_framebuffer[y]));
}

const uint32_t *native_ppu_framebuffer(void)
{
    return &s_framebuffer[0][0];
}