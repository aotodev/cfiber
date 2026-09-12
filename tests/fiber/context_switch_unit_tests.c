/**
 * @file  context_switch_unit_tests.c
 * @brief Register preservation and entry-state tests for switch_context().
 *
 * @details
 * The register traffic is in cfiber_test_switch_regs() (switch_regs_<arch>.S):
 * load a pattern into every callee-saved register, switch_context(), store the
 * set on return. No compiler-generated code runs in between, so the result does
 * not depend on register allocation, optimisation level or sanitizer
 * instrumentation.
 *
 * Two fibers switch to each other through the helper, each with its own
 * pattern. While one is switched out, the other reads the callee-saved slots of
 * its context_t. That catches a save-side slot mix-up the load/store round trip
 * alone cannot: save r12 into r13's slot, restore r12 from r13's slot, and the
 * round trip still passes.
 *
 * Fibers only record into the fixture; main asserts once control is back.
 */

#include "cfiber/fiber/fiber.h"
#include "test/test.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Per-architecture callee-saved set. Slot order must match switch_regs_<arch>.S.
 * ============================================================================ */

#ifdef __x86_64__

#define REG_COUNT 6
#define STACK_SIZE 8192
#define ENTRY_SP_ALIGN 16
#define ENTRY_SP_OFFSET 8 /* return address pushed by call */

static const char* const reg_names[REG_COUNT] = {"rbx", "rbp", "r12", "r13", "r14", "r15"};

static uintptr_t ctx_sp(const context_t* ctx) {
    return ctx->rsp;
}

static void ctx_regs(const context_t* ctx, uintptr_t out[REG_COUNT]) {
    out[0] = ctx->rbx;
    out[1] = ctx->rbp;
    out[2] = ctx->r12;
    out[3] = ctx->r13;
    out[4] = ctx->r14;
    out[5] = ctx->r15;
}

#elifdef __aarch64__

#define REG_COUNT 19
#define STACK_SIZE 8192
#define ENTRY_SP_ALIGN 16
#define ENTRY_SP_OFFSET 0

static const char* const reg_names[REG_COUNT] = {"x19",
                                                 "x20",
                                                 "x21",
                                                 "x22",
                                                 "x23",
                                                 "x24",
                                                 "x25",
                                                 "x26",
                                                 "x27",
                                                 "x28",
                                                 "x29",
                                                 "d8",
                                                 "d9",
                                                 "d10",
                                                 "d11",
                                                 "d12",
                                                 "d13",
                                                 "d14",
                                                 "d15"};

static uintptr_t ctx_sp(const context_t* ctx) {
    return ctx->sp;
}

static uintptr_t bits(double d) {
    uintptr_t b;
    memcpy(&b, &d, sizeof b);
    return b;
}

static void ctx_regs(const context_t* ctx, uintptr_t out[REG_COUNT]) {
    out[0] = ctx->x19;
    out[1] = ctx->x20;
    out[2] = ctx->x21;
    out[3] = ctx->x22;
    out[4] = ctx->x23;
    out[5] = ctx->x24;
    out[6] = ctx->x25;
    out[7] = ctx->x26;
    out[8] = ctx->x27;
    out[9] = ctx->x28;
    out[10] = ctx->x29;
    out[11] = bits(ctx->v8);
    out[12] = bits(ctx->v9);
    out[13] = bits(ctx->v10);
    out[14] = bits(ctx->v11);
    out[15] = bits(ctx->v12);
    out[16] = bits(ctx->v13);
    out[17] = bits(ctx->v14);
    out[18] = bits(ctx->v15);
}

#elifdef __arm__

#ifdef CFIBER_ARM_FPU
#define REG_COUNT 24
#else
#define REG_COUNT 8
#endif
#define STACK_SIZE 1024
#define ENTRY_SP_ALIGN 8
#define ENTRY_SP_OFFSET 0

static const char* const reg_names[REG_COUNT] = {
    "r4",  "r5",  "r6",  "r7",  "r8",  "r9",  "r10", "r11",
#ifdef CFIBER_ARM_FPU
    "s16", "s17", "s18", "s19", "s20", "s21", "s22", "s23", "s24", "s25", "s26", "s27", "s28", "s29", "s30", "s31",
#endif
};

static uintptr_t ctx_sp(const context_t* ctx) {
    return ctx->sp;
}

#ifdef CFIBER_ARM_FPU
static uintptr_t bits(float f) {
    uintptr_t b;
    memcpy(&b, &f, sizeof b);
    return b;
}
#endif

