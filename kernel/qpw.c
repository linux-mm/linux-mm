// SPDX-License-Identifier: GPL-2.0
#include "linux/export.h"
#include <linux/sched.h>
#include <linux/qpw.h>
#include <linux/string.h>

DEFINE_STATIC_KEY_MAYBE(CONFIG_QPW_DEFAULT, qpw_sl);
EXPORT_SYMBOL(qpw_sl);

static int __init qpw_setup(char *str)
{
	int opt;

	if (!get_option(&str, &opt)) {
		pr_warn("QPW: invalid qpw parameter: %s, ignoring.\n", str);
		return 0;
	}

	if (opt)
		static_branch_enable(&qpw_sl);
	else
		static_branch_disable(&qpw_sl);

	return 1;
}
__setup("qpw=", qpw_setup);
