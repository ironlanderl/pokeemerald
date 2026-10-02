/* Native linker script for the pokeemerald Linux port.
 *
 * Native code is placed at 0x40000000: above the emulated GBA window
 * (0x02000000..0x09000000), so it cannot collide with the mmap'd EWRAM, IWRAM,
 * VRAM, OAM, palette RAM, ROM or flash regions.
 *
 * The whole image must stay below 4 GiB. ROM-resident data stores absolute
 * 32-bit pointers to native symbols -- gMPlayTable holds &gMPlayInfo_BGM, for
 * example -- so anything above 0x100000000 truncates to garbage when read back
 * as a ROM word. The load image is roughly 15 MiB, so 0x40000000 leaves ample
 * room and still clears the GBA window.
 */
ENTRY(_start)

/* Keep the standard interpreter/CRT relationship: we link libc normally and
 * only replace the entry symbol, so the dynamic loader behaves normally. */

/* Defined in ld_script.ld for the ROM build; m4a reads them. */
gNumMusicPlayers = 4;
gMaxLines = 0;
SECTIONS
{
    . = 0x40000000;
    /* Scrt1.o supplies _start, which calls main(); main is this port's. */
    .text : { *(.text.startup) *(.init) *(.text .text.*) }

    /* The dynamic-linker sections must be laid out exactly as the default
     * script does. R_AARCH64_JUMP_SLOT relocations record offsets into .got.plt,
     * and a hand-rolled layout leaves ld emitting PLT stubs that index the wrong
     * slot -- which showed up as mmap() calling fopen() with its arguments. */
    .plt : { *(.plt .plt.*) *(.iplt) }
    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) *(.gnu.linkonce.r.*) }
    . = ALIGN(4096);
    /* .got must be writable and page-aligned: the dynamic loader writes
     * resolved addresses into it at load time. Folding it into .data made the
     * GOT land in the read-only RELRO region after relocation, so lazy PLT
     * resolution silently failed and calls reached the wrong symbol. */
    /* .rela.plt must exist as its own section: it is what JMPREL points at,
     * and it is where the 70 R_AARCH64_JUMP_SLOT relocations live. Letting it
     * merge into .rela.dyn left the loader with no PLT relocation table, so
     * PLT slots were never filled in and calls landed on whatever the stub
     * happened to point at. */
    .rela.dyn : { *(.rela.dyn) }
    .rela.plt : { *(.rela.plt) }
    .got : { *(.got .got.plt) }
    . = ALIGN(4096);
    .data : { *(.data .data.*) *(.ewram_data) *(.common_data) }
    . = ALIGN(4096);
    .bss : { *(.bss .bss.*) *(.bss.code) *(COMMON) }
    /DISCARD/ : { *(.ARM.attributes) *(.comment) }
}