static void ctx_regs(const context_t* ctx, uintptr_t out[REG_COUNT]) {
    out[0] = ctx->r4;
    out[1] = ctx->r5;
    out[2] = ctx->r6;
    out[3] = ctx->r7;
    out[4] = ctx->r8;
    out[5] = ctx->r9;
    out[6] = ctx->r10;
    out[7] = ctx->r11;
#ifdef CFIBER_ARM_FPU
    out[8] = bits(ctx->s16);
    out[9] = bits(ctx->s17);
    out[10] = bits(ctx->s18);
    out[11] = bits(ctx->s19);
    out[12] = bits(ctx->s20);
    out[13] = bits(ctx->s21);
    out[14] = bits(ctx->s22);
    out[15] = bits(ctx->s23);
    out[16] = bits(ctx->s24);
    out[17] = bits(ctx->s25);
    out[18] = bits(ctx->s26);
    out[19] = bits(ctx->s27);
    out[20] = bits(ctx->s28);
    out[21] = bits(ctx->s29);
    out[22] = bits(ctx->s30);
    out[23] = bits(ctx->s31);
#endif
}

#else
#error "Unsupported architecture"
#endif

/* ============================================================================
 * Assembly helpers (switch_regs_<arch>.S)
 * ============================================================================ */

/* Loads magic into the callee-saved set, switch_context(self, other), stores
 * the set into out once resumed. */
void cfiber_test_switch_regs(context_t* self,
                             context_t* other,
                             const uintptr_t magic[REG_COUNT],
                             uintptr_t out[REG_COUNT]);

/* Fiber entry point: tail-calls cfiber_test_fiber_main(user_data, entry sp). */
void cfiber_test_fiber_entry(void* user_data);

/* ============================================================================
 * Fixture
 * ============================================================================ */

enum step : uint8_t {
    STEP_TEST_ENTER = 1,
    STEP_INTERMEDIARY,
    STEP_TEST_RESUME,
};

typedef struct {
    context_t main_ctx;
    fiber_t test_fiber;
    fiber_t intermediary;

    enum step trace[4];
    size_t trace_len;

    uintptr_t entry_sp; /* test fiber sp at entry, before any prologue */

    /* a: held by the test fiber across its switch; b: by the intermediary. */
    uintptr_t magic_a[REG_COUNT];
    uintptr_t magic_b[REG_COUNT];
    uintptr_t out_a[REG_COUNT];   /* registers on return from the helper */
    uintptr_t out_b[REG_COUNT];   /* never filled: the intermediary is not resumed */
    uintptr_t saved_a[REG_COUNT]; /* context_t slots, read while switched out */
    uintptr_t saved_b[REG_COUNT];
    uintptr_t saved_sp_a;
    uintptr_t saved_sp_b;
} fixture;

static fixture fx;

static void trace_push(fixture* f, enum step s) {
    if (f->trace_len < sizeof f->trace / sizeof f->trace[0]) {
        f->trace[f->trace_len] = s;
    }
    f->trace_len++;
}

/* Full-width, per-slot distinct, not NaN as double or float. */
static void fill_pattern(uintptr_t out[REG_COUNT], uintptr_t seed) {
    for (size_t i = 0; i < REG_COUNT; i++) {
        out[i] = seed ^ i;
    }
}

/* The fibers never return: the test fiber switches back to main and the
 * intermediary is left parked. Registered to exercise the API only. */
static void noop_return_hook(void* ctx) {
    (void)ctx;
    __builtin_unreachable();
}

static bool setup_fiber(fiber_t* fiber, fiber_fn fn) {
    fiber->stack = malloc(STACK_SIZE);
    if (!fiber->stack) {
        return false;
    }
    fiber->stack_size = STACK_SIZE;
    memset(&fiber->ctx, 0, sizeof fiber->ctx);
    init_fiber(fiber, fn, &fx);
    return true;
}

static void teardown_fiber(fiber_t* fiber) {
    free(fiber->stack);
    fiber->stack = nullptr;
}

/* ============================================================================
 * Fibers: record only, assert from main.
 * ============================================================================ */

