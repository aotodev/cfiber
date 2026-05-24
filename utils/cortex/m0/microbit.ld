/*==============================================================================
 * Linker script for BBC micro:bit v1 board with nRF51822 SoC (Cortex-M0)
 *==============================================================================*/

MEMORY
{
    FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 256K
    RAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 16K
}

ENTRY(_start)

SECTIONS
{
    .text :
    {
        KEEP(*(.isr_vector))
        *(.text*)
        *(.rodata*)
    } > FLASH

    _data_load_start = LOADADDR(.data);

    .data : ALIGN(4) /* Ensure the whole section starts on a 4-byte boundary */
    {
        _sdata = .;
        *(.data*)
        . = ALIGN(4); /* Pad the section to a 4-byte boundary */
        _edata = .;
    } > RAM AT > FLASH

    .bss : ALIGN(4) /* Ensure the whole section starts on a 4-byte boundary */
    {
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4); /* Pad the section to a 4-byte boundary */
        _ebss = .;
    } > RAM

    _end = .; /* Symbol marking the end of initialized/uninitialized data */

    /* Define the boundary for the heap */
    _heap_start = _end;
    _heap_end = ORIGIN(RAM) + LENGTH(RAM); /* End of RAM */

    _stack_top = ORIGIN(RAM) + LENGTH(RAM);
}
