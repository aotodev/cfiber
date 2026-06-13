/**
 * @file  fuzz_input.h
 * @brief Tiny byte-stream reader and invariant macro shared by the fuzzers.
 *
 * @details The fuzzers interpret libFuzzer's random input as a stream of
 *          opcodes/parameters. This reader yields zero once exhausted, so a
 *          harness can drain the input with a simple `while (fuzz_remaining())`
 *          loop without bounds-checking every read.
 */

#ifndef CFIBER_FUZZ_INPUT_H
#define CFIBER_FUZZ_INPUT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    const uint8_t* data;
    size_t size;
    size_t pos;
} fuzz_input;

static inline fuzz_input fuzz_input_init(const uint8_t* data, size_t size) {
    return (fuzz_input){.data = data, .size = size, .pos = 0};
}

static inline size_t fuzz_remaining(const fuzz_input* in) {
    return in->size - in->pos;
}

static inline uint8_t fuzz_u8(fuzz_input* in) {
    return in->pos < in->size ? in->data[in->pos++] : 0u;
}

static inline uint32_t fuzz_u32(fuzz_input* in) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        v = (v << 8) | fuzz_u8(in);
    }
    return v;
}

/** Inclusive [lo, hi] range derived from the input. */
static inline uint32_t fuzz_range(fuzz_input* in, uint32_t lo, uint32_t hi) {
    if (hi <= lo) {
        return lo;
    }
    return lo + (fuzz_u32(in) % (hi - lo + 1u));
}

/**
 * Invariant assertion: aborts (so libFuzzer reports a crash with the input that
 * triggered it) when @p cond is false. Unlike the library's ASSERT, this is
 * always armed regardless of NDEBUG.
 */
#define FUZZ_CHECK(cond)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "FUZZ INVARIANT FAILED: %s (%s:%d)\n", #cond, __FILE__, __LINE__);                         \
            abort();                                                                                                   \
        }                                                                                                              \
    } while (0)

#endif /* CFIBER_FUZZ_INPUT_H */
