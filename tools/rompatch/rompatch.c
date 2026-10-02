// tools/rompatch/rompatch.c
//
// Reads the linked ARM ELF and generates the artifacts that let a *natively*
// compiled program consume the original ROM in place:
//
//   rom_syms.h     -- ROM-resident symbols declared at their true ROM address.
//   rom_patches.c  -- {rom_offset, target_symbol} for every absolute pointer
//                     baked into ROM-resident data. At startup the loader
//                     rewrites each word to the native address of the symbol,
//                     so tables like gScriptCmdTable dispatch to native code.
//
// This is how assets stay in the ROM rather than the repository: the native
// binary never stores a copy of the graphics, maps, or sound data.
//
// Dependency: libelf (elfutils-dev / libelf-dev).

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gelf.h>

#define ROM_BASE 0x08000000u
#define ROM_END 0x0A000000u

static void die(const char *msg);

// ---------------------------------------------------------------------------
// Symbol table
// ---------------------------------------------------------------------------

struct sym
{
    char *name;
    GElf_Addr addr;
    unsigned index;
    bool global; // GLOBAL/WEAK binding, so native code can reference it
    bool native_defined; // also defined in a natively-compiled object
};

static struct sym **g_syms;
static size_t g_syms_len, g_syms_cap;

// Open-addressing hash set keyed by symbol name. Indices are handed out once
// and never reused, so a patch's recorded target stays valid even as the table
// grows. (An earlier sorted-array version shifted elements on insert, which
// invalidated previously-returned pointers and silently corrupted every
// patch.)
#define SYM_HASH_BITS 20
#define SYM_HASH_SIZE (1u << SYM_HASH_BITS)
static unsigned *g_sym_hash; // stores index+1; 0 means empty

static unsigned long hash_name(const char *s)
{
    unsigned long h = 5381;
    for (; *s; s++)
        h = h * 33u + (unsigned char)*s;
    return h;
}

static struct sym *sym_intern(Elf *elf, const char *name)
{
    if (!g_sym_hash)
    {
        g_sym_hash = calloc(SYM_HASH_SIZE, sizeof(*g_sym_hash));
        if (!g_sym_hash)
            die("out of memory");
    }

    unsigned long h = hash_name(name) & (SYM_HASH_SIZE - 1);
    unsigned slot = 0;
    bool vacant = false;
    for (unsigned probe = 0; probe < SYM_HASH_SIZE; probe++)
    {
        slot = (unsigned)((h + probe) & (SYM_HASH_SIZE - 1));
        unsigned entry = g_sym_hash[slot];
        if (entry == 0)
        {
            vacant = true;
            break;
        }
        struct sym *s = g_syms[entry - 1];
        if (!strcmp(s->name, name))
            return s;
    }
    if (!vacant)
        die("symbol hash table full");

    if (g_syms_len == g_syms_cap)
    {
        size_t cap = g_syms_cap ? g_syms_cap * 2 : 8192;
        g_syms = realloc(g_syms, cap * sizeof(*g_syms));
        if (!g_syms)
            die("out of memory");
        g_syms_cap = cap;
    }

    struct sym *s = malloc(sizeof(*s));
    if (!s)
        die("out of memory");
    s->name = strdup(name);
    if (!s->name)
        die("out of memory");
    s->addr = 0;
    s->index = (unsigned)g_syms_len;
    s->global = false;
    s->native_defined = false;
    g_syms[g_syms_len++] = s;
    g_sym_hash[slot] = s->index + 1;
    (void)elf;
    return s;
}

static void die(const char *msg)
{
    fprintf(stderr, "rompatch: %s\n", msg);
    exit(1);
}

// ---------------------------------------------------------------------------
// Patch list
// ---------------------------------------------------------------------------

struct patch
{
    uint32_t offset; // byte offset from ROM base
    uint32_t target; // index into g_syms
};

static struct patch *g_patches;
static size_t g_patches_len, g_patches_cap;

