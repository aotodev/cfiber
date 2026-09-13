/* Arm MPS2+ AN385 (Cortex-M3): code in ITCM, data, heap and stack in DTCM. */

MEMORY
{
    ITCM (rx)  : ORIGIN = 0x00000000, LENGTH = 1M
    DTCM (rwx) : ORIGIN = 0x20000000, LENGTH = 1M
}

/* Main stack at the top of DTCM; fiber stacks come from the heap. */
_main_stack_size = 16K;

ENTRY(_start)

SECTIONS
{
    .text :
    {
        KEEP(*(.isr_vector))
        *(.text*)
        *(.rodata*)
    } > ITCM

    _data_load_start = LOADADDR(.data);

    /* 32: startup_qemu_armv7-m.S copies and zeroes in 8-word strides. */
    .data : ALIGN(32)
    {
        _sdata = .;
        *(.data*)
        . = ALIGN(32);
        _edata = .;
    } > DTCM AT > ITCM

    .bss (NOLOAD) : ALIGN(32)
    {
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(32);
        _ebss = .;
    } > DTCM

    _heap_start = _ebss;
    _stack_top = ORIGIN(DTCM) + LENGTH(DTCM);
    _stack_bottom = _stack_top - _main_stack_size;
    _heap_end = _stack_bottom;

    ASSERT(_heap_start <= _heap_end, "no RAM left for the heap")
}
