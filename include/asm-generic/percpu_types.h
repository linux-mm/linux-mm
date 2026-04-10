/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_GENERIC_PERCPU_TYPES_H_
#define _ASM_GENERIC_PERCPU_TYPES_H_

#ifndef __ASSEMBLER__
/*
 * __percpu_qual is the qualifier for the percpu named address space.
 *
 * Most arches use generic named address space for percpu variables but
 * some arches define percpu variables in different named address space
 * (on the x86 arch, percpu variable may be declared as being relative
 * to the %fs or %gs segments using __seg_fs or __seg_gs named address
 * space qualifier).
 */
#ifndef __percpu_qual
# define __percpu_qual
#endif

#endif /* __ASSEMBLER__ */
#endif /* _ASM_GENERIC_PERCPU_TYPES_H_ */
