/* SPDX-FileCopyrightText: 2026 Aoto
 * SPDX-License-Identifier: MIT */

/* Object-format bracketing for the hosted .S files. ELF wants .type, .size,
 * .hidden and a .note.GNU-stack section; Mach-O wants a leading underscore
 * on every symbol and .private_extern. The bodies stay format-free: symbols
 * crossing the C/assembly boundary go through SYM() in both directions, and
 * every file ends with ASM_FILE_END.
 *
 * Statement separators: `;` on ELF, `%%` on Mach-O arm64 (where `;` starts
 * a comment). */

#ifndef CFIBER_FIBER_ASM_DEFS_INC
#define CFIBER_FIBER_ASM_DEFS_INC

#ifdef __APPLE__

#define SYM(name) _##name
#define FUNC_BEGIN(name) .globl SYM(name) %% SYM(name):
#define FUNC_END(name)
#define FUNC_HIDDEN(name) .private_extern SYM(name)
#define ASM_FILE_END

#else /* ELF */

#define SYM(name) name
#define FUNC_BEGIN(name) .global SYM(name); .type SYM(name), %function; SYM(name):
#define FUNC_END(name) .size SYM(name), . - SYM(name)
#define FUNC_HIDDEN(name) .hidden SYM(name)
#define ASM_FILE_END .section .note.GNU-stack, "", %progbits

#endif

#endif /* CFIBER_FIBER_ASM_DEFS_INC */
