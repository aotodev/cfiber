/* BBC micro:bit v1, nRF51822 (Cortex-M0): 256K flash, 16K RAM. */

MEMORY
{
    FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 256K
    RAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 16K
}

/* Main stack at the top of RAM; fiber stacks come from the heap. */
_main_stack_size = 2K;

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

    .data : ALIGN(4)
    {
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    .bss (NOLOAD) : ALIGN(4)
    {
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    _heap_start = _ebss;
    _stack_top = ORIGIN(RAM) + LENGTH(RAM);
    _stack_bottom = _stack_top - _main_stack_size;
    _heap_end = _stack_bottom;

    ASSERT(_heap_start <= _heap_end, "no RAM left for the heap")
}
