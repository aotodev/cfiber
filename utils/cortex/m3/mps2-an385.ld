/*==============================================================================
 * Linker script for the Arm MPS2+ AN385 platform (Cortex-M3)
 * Leverages ITCM for code and DTCM for data.
 *==============================================================================*/

MEMORY
{
    ITCM (rx) : ORIGIN = 0x00000000, LENGTH = 1M   /* Instruction TCM for code */
    DTCM (rwx): ORIGIN = 0x20000000, LENGTH = 1M   /* Data TCM for data/stack/heap */
}

ENTRY(_start)

SECTIONS
{
    /* The .text section, containing code and read-only data, goes into ITCM */
    .text :
    {
        KEEP(*(.isr_vector))
        *(.text*)
        *(.rodata*)
    } > ITCM

    /* LMA of .data is the end of .text in ITCM */
    _data_load_start = LOADADDR(.data);

    /* The .data section (VMA) goes into the start of DTCM */
    .data : ALIGN(4)
    {
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > DTCM AT > ITCM /* VMA in DTCM, LMA in ITCM */

    /* The .bss section follows .data in DTCM */
    .bss : ALIGN(4)
    {
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > DTCM

    _end = .;

    /* Stack grows down from the top of DTCM */
    _stack_top = ORIGIN(DTCM) + LENGTH(DTCM);
}
