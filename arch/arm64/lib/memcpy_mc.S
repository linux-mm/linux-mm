/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2012-2021, Arm Limited.
 *
 * Adapted from the original at:
 * https://github.com/ARM-software/optimized-routines/blob/afd6244a1f8d9229/string/aarch64/memcpy.S
 */

#include <linux/linkage.h>
#include <asm/assembler.h>
#include <asm/asm-uaccess.h>

/* Assumptions:
 *
 * ARMv8-a, AArch64, unaligned accesses.
 *
 */

	.macro ldrb1 reg, addr:vararg
	KERNEL_SEA(9998f, ldrb  \reg, \addr)
	.endm

	.macro ldr1 reg, addr:vararg
	KERNEL_SEA(9998f, ldr   \reg, \addr)
	.endm

	.macro ldp1 reg1, reg2, addr:vararg
	KERNEL_SEA(9998f, ldp   \reg1, \reg2, \addr)
	.endm

	.macro ret1
	mov	x0, #0
	ret
	.endm

	.macro cpy1 dst, src, count
	.arch_extension mops
	KERNEL_SEA(9998f, cpyp [\dst]!, [\src]!, \count!)
	KERNEL_SEA(9996f, cpym [\dst]!, [\src]!, \count!)
	KERNEL_SEA(9996f, cpye [\dst]!, [\src]!, \count!)
	.endm

SYM_FUNC_START(__memcpy_mc)
#include "memcpy_template.S"

	// Exception fixups
9996:	b.cs	9998f
	// Registers are in Option A format
	add	dst, dst, count
9998:	sub	x0, dstend, dstin			// bytes not copied
	ret
SYM_FUNC_END(__memcpy_mc)

EXPORT_SYMBOL(__memcpy_mc)
SYM_FUNC_ALIAS_WEAK(memcpy_mc, __memcpy_mc)
EXPORT_SYMBOL(memcpy_mc)
