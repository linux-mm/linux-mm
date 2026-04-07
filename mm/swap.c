// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/mm/swap.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 */

/*
 * This file contains the default values for the operation of the
 * Linux VM subsystem. Fine-tuning documentation can be found in
 * Documentation/admin-guide/sysctl/vm.rst.
 * Started 18.12.91
 * Swap aging added 23.2.95, Stephen Tweedie.
 * Buffermem limits added 12.3.98, Rik van Riel.
 */

#include <linux/mm.h>
#include <linux/init.h>
#include <linux/swap.h>

/* How many pages do we try to swap or page in/out together? As a power of 2 */
int page_cluster;
static const int page_cluster_max = 31;

static const struct ctl_table swap_sysctl_table[] = {
	{
		.procname	= "page-cluster",
		.data		= &page_cluster,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= (void *)&page_cluster_max,
	}
};

/*
 * Perform any setup for the swap system
 */
void __init swap_setup(void)
{
	unsigned long megs = PAGES_TO_MB(totalram_pages());

	/* Use a smaller cluster for small-memory machines */
	if (megs < 16)
		page_cluster = 2;
	else
		page_cluster = 3;
	/*
	 * Right now other parts of the system means that we
	 * _really_ don't want to cluster much more
	 */

	register_sysctl_init("vm", swap_sysctl_table);
}
