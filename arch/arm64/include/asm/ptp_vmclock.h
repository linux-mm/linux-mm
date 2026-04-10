/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_PTP_VMCLOCK_H
#define __ASM_PTP_VMCLOCK_H

#include <asm/arch_timer.h>

static inline u64 ptp_vmclock_read_cpu_counter(void)
{
	return arch_timer_read_counter();
}

#endif
