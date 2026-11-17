/* Extension firmware: lives in the free flash above the stock application.
 *
 * Stock app occupies 0x08002800-0x0801F934 on the v2.1.2 image.
 * GD32F303CC flash is 256 KB (to 0x08040000) with 2 KB pages, so 0x08020000
 * is page-aligned and clear of the stock image.
 *
 * Stock RAM: initial SP 0x20005260, SCPI state through ~0x20004260.
 * We take the top of the 48 KB SRAM to stay well clear.
 */
INCLUDE stock_symbols.ld

MEMORY
{
  EXT_FLASH (rx) : ORIGIN = 0x08020000, LENGTH = 128K
  EXT_RAM  (rwx) : ORIGIN = 0x2000A000, LENGTH = 8K
}

ENTRY(ext_init)

SECTIONS
{
  .ext_text : {
    KEEP(*(.ext_entry))
    *(.text .text.*)
    *(.rodata .rodata.*)
    . = ALIGN(4);
  } > EXT_FLASH

  .ext_data : {
    . = ALIGN(4);
    __ext_data_start = .;
    *(.data .data.*)
    . = ALIGN(4);
    __ext_data_end = .;
  } > EXT_RAM AT > EXT_FLASH
  __ext_data_load = LOADADDR(.ext_data);

  .ext_bss (NOLOAD) : {
    . = ALIGN(4);
    __ext_bss_start = .;
    *(.bss .bss.*) *(COMMON)
    . = ALIGN(4);
    __ext_bss_end = .;
  } > EXT_RAM

  /DISCARD/ : { *(.ARM.exidx*) *(.comment) }
}
