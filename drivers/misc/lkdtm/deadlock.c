// SPDX-License-Identifier: GPL-2.0
/*
 * This is for all the tests related to deadlock.
 */
#include "lkdtm.h"
#include <linux/mm.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/wait.h>
#include <linux/pagemap.h>

static struct folio *folio_A;
static struct folio *folio_B;

/*
 * Triggering a simple AA deadlock on a folio, Attempting to acquire the same
 * folio twice in the same execution context, resulting in a self-deadlock.
 */
static void lkdtm_FOLIO_LOCK_AA(void)
{
	folio_A = folio_alloc(GFP_KERNEL | __GFP_ZERO, 0);

	if (!folio_A) {
		pr_err("folio_alloc() failed.\n");
		return;
	}

	folio_lock(folio_A);
	folio_lock(folio_A);

	/* Unreachable */
	folio_unlock(folio_A);
	folio_unlock(folio_A);

	folio_put(folio_A);
}

/*
 * Attempting the 'AB' order for ABBA deadlock
 */
static int lkdtm_folio_AB_kthread(void *data)
{
	while (true) {
		folio_lock(folio_A);
		folio_lock(folio_B);
		folio_unlock(folio_B);
		folio_unlock(folio_A);
	}

	return 0;
}

/*
 * Attempting the 'BA' order for ABBA deadlock
 */
static int lkdtm_folio_BA_kthread(void *data)
{
	while (true) {
		folio_lock(folio_B);
		folio_lock(folio_A);
		folio_unlock(folio_A);
		folio_unlock(folio_B);
	}

	return 0;
}

/*
 * Spawning kthreads that attempt to acquire Waiter A and Waiter B in reverse
 * order. Leading to a state where Thread A holds Waiter A and waits for
 * Waiter B, while Thread B holds Waiter B and waits for Waiter A.
 */
static void lkdtm_FOLIO_LOCK_ABBA(void)
{
	struct task_struct *t0, *t1;

	folio_A = folio_alloc(GFP_KERNEL | __GFP_ZERO, 0);
	folio_B = folio_alloc(GFP_KERNEL | __GFP_ZERO, 0);

	if (!folio_A || !folio_B) {
		pr_err("folio_alloc() failed.\n");
		return;
	}

	t0 = kthread_run(lkdtm_folio_AB_kthread, NULL, "lkdtm_folio_A");
	t1 = kthread_run(lkdtm_folio_BA_kthread, NULL, "lkdtm_folio_B");

	if (IS_ERR(t0) || IS_ERR(t1))
		pr_err("failed to start kthread.\n");

	folio_put(folio_A);
	folio_put(folio_B);
}

DEFINE_MUTEX(mutex_b);

/* Attempting 'folio_lock() A then Mutex B' order */
static int lkdtm_folio_mutex_kthread(void *data)
{
	while (true) {
		folio_lock(folio_A);
		mutex_lock(&mutex_b);
		mutex_unlock(&mutex_b);
		folio_unlock(folio_A);
	}

	return 0;
}

/* Attempting 'Mutex B then folio_lock() A' order */
static int lkdtm_mutex_folio_kthread(void *data)
{
	while (true) {
		mutex_lock(&mutex_b);
		folio_lock(folio_A);
		folio_unlock(folio_A);
		mutex_unlock(&mutex_b);
	}

	return 0;
}

/* Triggering ABBA deadlock between folio_lock() and mutex. */
static void lkdtm_FOLIO_MUTEX_LOCK_ABBA(void)
{
	struct task_struct *t0, *t1;

	folio_A = folio_alloc(GFP_KERNEL | __GFP_ZERO, 0);

	t0 = kthread_run(lkdtm_folio_mutex_kthread, NULL, "lkdtm_folio_mutex");
	t1 = kthread_run(lkdtm_mutex_folio_kthread, NULL, "lkdtm_mutex_folio");

	if (IS_ERR(t0) || IS_ERR(t1))
		pr_err("failed to start kthreads\n");

	folio_put(folio_A);
}

static struct crashtype crashtypes[] = {
	CRASHTYPE(FOLIO_LOCK_AA),
	CRASHTYPE(FOLIO_LOCK_ABBA),
	CRASHTYPE(FOLIO_MUTEX_LOCK_ABBA),
};

struct crashtype_category deadlock_crashtypes = {
	.crashtypes = crashtypes,
	.len = ARRAY_SIZE(crashtypes),
};
