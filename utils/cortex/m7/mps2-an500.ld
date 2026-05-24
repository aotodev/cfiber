/*==============================================================================
 * Linker script for QEMU MPS2‑AN500 (Cortex‑M7, 4 MiB FLASH, 4 MiB SRAM)
 *==============================================================================*/

ENTRY(_start)

/*----------------------------------------------------------------------------*/
/* Memory regions                                                             */
/*----------------------------------------------------------------------------*/
MEMORY
{
    /* QEMU maps FLASH at 0x00000000 and SRAM at 0x20000000 */
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 4M
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 4M
}

/*----------------------------------------------------------------------------*/
/* Section placement                                                          */
/*----------------------------------------------------------------------------*/
SECTIONS
{
    /*----------------------------------------------------------*
     * Vector table and code execute from FLASH
     *----------------------------------------------------------*/
    .text :
    {
      __text_start__ = .;
      KEEP(*(.isr_vector))
      *(.text*)
      *(.rodata*)
      KEEP(*(.init))
      KEEP(*(.fini))
      /* Exception unwinding tables */
      *(.ARM.extab* .gnu.linkonce.armextab.*)
      *(.ARM.exidx* .gnu.linkonce.armexidx.*)
      __text_end__ = .;
    } > FLASH
    /*----------------------------------------------------------*
     * Initialized data — load in FLASH, run in RAM
     *----------------------------------------------------------*/
    .data : AT(__text_end__)
    {
        __data_load_start = LOADADDR(.data);
        __data_start__ = .;
        *(.data*)
        *(.ramfunc*)                /* Optional: functions copied to RAM */
        __data_end__ = .;
    } > RAM

    /*----------------------------------------------------------*
     * Uninitialized data (zero‑fill in RAM)
     *----------------------------------------------------------*/
    .bss (NOLOAD) :
    {
        __bss_start__ = .;
        *(.bss*)
        *(COMMON)
        __bss_end__ = .;
    } > RAM

    /*----------------------------------------------------------*
     * Heap + stack region (top of RAM downward)
     *----------------------------------------------------------*/
    .stack (NOLOAD) :
    {
        . = ALIGN(8);
        __stack_bottom__ = .;
        . += 0x00001000;            /* 4 KiB stack – adjust as desired */
        __stack_top__ = .;
    } > RAM

    /* Symbols matching your startup expectations */
    _sdata           = __data_start__;
    _edata           = __data_end__;
    _data_load_start = __data_load_start;
    _sbss            = __bss_start__;
    _ebss            = __bss_end__;
    _stack_top       = __stack_top__;
}

/* End of file */
