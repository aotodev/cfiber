/* QEMU MPS2-AN500 (Cortex-M7): 4M flash at 0, 4M SRAM at 0x20000000. */

MEMORY
{
    FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 4M
    RAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 4M
}

/* Main stack at the top of RAM; fiber stacks come from the heap. */
_main_stack_size = 16K;

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

    /* 32: startup_qemu_armv7-m.S copies and zeroes in 8-word strides. */
    .data : ALIGN(32)
    {
        _sdata = .;
        *(.data*)
        . = ALIGN(32);
        _edata = .;
    } > RAM AT > FLASH

    .bss (NOLOAD) : ALIGN(32)
    {
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(32);
        _ebss = .;
    } > RAM

    _heap_start = _ebss;
    _stack_top = ORIGIN(RAM) + LENGTH(RAM);
    _stack_bottom = _stack_top - _main_stack_size;
    _heap_end = _stack_bottom;

    ASSERT(_heap_start <= _heap_end, "no RAM left for the heap")
}
