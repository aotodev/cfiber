/* SPDX-FileCopyrightText: 2026 Aoto
 * SPDX-License-Identifier: MIT */

/**
 * @file  internal.h
 * @brief Library-private helpers. Not installed; no public header includes it.
 */

#ifndef CFIBER_INTERNAL_H
#define CFIBER_INTERNAL_H

#include "cfiber/core/macros.h"

#include <stddef.h>

#define CFIBER_HIDDEN __attribute__((visibility("hidden")))

#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

#ifdef NDEBUG
#define ASSERT(expr) ((void)0)
#else
#define ASSERT(expr) ((expr) ? (void)0 : __builtin_trap())
#endif

static inline size_t align_up(size_t v, size_t align) {
    return (v + align - 1) & ~(align - 1);
}

/* First-entry trampoline, per-arch assembly: calls the entry function with its
 * argument, then cfiber_epilogue. */
CFIBER_HIDDEN void cfiber_prologue(void);

/* Runs on the returning fiber's stack; dispatches to the registered return hook. */
[[noreturn]] CFIBER_HIDDEN void cfiber_epilogue(void);

#endif /* CFIBER_INTERNAL_H */
