/* SPDX-License-Identifier: GPL-2.0+ OR MIT */
/* SPDX-FileCopyrightText: 2025-2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com> */

#ifndef _PERCPU_COUNTER_TREE_H
#define _PERCPU_COUNTER_TREE_H

#include <linux/bug.h>
#include <linux/atomic.h>
#include <linux/limits.h>
#include <linux/percpu.h>

/* Largest batch size: level 0 residuals are within (-batch, +batch) of a signed byte. */
#define PERCPU_COUNTER_TREE_MAX_BATCH	128
static_assert(PERCPU_COUNTER_TREE_MAX_BATCH - 1 <= S8_MAX);

#ifdef CONFIG_SMP

struct percpu_counter_tree {
	/*
	 * Per-CPU split counters (tree level 0): residuals not propagated
	 * yet, within (-batch, +batch).
	 */
	s8 __percpu *level0;
	/*
	 * The batch size is the increment step at level 0 which triggers a
	 * carry propagation: a power of 2 between 2 and
	 * PERCPU_COUNTER_TREE_MAX_BATCH. It is stored next to level0, the
	 * other field read by the update fast path, so both usually share a
	 * cache line: this leaves a hole before items_stride.
	 */
	unsigned char batch_order;
	unsigned int items_stride;	/* Distance between this counter's consecutive items. */
	/*
	 * Approximate sum: this counter's item of the root, in the tree
	 * items allocated with the per-CPU counters.
	 */
	atomic_long_t *approx_sum;
	long bias;	/* Bias to apply to counter precise and approximate values. */
};

int percpu_counter_tree_init_many(struct percpu_counter_tree *counters,
				  unsigned int nr_counters, unsigned long batch_size,
				  gfp_t gfp_flags);
int percpu_counter_tree_init(struct percpu_counter_tree *counter,
			     unsigned long batch_size, gfp_t gfp_flags);
void percpu_counter_tree_destroy_many(struct percpu_counter_tree *counters,
				      unsigned int nr_counters);
void percpu_counter_tree_destroy(struct percpu_counter_tree *counter);
void percpu_counter_tree_add(struct percpu_counter_tree *counter, long inc);
long percpu_counter_tree_precise_sum(struct percpu_counter_tree *counter);
int percpu_counter_tree_approximate_compare(struct percpu_counter_tree *a,
					    struct percpu_counter_tree *b);
int percpu_counter_tree_approximate_compare_value(struct percpu_counter_tree *counter, long v);
int percpu_counter_tree_precise_compare(struct percpu_counter_tree *a,
					struct percpu_counter_tree *b);
int percpu_counter_tree_precise_compare_value(struct percpu_counter_tree *counter, long v);
void percpu_counter_tree_set(struct percpu_counter_tree *counter, long v);
int percpu_counter_tree_subsystem_init(void);

/**
 * percpu_counter_tree_approximate_sum() - Return approximate counter sum.
 * @counter: The counter to sum.
 *
 * Querying the approximate sum is fast, but it is only accurate within
 * the bound returned by percpu_counter_tree_approximate_accuracy().
 * This is meant to be used when speed is preferred over accuracy.
 *
 * Return: The current approximate counter sum.
 */
static inline
long percpu_counter_tree_approximate_sum(struct percpu_counter_tree *counter)
{
	unsigned long v = atomic_long_read(counter->approx_sum);

	return (long)(v + (unsigned long)READ_ONCE(counter->bias));
}

unsigned long percpu_counter_tree_approximate_accuracy(struct percpu_counter_tree *counter);

#if IS_ENABLED(CONFIG_KUNIT)
unsigned int percpu_counter_tree_check_config(void);
unsigned int percpu_counter_tree_check_residuals(struct percpu_counter_tree *counter);
void percpu_counter_tree_copy_state(struct percpu_counter_tree *dst,
				    struct percpu_counter_tree *src);
#endif

#else	/* !CONFIG_SMP */

struct percpu_counter_tree {
	atomic_long_t count;
};

static inline
int percpu_counter_tree_init_many(struct percpu_counter_tree *counters,
				  unsigned int nr_counters, unsigned long batch_size,
				  gfp_t gfp_flags)
{
	/*
	 * There is no tree to batch into on UP, so @batch_size has no
	 * effect. Validate it anyway so callers get the same result as
	 * on SMP.
	 */
	if (WARN_ON(batch_size <= 1 || (batch_size & (batch_size - 1)) ||
		    batch_size > PERCPU_COUNTER_TREE_MAX_BATCH))
		return -EINVAL;
	for (unsigned int i = 0; i < nr_counters; i++)
		atomic_long_set(&counters[i].count, 0);
	return 0;
}

static inline
int percpu_counter_tree_init(struct percpu_counter_tree *counter,
			     unsigned long batch_size, gfp_t gfp_flags)
{
	return percpu_counter_tree_init_many(counter, 1, batch_size, gfp_flags);
}

static inline
void percpu_counter_tree_destroy_many(struct percpu_counter_tree *counters,
				      unsigned int nr_counters)
{
}

static inline
void percpu_counter_tree_destroy(struct percpu_counter_tree *counter)
{
}

