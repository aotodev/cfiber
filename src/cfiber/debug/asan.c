#include "cfiber/debug/asan.h"

/* The whole TU is empty unless the library is built with AddressSanitizer.
 * CFIBER_ASAN_ENABLED is driven by the compiler's __SANITIZE_ADDRESS__ macro,
 * which the CFIBER_ASAN build option turns on via -fsanitize=address. */
#if CFIBER_ASAN_ENABLED

/* Bounds of the host (scheduler) stack, discovered on first fiber entry. ASan
 * is hosted-only, so thread_local is always available here. */
static thread_local const void* tl_host_low = nullptr;
static thread_local size_t tl_host_size = 0;
static thread_local bool tl_host_known = false;

/* Set between a start_switch and its matching finish. Only cfiber_asan_switch
 * issues starts, so this lets the shared assembly prologue tell an annotated
 * (scheduler-driven) fiber entry apart from a raw switch_context() entry, where
 * issuing finish_switch_fiber would abort with "finishing a switch that has not
 * started". Cooperative scheduling means at most one start is ever outstanding
 * on a thread, so a single flag suffices. */
static thread_local bool tl_switch_pending = false;

/* These helpers themselves must not be ASan-instrumented: their job is to drive
 * ASan's fiber bookkeeping by hand, and instrumenting them would let the
 * compiler insert shadow accesses that reference a stack ASan does not yet
 * consider current (mid-switch), producing spurious reports. */
#define CFIBER_NO_ASAN __attribute__((no_sanitize_address, noinline))

CFIBER_NO_ASAN void cfiber_asan_switch(context_t* const from,
                                       context_t* const to,
                                       const void* const to_stack_low,
                                       const size_t to_stack_size,
                                       const bool finishing) {
    void* fake_stack = nullptr;

    /* Pass NULL as the save slot when the outgoing fiber is terminating so ASan
     * frees its fake stack instead of stashing it for a resume that will never
     * come. */
    tl_switch_pending = true;
    __sanitizer_start_switch_fiber(finishing ? nullptr : &fake_stack, to_stack_low, to_stack_size);

    switch_context(from, to);

    /* Reached only when this fiber is resumed (never for a terminating one,
     * whose switch_context above does not return). The party that switched back
     * into us issued the paired start; consume it and tell ASan the swap
     * completed. We are now back on `from`'s stack. */
    if (tl_switch_pending) {
        tl_switch_pending = false;
        __sanitizer_finish_switch_fiber(fake_stack, nullptr, nullptr);
    }
}

CFIBER_NO_ASAN void cfiber_asan_on_fiber_entry(void) {
    /* A fiber entered by a bare switch_context() (e.g. driven directly rather
     * than through the scheduler) had no paired start_switch_fiber; issuing a
     * finish here would abort. Only annotate scheduler-driven entries. */
    if (!tl_switch_pending) {
        return;
    }
    tl_switch_pending = false;

    const void* host_low = nullptr;
    size_t host_size = 0;

    /* Fresh fiber: no prior fake stack of our own to restore (NULL). The
     * out-params yield the bounds of the stack we were switched from — always
     * the scheduler (host) stack, since fibers are only entered from it. */
    __sanitizer_finish_switch_fiber(nullptr, &host_low, &host_size);

    if (!tl_host_known) {
        tl_host_low = host_low;
        tl_host_size = host_size;
        tl_host_known = true;
    }
}

void cfiber_asan_host_bounds(const void** const low, size_t* const size) {
    *low = tl_host_low;
    *size = tl_host_size;
}

#endif /* CFIBER_ASAN_ENABLED */
