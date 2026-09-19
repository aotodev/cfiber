/**
 * @file  test_growable_stack.c
 * @brief Unit tests for MMU-backed growable stacks and their pooled allocator.
 *
 * @details
 * Growable stacks are mmap'd with a PROT_NONE guard page at the bottom and a
 * lazily-populated (MAP_NORESERVE) usable region above it. These tests verify:
 *   - the descriptor layout returned by cfiber_growable_stack_create();
 *   - that the whole usable region is writable (demand paging "grows" it);
 *   - that recycling releases pages but leaves the stack usable;
 *   - that the bottom guard page faults on overflow (caught via a SIGSEGV
 *     handler + siglongjmp);
 *   - pooled alloc/release/reuse and clean destroy.
 *
 * Hosted Linux/Unix only: the growable stack sources are not built for
 * freestanding targets. The guard-page fault test is skipped under ASan, whose
 * own SIGSEGV handling would interfere with the test's handler.
 */

#include "cfiber/stack/growable_stack.h"
#include "cfiber/stack/growable_stack_allocator.h"
#include "cfiber/stack/stack.h"
#include "test/test.h"

#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static size_t page_size(void) {
    const long ps = sysconf(_SC_PAGESIZE);
    return ps > 0 ? (size_t)ps : 4096u;
}

/* ============================================================================
 * Low-level growable stack
 * ============================================================================ */

static int test_growable_create_layout(void) {
    const size_t max = page_size() * 16;
    cfiber_stack_t s = cfiber_growable_stack_create(max);

    ASSERT_TRUE(cfiber_stack_is_valid(&s));
    /* total VMA = requested size + one bottom guard page */
    ASSERT_EQ_U64(s.total_size, max + page_size());
    ASSERT_EQ_PTR(s.stack_top, (char*)s.mem_base + s.total_size);
    /* the usable stack starts above the guard page and is exactly max */
    ASSERT_EQ_PTR(s.usable_base, (char*)s.mem_base + page_size());
    ASSERT_EQ_U64(cfiber_stack_usable_size(&s), max);

    cfiber_growable_stack_destroy(&s);
    ASSERT_NULL(s.mem_base);
    ASSERT_NULL(s.usable_base);
    ASSERT_NULL(s.stack_top);
    ASSERT_EQ_U64(s.total_size, 0);
    return 0;
}

static int test_growable_create_rejects_bad_sizes(void) {
    const size_t ps = page_size();

    errno = 0;
    cfiber_stack_t zero = cfiber_growable_stack_create(0);
    ASSERT_FALSE(cfiber_stack_is_valid(&zero));
    ASSERT_EQ_U32(errno, EINVAL);

    errno = 0;
    cfiber_stack_t odd = cfiber_growable_stack_create(ps + 1);
    ASSERT_FALSE(cfiber_stack_is_valid(&odd));
    ASSERT_EQ_U32(errno, EINVAL);

    errno = 0;
    cfiber_stack_t huge = cfiber_growable_stack_create(SIZE_MAX - ps + 1); /* + guard page would wrap */
    ASSERT_FALSE(cfiber_stack_is_valid(&huge));
    ASSERT_EQ_U32(errno, EINVAL);
    return 0;
}

static int test_growable_usable_region_writable(void) {
    const size_t ps = page_size();
    cfiber_stack_t s = cfiber_growable_stack_create(ps * 8);
    ASSERT_TRUE(cfiber_stack_is_valid(&s));

    /* usable region begins just above the guard page */
    uint8_t* const bottom = (uint8_t*)s.mem_base + ps;
    uint8_t* const top = (uint8_t*)s.stack_top;

    /* touch one byte in every usable page: each access faults in a fresh
     * physical page (demand paging). This is how the stack "grows" */
    for (uint8_t* p = bottom; p < top; p += ps) {
        *p = 0x5A;
    }
    top[-1] = 0x5A;

    bool all_set = true;
    for (uint8_t* p = bottom; p < top; p += ps) {
        if (*p != 0x5A) {
            all_set = false;
        }
    }
    ASSERT_TRUE(all_set);
    ASSERT_EQ_U32(top[-1], 0x5A);

    cfiber_growable_stack_destroy(&s);
    return 0;
}

static int test_growable_recycle_keeps_stack_usable(void) {
    const size_t ps = page_size();
    cfiber_stack_t s = cfiber_growable_stack_create(ps * 8);
    ASSERT_TRUE(cfiber_stack_is_valid(&s));

    uint8_t* const bottom = (uint8_t*)s.mem_base + ps;
    uint8_t* const top = (uint8_t*)s.stack_top;

    memset(bottom, 0x11, (size_t)(top - bottom));

    const size_t size_before = s.total_size;
    void* const base_before = s.mem_base;
    cfiber_growable_stack_recycle(&s);

    /* recycle releases physical pages but keeps the VMA / descriptor intact */
    ASSERT_TRUE(cfiber_stack_is_valid(&s));
    ASSERT_EQ_U64(s.total_size, size_before);
    ASSERT_EQ_PTR(s.mem_base, base_before);

    /* still writable afterwards (pages fault back in) */
    bottom[0] = 0x33;
    top[-1] = 0x44;
    ASSERT_EQ_U32(bottom[0], 0x33);
    ASSERT_EQ_U32(top[-1], 0x44);

    cfiber_growable_stack_destroy(&s);
    return 0;
}

/* ---- guard-page fault detection ---- */