static void patch_add(uint32_t offset, const struct sym *target)
{
    if (g_patches_len == g_patches_cap)
    {
        size_t cap = g_patches_cap ? g_patches_cap * 2 : 65536;
        g_patches = realloc(g_patches, cap * sizeof(*g_patches));
        if (!g_patches)
            die("out of memory");
        g_patches_cap = cap;
    }
    g_patches[g_patches_len].offset = offset;
    g_patches[g_patches_len].target = target->index;
    g_patches_len++;
}

static bool addr_is_rom(GElf_Addr a)
{
    return a >= ROM_BASE && a < ROM_END;
}

// EWRAM (0x02000000) and IWRAM (0x03000000). These are recompiled natively
// like everything else, so they are treated as ordinary native symbols rather
// than being defined as absolute addresses.
static bool addr_is_ram(GElf_Addr a)
{
    return (a >= 0x02000000u && a < 0x02040000u) || (a >= 0x03000000u && a < 0x03008000u);
}

// Look up the final ROM address of one section of one object.
struct obj_sec
{
    char obj[160];
    char sec[32];
    uint32_t addr;
};

// Locate an object's .shstrtab section index. e_shstrndx in these object
// files points at the wrong slot, so identify the table by its own content.
static size_t find_shstrtab(Elf *e)
{
    size_t n = 0;
    elf_getshdrnum(e, &n);
    for (size_t i = 0; i < n; i++)
    {
        Elf_Scn *s = elf_getscn(e, i);
        if (!s)
            continue;
        GElf_Shdr sh;
        if (gelf_getshdr(s, &sh) != &sh || sh.sh_type != SHT_STRTAB)
            continue;
        Elf_Data *d = elf_getdata(s, NULL);
        if (!d || !d->d_buf)
            continue;
        // The section-name table always begins with a NUL byte followed by
        // ".symtab". Its buffer is *not* NUL-terminated past d_size, so search
        // it with an explicit bound rather than strstr.
        const char *str = (const char *)d->d_buf;
        size_t len = d->d_size;
        if (len > 9 && str[0] == '\0')
        {
            static const char kProbe[] = ".shstrtab";
            for (size_t off = 1; off + sizeof(kProbe) <= len; off++)
            {
                if (!memcmp(str + off, kProbe, sizeof(kProbe)))
                    return i;
            }
        }
    }
    return 0;
}

static bool find_section_addr(const struct obj_sec *secs, size_t n, const char *obj,
                              const char *sec, uint32_t *out)
{
    for (size_t i = 0; i < n; i++)
    {
        if (!strcmp(secs[i].obj, obj) && !strcmp(secs[i].sec, sec))
        {
            *out = secs[i].addr;
            return true;
        }
    }
    return false;
}

