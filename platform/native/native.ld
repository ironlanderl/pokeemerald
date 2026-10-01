/* Native linker script for the pokeemerald Linux port.
 *
 * Native code lives at 0x10000000, above the emulated GBA window
 * (0x02000000..0x09000000), so it cannot collide with the mmap'd EWRAM,
 * IWRAM, VRAM, OAM, palette RAM, ROM or flash regions.
 */
ENTRY(native_main)

/* Keep the standard interpreter/CRT relationship: we link libc normally and
 * only replace the entry symbol, so the dynamic loader behaves normally. */

/* Defined in ld_script.ld for the ROM build; m4a reads them. */
gNumMusicPlayers = 4;
gMaxLines = 0;
SECTIONS
{
    . = 0x10000000;
    .text : { *(.text.startup) *(.text .text.*) }
    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(4096);
    .data : { *(.data .data.*) *(.ewram_data) *(.common_data) }
    . = ALIGN(4096);
    .bss : { *(.bss .bss.*) *(.bss.code) *(COMMON) }
    /DISCARD/ : { *(.ARM.attributes) *(.comment) }
}