// NOLINTNEXTLINE(misc-use-internal-linkage): entered from switch_regs_<arch>.S
void cfiber_test_fiber_main(void* user_data, uintptr_t entry_sp) {
    fixture* f = user_data;
    f->entry_sp = entry_sp;
    trace_push(f, STEP_TEST_ENTER);

    cfiber_test_switch_regs(&f->test_fiber.ctx, &f->intermediary.ctx, f->magic_a, f->out_a);

    trace_push(f, STEP_TEST_RESUME);
    ctx_regs(&f->intermediary.ctx, f->saved_b);
    f->saved_sp_b = ctx_sp(&f->intermediary.ctx);

    switch_context(&f->test_fiber.ctx, &f->main_ctx);
}

static void intermediary_main(void* user_data) {
    fixture* f = user_data;
    trace_push(f, STEP_INTERMEDIARY);
    ctx_regs(&f->test_fiber.ctx, f->saved_a);
    f->saved_sp_a = ctx_sp(&f->test_fiber.ctx);

    cfiber_test_switch_regs(&f->intermediary.ctx, &f->test_fiber.ctx, f->magic_b, f->out_b);
    /* not resumed */
}

/* ============================================================================
 * Tests
 * ============================================================================ */

/* Runs the switches; the tests below inspect what the fibers recorded. */
static int test_round_trip(void) {
    fill_pattern(fx.magic_a, UINTPTR_MAX / 3);    /* 0x5555... */
    fill_pattern(fx.magic_b, ~(UINTPTR_MAX / 3)); /* 0xAAAA... */

    ASSERT_TRUE(setup_fiber(&fx.test_fiber, cfiber_test_fiber_entry));
    ASSERT_TRUE(setup_fiber(&fx.intermediary, intermediary_main));

    switch_context(&fx.main_ctx, &fx.test_fiber.ctx);
    return 0;
}

#define ASSERT_REGS_EQ(actual, expected)                                                                               \
    do {                                                                                                               \
        for (size_t i_ = 0; i_ < REG_COUNT; i_++) {                                                                    \
            if ((actual)[i_] != (expected)[i_]) {                                                                      \
                CFIBER_FAIL_("%s[%s] = %#" PRIxPTR ", expected %#" PRIxPTR,                                            \
                             #actual,                                                                                  \
                             reg_names[i_],                                                                            \
                             (actual)[i_],                                                                             \
                             (expected)[i_]);                                                                          \
            }                                                                                                          \
            cfiber_check_pass();                                                                                       \
        }                                                                                                              \
    } while (0)

/* Full-descending stack: an empty one has sp at the top boundary. */
static bool within_stack(const fiber_t* fiber, uintptr_t sp) {
    const uintptr_t base = (uintptr_t)fiber->stack;
    return sp >= base && sp <= base + fiber->stack_size;
}

static int test_execution_order(void) {
    ASSERT_EQ_U32(fx.trace_len, 3);
    ASSERT_EQ_U32(fx.trace[0], STEP_TEST_ENTER);
    ASSERT_EQ_U32(fx.trace[1], STEP_INTERMEDIARY);
    ASSERT_EQ_U32(fx.trace[2], STEP_TEST_RESUME);
    return 0;
}

static int test_entry_stack(void) {
    ASSERT_TRUE(within_stack(&fx.test_fiber, fx.entry_sp));
    ASSERT_EQ_U32(fx.entry_sp % ENTRY_SP_ALIGN, ENTRY_SP_OFFSET);
    return 0;
}

/* Restore side: registers after the round trip equal what was loaded. */
static int test_registers_restored(void) {
    ASSERT_REGS_EQ(fx.out_a, fx.magic_a);
    return 0;
}

/* Save side: the context_t slots hold the pattern while switched out. */
static int test_registers_saved(void) {
    ASSERT_REGS_EQ(fx.saved_a, fx.magic_a);
    ASSERT_REGS_EQ(fx.saved_b, fx.magic_b);
    return 0;
}

static int test_saved_stack_pointer(void) {
    ASSERT_TRUE(within_stack(&fx.test_fiber, fx.saved_sp_a));
    ASSERT_TRUE(within_stack(&fx.intermediary, fx.saved_sp_b));
    return 0;
}

int main(void) {
    cfiber_test_suite_begin("context switch / register preservation");

    cfiber_set_return_hook(noop_return_hook, nullptr);

    RUN_TEST(test_round_trip);
    RUN_TEST(test_execution_order);
    RUN_TEST(test_entry_stack);
    RUN_TEST(test_registers_restored);
    RUN_TEST(test_registers_saved);
    RUN_TEST(test_saved_stack_pointer);

    teardown_fiber(&fx.test_fiber);
    teardown_fiber(&fx.intermediary);
    return cfiber_test_report();
}
