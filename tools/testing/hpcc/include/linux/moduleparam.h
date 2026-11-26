/* SPDX-License-Identifier: GPL-2.0+ OR MIT */
#include "../hpcc_shim.h"

struct kernel_param;

struct kernel_param_ops {
	int (*set)(const char *val, const struct kernel_param *kp);
	int (*get)(char *buffer, const struct kernel_param *kp);
};

#define module_param_cb(name, ops, arg, perm)
#define MODULE_PARM_DESC(parm, desc)
