/**
 * System call stubs for embedded systems
 *
 * The ones we actually need to properly implement are:
 * - sbrk For dynamic allocation
 * - write To print to the tty
 *
 * The other ones are required by newlib but not used in our fiber test/sample
 */

#ifdef __arm__

#include <errno.h>
#include <stdint.h>
#include <sys/types.h>

/* Function to get the current stack pointer */
static inline char* get_stack_pointer(void) {
    char* sp;
    __asm__ volatile("mov %0, sp"
                     : "=r"(sp));
    return sp;
}

/** End of BSS, start of heap. */
extern char _ebss;

/** Tracks the current end of the heap. */
static char* current_break = nullptr;

void* _sbrk(ptrdiff_t increment) {
    char* current_stack_ptr = get_stack_pointer();

    if (!current_break) {
        current_break = &_ebss;
    }

    char* previous_break = current_break;

    /* Is the new top of the heap going to cross the current bottom of the stack? */
    if ((current_break + increment) > current_stack_ptr) {
        /* Maybe: Trigger a hard fault; this is a fatal error. */
        errno = ENOMEM;
        return (void*)-1;
    }

    current_break += increment;
    return (void*)previous_break;
}

void _exit(int status) {
    /* Use ARM semihosting to exit QEMU cleanly.
       Prefer SYS_EXIT_EXTENDED (0x20) so we can propagate the status code.
       Fallback: if semihosting is unavailable, park the core. */
    struct {
        uint32_t reason;
        uint32_t value;
    } args = {0x20026u /* ADP_Stopped_ApplicationExit */, (uint32_t)status};

    __asm__ volatile("mov r0, #0x20\n" /* SYS_EXIT_EXTENDED */
                     "mov r1, %0\n"    /* pointer to args */
                     "bkpt 0xAB\n"
                     :
                     : "r"(&args)
                     : "r0", "r1", "memory");

    /* Fallback if semihosting disabled */
    while (1) {
        __asm__("wfi");
    }
}

int _close(int file) {
    (void)file;
    return -1;
}

int _fstat(int file, void* st) {
    (void)file;
    (void)st;
    return 0;
}

int _isatty(int file) {
    (void)file;
    return 1;
}

int _lseek(int file, int ptr, int dir) {
    (void)file;
    (void)ptr;
    (void)dir;
    return 0;
}

int _read(int file, char* ptr, int len) {
    (void)file;
    (void)ptr;
    (void)len;
    return 0;
}

int _write(int file, char* ptr, int len) {
    (void)file;

    /* Use ARM semihosting to actually output to console */
    for (int i = 0; i < len; i++) {
        /* ARM semihosting call to write character */
        __asm__ volatile(     //
            "mov r0, #0x03\n" // SYS_WRITEC
            "mov r1, %0\n"    // pointer to character
            "bkpt #0xAB\n"    // semihosting breakpoint
            :
            : "r"(ptr + i)
            : "r0", "r1");
    }

    return len;
}

int _kill(int pid, int sig) {
    (void)pid;
    (void)sig;
    return -1;
}

int _getpid(void) {
    return 1;
}

#endif
