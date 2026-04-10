/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_PTP_VMCLOCK_H
#define __ASM_PTP_VMCLOCK_H

#include <asm/tsc.h>

static inline u64 ptp_vmclock_read_cpu_counter(void)
{
	return cpu_feature_enabled(X86_FEATURE_TSC) ? rdtsc() : 0;
}

#endif