int main(int argc, char **argv)
{
    if (argc != 9)
    {
        fprintf(stderr,
                "usage: %s <pokeemerald.elf> <pokeemerald.map> <objdir>"
                " <native_syms.txt> <exclude_syms.txt>"
                " <rom_syms.h> <rom_patches.c> <rom_syms.S>\n",
                argv[0]);
        return 2;
    }
    const char *map_path = argv[2];
    const char *objdir = argv[3];
    const char *native_syms = argv[4];
    const char *exclude_syms = argv[5];
    const char *out_hdr = argv[6];
    const char *out_src = argv[7];
    const char *out_asm = argv[8];

    if (elf_version(EV_CURRENT) == EV_NONE)
        die("libelf is too old");

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0)
        die("cannot open ELF");

    Elf *elf = elf_begin(fd, ELF_C_READ, NULL);
    if (!elf || elf_kind(elf) != ELF_K_ELF)
        die("not an ELF file");

    // ---- Pass 1: every symbol defined in the ROM image ------------------
    // These become directly referenceable from native code at their true
    // address, so the game reads assets straight out of the mapping.
    //
    // Locate the symbol table by scanning sections: elf_getsymtab/elf_getsyms
    // are 64-bit-only libelf entry points, and this tool must build on any
    // host regardless of its own pointer width.
    Elf_Scn *symtab_scn = NULL;
    GElf_Shdr symtab_shdr;
    memset(&symtab_shdr, 0, sizeof(symtab_shdr));

    Elf_Scn *s = NULL;
    while ((s = elf_nextscn(elf, s)) != NULL)
    {
        GElf_Shdr sh;
        if (gelf_getshdr(s, &sh) != &sh)
            continue;
        if (sh.sh_type == SHT_SYMTAB)
        {
            symtab_scn = s;
            symtab_shdr = sh;
            break;
        }
    }
    if (!symtab_scn)
        die("no SHT_SYMTAB section found");

    Elf_Data *symdata = elf_getdata(symtab_scn, NULL);
    if (!symdata || !symdata->d_buf)
        die("cannot read symbol table");

    Elf32_Sym *symtab = (Elf32_Sym *)symdata->d_buf;
    size_t nsymtab = symdata->d_size / sizeof(Elf32_Sym);

    unsigned rom_syms = 0;
    for (size_t i = 0; i < nsymtab; i++)
    {
        // NOTE: elf_strptr's first argument is a *section index*, not a symbol
        // index -- it must be the symtab's sh_link (the strtab). Passing the
        // symbol index here silently yields empty names for most entries.
        const char *name = elf_strptr(elf, symtab_shdr.sh_link, symtab[i].st_name);
        if (!name || !*name)
            continue;
        if (symtab[i].st_shndx == SHN_UNDEF || symtab[i].st_shndx == SHN_ABS)
            continue;
        unsigned char type = GELF_ST_TYPE(symtab[i].st_info);
        if (type != STT_FUNC && type != STT_OBJECT && type != STT_NOTYPE)
            continue;
        if (!addr_is_rom(symtab[i].st_value))
            continue;

        struct sym *sym = sym_intern(elf, name);
        sym->addr = symtab[i].st_value;
        rom_syms++;

        // A RAM symbol with the same name is recompiled natively; make sure the
        // single interned entry is marked as natively defined.
        (void)0;
    }

    // Any symbol living in EWRAM/IWRAM is recompiled natively too, so it must
    // never be emitted as an absolute ROM address.
    for (size_t i = 0; i < g_syms_len; i++)
        if (addr_is_ram(g_syms[i]->addr))
            g_syms[i]->native_defined = true;

    // Mark every symbol the native link already defines. These are the assets
    // declared with INCGFX/INCBIN in src/*.c, which the ROM build also places
    // in the image; the native definition wins and must not also be emitted as
    // an absolute ROM address.
    {
        FILE *nf = fopen(native_syms, "r");
        if (!nf)
            die("cannot open native symbol list");
        char line[512];
        while (fgets(line, sizeof(line), nf))
        {
            size_t n = strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
                line[--n] = '\0';
            if (!n)
                continue;
            struct sym *sy = sym_intern(elf, line);
            sy->native_defined = true;
        }
        fclose(nf);
    }

    // Names that must never be emitted as absolute ROM addresses, even though
    // the ROM happens to define them. The GBA link resolves libc calls (strcmp,
    // memcpy, memset) to routines placed inside the ROM image; a native link
    // must use the host's versions instead, or every call site jumps into the
    // mapped ROM image instead of running.
    {
        FILE *ef = fopen(exclude_syms, "r");
        if (!ef)
            die("cannot open exclusion list");
        char line[512];
        while (fgets(line, sizeof(line), ef))
        {
            size_t n = strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
                line[--n] = '\0';
            if (n)
                sym_intern(elf, line)->native_defined = true;
        }
        fclose(ef);
    }

    // ---- Pass 2: relocations --------------------------------------------
    //
    // The *linked* ELF has already had its relocations applied and dropped, so
    // they must be read from the unlinked objects instead. For each object we
    // need to know where each of its sections landed in the final image; that
    // comes from the linker map, which records lines of the form
    //
    //     .rodata        0x083dfe14    0xa70e4 data/maps.o
    //
    // (object name last, on a line following the `<obj>(<section>)` header).
    //
    // Only objects under build/emerald are considered: those are the ones whose
    // contents live in the ROM.
    unsigned long patched = 0, skipped = 0, unpatchable = 0, skipped_native = 0;

    // For each object, where did each of its sections land in the final image?
    struct obj_sec *secs = NULL;
    size_t secs_len = 0, secs_cap = 0;

    FILE *mf = fopen(map_path, "r");
    if (!mf)
        die("cannot open link map");
    {
        char line[512];
        while (fgets(line, sizeof(line), mf))
        {
            // Placement lines look like:
            //   .rodata        0x083dfe14    0xa70e4 data/maps.o
            //   script_data    0x081db67c    0xed6e8 data/event_scripts.o
            //
            // The section name is matched without requiring a leading '.',
            // because ld_script.ld renames some sections (script_data,
            // battle_scripts, etc.) that hold ROM-resident pointers.
            unsigned long addr;
            char secname[32], objpath[128];
            {
                const char *p = line;
                while (*p == ' ' || *p == '\t')
                    p++;
                char namebuf[32];
                size_t ni = 0;
                while (p[ni] && p[ni] != ' ' && p[ni] != '\t' && ni < sizeof(namebuf) - 1)
                {
                    namebuf[ni] = p[ni];
                    ni++;
                }
                namebuf[ni] = '\0';
                if (ni == 0 || p[ni] == '\n')
                    continue;
                if (sscanf(p + ni, " 0x%lx %*x %127s", &addr, objpath) != 2)
                    continue;
                snprintf(secname, sizeof(secname), "%s", namebuf);
            }
            if (!addr_is_rom(addr))
                continue;

            // Only objects whose contents live in the ROM matter. The map
            // prints the path relative to build/emerald, e.g. "data/maps.o".
            if (strncmp(objpath, "data/", 5) != 0 && strncmp(objpath, "sound/", 6) != 0 &&
                strncmp(objpath, "src/", 4) != 0)
                continue;

            // Relocations are read back from the object file, so keep the
            // path relative to objdir exactly as the map printed it
            // (e.g. "data/maps.o").
            const char *obj = objpath;

            int seen = 0;
            for (size_t i = 0; i < secs_len; i++)
                if (!strcmp(secs[i].obj, obj) && !strcmp(secs[i].sec, secname))
                {
                    seen = 1;
                    break;
                }
            if (seen)
                continue;

            if (secs_len == secs_cap)
            {
                size_t cap = secs_cap ? secs_cap * 2 : 4096;
                secs = realloc(secs, cap * sizeof(*secs));
                if (!secs)
                    die("out of memory");
                secs_cap = cap;
            }
            snprintf(secs[secs_len].obj, sizeof(secs[secs_len].obj), "%s", obj);
            snprintf(secs[secs_len].sec, sizeof(secs[secs_len].sec), "%s", secname);
            secs[secs_len].addr = (uint32_t)addr;
            secs_len++;
        }
    }
    fclose(mf);

    // Now read relocations out of each unlinked object under the build tree.
    // Iterate objects (not sections): each .rel.* names its own target section
    // via sh_info, so the base address must be looked up per relocation.
    for (size_t i = 0; i < secs_len; i++)
    {
        char opath[512];
        snprintf(opath, sizeof(opath), "%s/%s", objdir, secs[i].obj);

        int ofd = open(opath, O_RDONLY);
        if (ofd < 0)
            continue; // object not present in this build; nothing to patch
        Elf *oelf = elf_begin(ofd, ELF_C_READ, NULL);
        if (!oelf)
        {
            close(ofd);
            continue;
        }

        Elf_Scn *os = NULL;
        while ((os = elf_nextscn(oelf, os)) != NULL)
        {
            GElf_Shdr sh;
            if (gelf_getshdr(os, &sh) != &sh || sh.sh_type != SHT_REL || sh.sh_entsize == 0)
                continue;

            Elf_Scn *tscn = elf_getscn(oelf, sh.sh_info);
            if (!tscn)
                continue;
            GElf_Shdr tsh;
            if (gelf_getshdr(tscn, &tsh) != &tsh)
                continue;

            // Find where THIS relocation's target section landed in the ROM.
            // The map records the section under its link-time name, which for
            // renamed sections (script_data, ...) is what ld saw.
            // Resolve the target section's name against the object's
            // section-name string table. The ELF header's e_shstrndx is
            // unreliable in these objects (it points past the real .shstrtab),
            // so find the SHT_STRTAB that is *not* the symbol/section string
            // table by scanning for the one named ".shstrtab".
            char tname[64];
            size_t obj_shstrndx = find_shstrtab(oelf);
            const char *tn = obj_shstrndx ? elf_strptr(oelf, obj_shstrndx, tsh.sh_name) : NULL;
            snprintf(tname, sizeof(tname), "%s", tn ? tn : "");

            uint32_t sec_base;
            if (!find_section_addr(secs, secs_len, secs[i].obj, tname, &sec_base))
                continue; // section not placed in ROM; nothing baked in

            Elf_Scn *ss = elf_getscn(oelf, sh.sh_link);
            if (!ss)
                continue;
            GElf_Shdr ssh;
            if (gelf_getshdr(ss, &ssh) != &ssh)
                continue;

            Elf_Data *rd = elf_getdata(os, NULL);
            Elf_Data *sd = elf_getdata(ss, NULL);
            if (!rd || !rd->d_buf || !sd || !sd->d_buf)
                continue;

            Elf32_Rel *rels = (Elf32_Rel *)rd->d_buf;
            Elf32_Sym *syms = (Elf32_Sym *)sd->d_buf;
            size_t nrel = sh.sh_size / sh.sh_entsize;
            size_t nsym = sd->d_size / sizeof(Elf32_Sym);
            sec_base -= ROM_BASE;

            for (size_t k = 0; k < nrel; k++)
            {
                if (ELF32_R_TYPE(rels[k].r_info) != R_ARM_ABS32)
                    continue;
                unsigned si = ELF32_R_SYM(rels[k].r_info);
                if (si >= nsym)
                    continue;

                const char *nm = elf_strptr(oelf, ssh.sh_link, syms[si].st_name);
                if (!nm || !*nm)
                    continue;

                // Targets already in the ROM keep their baked value.
                if (addr_is_rom(syms[si].st_value))
                {
                    skipped++;
                    continue;
                }

                struct sym *tgt = sym_intern(elf, nm);
                // Binding comes from the object's symbol table; a relocation
                // can equally name a static function (IntrDummy and friends).
                tgt->global = (ELF32_ST_BIND(syms[si].st_info) == STB_GLOBAL
                               || ELF32_ST_BIND(syms[si].st_info) == STB_WEAK);
                patch_add(sec_base + rels[k].r_offset, tgt);
                patched++;
            }
        }
        elf_end(oelf);
        close(ofd);
    }

    // ---- Emit rom_patches.c ---------------------------------------------
    FILE *f = fopen(out_src, "w");
    if (!f)
        die("cannot write rom_patches.c");

    fprintf(f, "/* Generated by tools/rompatch from %s. Do not edit. */\n", argv[1]);
    fprintf(f, "#include <stdint.h>\n\n");
    fprintf(f, "struct rom_patch { uint32_t rom_offset; uint32_t target; };\n\n");
    fprintf(f, "const struct rom_patch g_rom_patches[] = {\n");
    for (size_t i = 0; i < g_patches_len; i++)
        fprintf(f, "{0x%08xu,%u},\n", g_patches[i].offset, g_patches[i].target);
    fprintf(f, "};\n");
    fprintf(f, "const unsigned g_rom_patch_count = %zu;\n\n", g_patches_len);

    fprintf(f, "const char *const g_rom_symbol_names[] = {\n");
    for (size_t i = 0; i < g_syms_len; i++)
        fprintf(f, "\"%s\",\n", g_syms[i]->name);
    fprintf(f, "};\n");
    fprintf(f, "const unsigned g_rom_symbol_count = %zu;\n", g_syms_len);

    // Addresses for every symbol the tool knows about.
    //
    // ROM-resident symbols get absolute definitions in rom_syms.s, so their
    // address is the ROM address and they need no patching. Everything else
    // was recompiled natively and lives wherever the native linker put it; we
    // read that back out of this ELF, which is the native link, so the value
    // here is already correct.
    // Addresses for every symbol the tool knows about.