static inline
long percpu_counter_tree_precise_sum(struct percpu_counter_tree *counter)
{
	return atomic_long_read(&counter->count);
}

static inline
int percpu_counter_tree_precise_compare(struct percpu_counter_tree *a,
					struct percpu_counter_tree *b)
{
	/* Compare on the wrapped difference, as on SMP. */
	long delta = (long)((unsigned long)percpu_counter_tree_precise_sum(a) -
			    (unsigned long)percpu_counter_tree_precise_sum(b));

	if (delta < 0)
		return -1;
	if (delta > 0)
		return 1;
	return 0;
}

static inline
int percpu_counter_tree_precise_compare_value(struct percpu_counter_tree *counter, long v)
{
	/* Compare on the wrapped difference, as on SMP. */
	long delta = (long)((unsigned long)v -
			    (unsigned long)percpu_counter_tree_precise_sum(counter));

	if (delta < 0)
		return -1;
	if (delta > 0)
		return 1;
	return 0;
}

static inline
int percpu_counter_tree_approximate_compare(struct percpu_counter_tree *a,
					    struct percpu_counter_tree *b)
{
	return percpu_counter_tree_precise_compare(a, b);
}

static inline
int percpu_counter_tree_approximate_compare_value(struct percpu_counter_tree *counter, long v)
{
	return percpu_counter_tree_precise_compare_value(counter, v);
}

static inline
void percpu_counter_tree_set(struct percpu_counter_tree *counter, long v)
{
	atomic_long_set(&counter->count, v);
}

static inline
unsigned long percpu_counter_tree_approximate_accuracy(struct percpu_counter_tree *counter)
{
	return 0;
}

static inline
void percpu_counter_tree_add(struct percpu_counter_tree *counter, long inc)
{
	atomic_long_add(inc, &counter->count);
}

static inline
long percpu_counter_tree_approximate_sum(struct percpu_counter_tree *counter)
{
	return percpu_counter_tree_precise_sum(counter);
}

static inline
int percpu_counter_tree_subsystem_init(void)
{
	return 0;
}

#endif	/* CONFIG_SMP */

/**
 * percpu_counter_tree_approximate_sum_positive() - Return a positive approximate counter sum.
 * @counter: The counter to sum.
 *
 * Return an approximate counter sum which is guaranteed to be greater
 * or equal to 0.
 *
 * Return: The current positive approximate counter sum.
 */
static inline
long percpu_counter_tree_approximate_sum_positive(struct percpu_counter_tree *counter)
{
	long v = percpu_counter_tree_approximate_sum(counter);

	return v > 0 ? v : 0;
}

/**
 * percpu_counter_tree_precise_sum_positive() - Return a positive precise counter sum.
 * @counter: The counter to sum.
 *
 * Return a precise counter sum which is guaranteed to be greater
 * or equal to 0.
 *
 * Return: The current positive precise counter sum.
 */
static inline
long percpu_counter_tree_precise_sum_positive(struct percpu_counter_tree *counter)
{
	long v = percpu_counter_tree_precise_sum(counter);

	return v > 0 ? v : 0;
}

/**
 * percpu_counter_tree_approximate_min_max_range() - Return the min and max precise values.
 * @approx_sum: Approximated sum.
 * @accuracy: Tree accuracy, from percpu_counter_tree_approximate_accuracy().
 * @precise_min: Minimum possible value for precise sum (output).
 * @precise_max: Maximum possible value for precise sum (output).
 *
 * Calculate the minimum and maximum precise values for a given approximation
 * and accuracy. The accuracy is a bound on a settled tree: one with no update
 * in progress.
 *
 * The approximation is within the accuracy of the precise sum, in both
 * directions:
 *
 *   precise_sum - accuracy <= approx_sum <= precise_sum + accuracy
 *
 * Therefore, the precise sum is within the accuracy of the approximation:
 *
 *   approx_sum - accuracy <= precise_sum <= approx_sum + accuracy
 */
static inline
void percpu_counter_tree_approximate_min_max_range(long approx_sum, unsigned long accuracy,
						   long *precise_min, long *precise_max)
{
	*precise_min = approx_sum - accuracy;
	*precise_max = approx_sum + accuracy;
}

/**
 * percpu_counter_tree_approximate_min_max() - Return the approximation, min and max precise values.
 * @counter: The counter to sum.
 * @approx_sum: Approximate sum (output).
 * @precise_min: Minimum possible value for precise sum (output).
 * @precise_max: Maximum possible value for precise sum (output).
 *
 * Return the approximate sum, minimum and maximum precise values for
 * a counter.
 */
static inline
void percpu_counter_tree_approximate_min_max(struct percpu_counter_tree *counter,
					     long *approx_sum, long *precise_min, long *precise_max)
{
	unsigned long accuracy = percpu_counter_tree_approximate_accuracy(counter);
	long v = percpu_counter_tree_approximate_sum(counter);

	percpu_counter_tree_approximate_min_max_range(v, accuracy, precise_min, precise_max);
	*approx_sum = v;
}

#endif  /* _PERCPU_COUNTER_TREE_H */
