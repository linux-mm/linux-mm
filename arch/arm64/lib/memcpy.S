/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2012-2021, Arm Limited.
 *
 * Adapted from the original at:
 * https://github.com/ARM-software/optimized-routines/blob/afd6244a1f8d9229/string/aarch64/memcpy.S
 */

#include <linux/linkage.h>
#include <asm/assembler.h>

/* Assumptions:
 *
 * ARMv8-a, AArch64, unaligned accesses.
 *
 */

	.macro ldrb1 reg, addr:vararg
		ldrb  \reg, \addr
	.endm

	.macro ldr1 reg, addr:vararg
		ldr   \reg, \addr
	.endm

	.macro ldp1 reg1, reg2, addr:vararg
		ldp   \reg1, \reg2, \addr
	.endm

	.macro ret1
		ret
	.endm

	.macro cpy1 dst, src, count
		.arch_extension mops
		cpyp [\dst]!, [\src]!, \count!
		cpym [\dst]!, [\src]!, \count!
		cpye [\dst]!, [\src]!, \count!
	.endm

SYM_FUNC_START(__pi_memcpy)
#include "memcpy_template.S"
SYM_FUNC_END(__pi_memcpy)

SYM_FUNC_ALIAS(__memcpy, __pi_memcpy)
EXPORT_SYMBOL(__memcpy)
SYM_FUNC_ALIAS_WEAK(memcpy, __memcpy)
EXPORT_SYMBOL(memcpy)

SYM_FUNC_ALIAS(__pi_memmove, __pi_memcpy)

SYM_FUNC_ALIAS(__memmove, __pi_memmove)
EXPORT_SYMBOL(__memmove)
SYM_FUNC_ALIAS_WEAK(memmove, __memmove)
EXPORT_SYMBOL(memmove)
