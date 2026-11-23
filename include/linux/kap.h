/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_KAP_H
#define _LINUX_KAP_H

#include <linux/types.h>

#ifdef CONFIG_SECURITY_KAP
int kap_check_namespace_flags(unsigned long flags, const char *operation);
#else
static inline int kap_check_namespace_flags(unsigned long flags, const char *operation)
{
	return 0;
}
#endif

#endif /* _LINUX_KAP_H */