#ifndef __SANITIZE_ADDRESS__
static sigjmp_buf g_fault_jmp;
static volatile sig_atomic_t g_fault_caught;

static void fault_handler(int signo) {
    (void)signo;
    g_fault_caught = 1;
    siglongjmp(g_fault_jmp, 1);
}

static int test_growable_guard_page_faults(void) {
    cfiber_stack_t s = cfiber_growable_stack_create(page_size() * 4);
    ASSERT_TRUE(cfiber_stack_is_valid(&s));

    struct sigaction sa = {0};
    struct sigaction old_segv;
    struct sigaction old_bus;
    sa.sa_handler = fault_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGBUS, &sa, &old_bus);

    g_fault_caught = 0;
    if (sigsetjmp(g_fault_jmp, 1) == 0) {
        /* writing to the bottom guard page (PROT_NONE) must fault */
        volatile uint8_t* guard = (volatile uint8_t*)s.mem_base;
        *guard = 0x01;
        /* unreachable if the guard works */
    }

    sigaction(SIGSEGV, &old_segv, nullptr);
    sigaction(SIGBUS, &old_bus, nullptr);

    ASSERT_EQ_U32((uint32_t)g_fault_caught, 1);

    cfiber_growable_stack_destroy(&s);
    return 0;
}
#endif /* !__SANITIZE_ADDRESS__ */

/* ============================================================================
 * Pooled growable stack allocator
 * ============================================================================ */

static int test_pool_alloc_release_destroy(void) {
    const cfiber_growable_stack_allocator_args_t args = {
        .max_stack_size = page_size() * 8,
        .cache_capacity = 4,
        .initial_cached = 2,
    };
    cfiber_growable_stack_allocator_t* alloc = cfiber_growable_stack_allocator_create(args);
    ASSERT_NOT_NULL(alloc);

    cfiber_stack_t s1 = cfiber_growable_stack_alloc(alloc);
    cfiber_stack_t s2 = cfiber_growable_stack_alloc(alloc);
    ASSERT_TRUE(cfiber_stack_is_valid(&s1));
    ASSERT_TRUE(cfiber_stack_is_valid(&s2));
    ASSERT_NE_PTR(s1.mem_base, s2.mem_base);

    cfiber_growable_stack_release(alloc, &s1);
    /* release clears the caller's descriptor */
    ASSERT_NULL(s1.mem_base);
    cfiber_growable_stack_release(alloc, &s2);

    /* no outstanding stacks: destroy reports success */
    ASSERT_EQ_U32((uint32_t)cfiber_growable_stack_allocator_destroy(alloc), 0);
    return 0;
}

/* Releasing into a full cache unmaps the stack instead of pooling it. */
static int test_pool_full_cache_destroys(void) {
    const cfiber_growable_stack_allocator_args_t args = {
        .max_stack_size = page_size() * 4,
        .cache_capacity = 1,
        .initial_cached = 0,
    };
    cfiber_growable_stack_allocator_t* alloc = cfiber_growable_stack_allocator_create(args);
    ASSERT_NOT_NULL(alloc);

    cfiber_stack_t a = cfiber_growable_stack_alloc(alloc);
    cfiber_stack_t b = cfiber_growable_stack_alloc(alloc);
    ASSERT_TRUE(cfiber_stack_is_valid(&a));
    ASSERT_TRUE(cfiber_stack_is_valid(&b));

    cfiber_growable_stack_release(alloc, &a); /* cached */
    cfiber_growable_stack_release(alloc, &b); /* cache full: destroyed */
    ASSERT_NULL(b.mem_base);

    ASSERT_EQ_U32((uint32_t)cfiber_growable_stack_allocator_destroy(alloc), 0);
    return 0;
}

static int test_pool_recycles_cached_stack(void) {
    const cfiber_growable_stack_allocator_args_t args = {
        .max_stack_size = page_size() * 8,
        .cache_capacity = 2,
        .initial_cached = 0,
    };
    cfiber_growable_stack_allocator_t* alloc = cfiber_growable_stack_allocator_create(args);
    ASSERT_NOT_NULL(alloc);

    cfiber_stack_t s = cfiber_growable_stack_alloc(alloc);
    ASSERT_TRUE(cfiber_stack_is_valid(&s));
    void* const base = s.mem_base;

    /* releasing into a non-full cache keeps the stack for reuse (LIFO) */
    cfiber_growable_stack_release(alloc, &s);
    cfiber_stack_t reused = cfiber_growable_stack_alloc(alloc);
    ASSERT_EQ_PTR(reused.mem_base, base);

    cfiber_growable_stack_release(alloc, &reused);
    ASSERT_EQ_U32((uint32_t)cfiber_growable_stack_allocator_destroy(alloc), 0);
    return 0;
}

int main(void) {
    cfiber_test_suite_begin("growable stacks");

    RUN_TEST(test_growable_create_layout);
    RUN_TEST(test_growable_create_rejects_bad_sizes);
    RUN_TEST(test_growable_usable_region_writable);
    RUN_TEST(test_growable_recycle_keeps_stack_usable);
#ifndef __SANITIZE_ADDRESS__
    RUN_TEST(test_growable_guard_page_faults);
#endif
    RUN_TEST(test_pool_alloc_release_destroy);
    RUN_TEST(test_pool_full_cache_destroys);
    RUN_TEST(test_pool_recycles_cached_stack);

    return cfiber_test_report();
}