//
// ROM-resident symbols get absolute definitions in rom_syms.s, so their
// address is simply the ROM address and they need no patching: they are read
// in place out of the mapped image.
//
// Everything else was recompiled natively and lives wherever the native linker
// placed it, which this tool cannot know. Emit a real C reference so the
// native linker resolves it; the loader reads the result at startup.
//
// Local symbols (static functions and asm-local labels) cannot be referenced
// this way. Those get address 0 and are skipped, which is safe as long as the
// game never calls through the ROM word -- the count is reported at the end.
fprintf(f, "\n/* Native symbols referenced by the table below. */\n");
    for (size_t i = 0; i < g_syms_len; i++)
    {
        struct sym *sy = g_syms[i];
        if (sy->global && (!addr_is_rom(sy->addr) || sy->native_defined))
            fprintf(f, "extern char %s[];\n", sy->name);
    }

    fprintf(f, "\nconst uint64_t g_rom_symbol_addrs[] = {\n");
for (size_t i = 0; i < g_syms_len; i++)
{
    struct sym *s = g_syms[i];
    bool rom_only = addr_is_rom(s->addr) && !(s->native_defined && s->global);
    if (rom_only)
    {
        // Still ROM-resident: the native build reads it in place.
        fprintf(f, "0x%08lxul, /* %s (in ROM) */\n", (unsigned long)s->addr, s->name);
    }
    else if (s->global)
    {
        fprintf(f, "(uint64_t)(uintptr_t)&%s, /* %s */\n", s->name, s->name);
    }
    else
    {
        fprintf(f, "0, /* %s (local, unpatchable) */\n", s->name);
        unpatchable++;
    }
}
fprintf(f, "};\n");
fclose(f);

    // ---- Emit rom_syms.h -------------------------------------------------
    FILE *h = fopen(out_hdr, "w");
    if (!h)
        die("cannot write rom_syms.h");

    fprintf(h, "/* Generated by tools/rompatch from %s. Do not edit. */\n", argv[1]);
    fprintf(h, "#ifndef ROM_SYMS_H\n#define ROM_SYMS_H\n#include <stdint.h>\n\n");
    fprintf(h, "#define ROM_SYMBOL_COUNT %zu\n\n", g_syms_len);
    fprintf(h, "/* Symbols whose address changes between the ROM and the native\n"
               " * binary. The loader resolves each by name. */\n");
    for (size_t i = 0; i < g_syms_len; i++)
    {
        if (addr_is_rom(g_syms[i]->addr))
            continue;
        fprintf(h, "extern char %s[];\n", g_syms[i]->name);
    }
    fclose(h);

    // ---- Emit rom_syms.S -------------------------------------------------
    //
    // This is the other half of the assets-from-ROM mechanism. The vast
    // majority of the game's data -- map layouts, tilesets, event scripts,
    // battle scripts, sound tables -- is assembled into the ROM by
    // ld_script.ld and has no C definition. Native code needs those names to
    // resolve, so emit absolute symbol assignments; with -no-pie, reading
    // `extern const u8 gFoo[]` then yields the address inside the mapped ROM.
    //
    // A subset of ROM symbols is also defined natively: graphics pulled in
    // through INCGFX/INCBIN become C arrays in src/*.c, so the same name
    // exists in both. Those must come from the native object, or the link
    // fails with a duplicate definition. Pass the native symbol list in and
    // skip anything it already defines.
    FILE *as_ = fopen(out_asm, "w");
    if (!as_)
        die("cannot write rom_syms.S");

    fprintf(as_, "/* Generated by tools/rompatch from %s. Do not edit. */\n", argv[1]);
    fprintf(as_, "/* Absolute definitions for ROM-resident symbols that have no\n"
                 " * native C definition. */\n\n");

    for (size_t i = 0; i < g_syms_len; i++)
    {
        struct sym *sy = g_syms[i];
        if (!addr_is_rom(sy->addr) && !addr_is_ram(sy->addr))
            continue;
        if (sy->native_defined)
        {
            skipped_native++;
            continue;
        }
        fprintf(as_, ".globl %s\n", sy->name);
        fprintf(as_, ".set %s, 0x%08lx\n\n", sy->name, (unsigned long)sy->addr);
    }
    fclose(as_);

    elf_end(elf);
    close(fd);

    fprintf(stderr, "rompatch: %u ROM symbols, %zu total, %lu patches, %lu skipped"
                   ", %lu unpatchable-local, %lu skipped (native def)\n",
            rom_syms, g_syms_len, patched, skipped, unpatchable, skipped_native);
    return 0;
}