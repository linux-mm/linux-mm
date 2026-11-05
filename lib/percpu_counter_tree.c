// SPDX-License-Identifier: GPL-2.0+ OR MIT
// SPDX-FileCopyrightText: 2025-2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>

/*
 * Split Counters With Tree Approximation Propagation
 *
 * * Overview
 *
 * Each counter is a tree of counters. A CPU updates its own level 0
 * counter, a signed byte. Each counter keeps the residual it has not
 * propagated yet, and propagates a carry to its parent on the next level
 * once that residual reaches the batch size of its level, up to the root.
 * Most updates therefore stay on the updating CPU. The root holds the
 * approximate sum, read in O(1), and the precise sum adds the residuals of
 * all levels to it.
 *
 * * Tree shape
 *
 * Illustration with 8 CPUs and a fan-out of 2 at each level, which makes
 * 3 levels of counters below the root. This is not an actual
 * configuration: at 8 CPUs, the default tree is flat.
 *
 * Level 3:  0  (root)
 *           |   \
 *           |         \
 *           |               \
 * Level 2:  0                   1
 *           | \                 | \
 *           |    \              |    \
 *           |       \           |       \
 * Level 1:  0         2         1         3
 *           | \       | \       | \       | \
 *           |  \      |  \      |  \      |  \
 *           |   \     |   \     |   \     |   \
 * Level 0:  0    4    2    6    1    5    3    7
 *
 * CPU c propagates into item (c & (L - 1)) of a level holding L items,
 * so the CPUs sharing an item are not adjacent: CPUs 0 and 4 share
 * level 1 item 0, and CPUs 0, 2, 4, 6 share level 2 item 0. This mapping
 * uses every item of each level and gives each item the same number of
 * CPUs, give or take one, even when nr_cpu_ids is not a power of 2. The
 * diagram lists the CPUs in bit-reversed order to avoid crossing lines.
 *
 * The actual fan-out of each level depends on the topology (see
 * per_nr_cpu_order_config): up to 32 CPUs, the tree is flat, and the
 * level 0 counters propagate directly to the root.
 *
 * * Batch sizes and residuals
 *
 * The batch size of a level, BATCH(level N), is the batch size of the
 * level below multiplied by the fan-out of that level. Example for
 * BATCH(level 0) = 32, with the tree of the illustration above:
 *
 * BATCH(level 0) =  32
 * BATCH(level 1) =  64
 * BATCH(level 2) = 128
 *
 * Once the residual of a counter reaches +/- BATCH(level N), it propagates
 * its multiples of BATCH(level N), truncated toward zero, and subtracts
 * them. This hysteresis prevents updates going back and forth around a
 * BATCH(level N) boundary from propagating up the tree each time.
 *
 * The residual of a settled counter therefore stays within
 * (-BATCH(level N), +BATCH(level N)), and moves in steps of the counter's
 * input: level 0 receives the updates, in steps of 1, while a counter
 * above level 0 only receives the carries of the level below, in steps of
 * BATCH(level N - 1). The largest residual of a counter is the largest
 * multiple of its input step below its batch size: BATCH(level 0) - 1 at
 * level 0, and BATCH(level N) - BATCH(level N - 1) above:
 *
 *            input   residual        worst case        worst case
 *            step    per counter     below precise     above precise
 * Level 0:   1       [-31 .. 31]     248  (8 * 31)     248  (8 * 31)
 * Level 1:   32      {-32, 0, 32}    128  (4 * 32)     128  (4 * 32)
 * Level 2:   64      {-64, 0, 64}    128  (2 * 64)     128  (2 * 64)
 * Total:                             504               504
 *
 * * Approximation accuracy
 *
 * The approximate sum is the precise sum minus the residuals, so a
 * settled counter (no update in progress) is within:
 *
 *   precise - 504 <= approx <= precise + 504
 *
 * The accuracy returned by percpu_counter_tree_approximate_accuracy() is
 * exact. It adds the largest residuals of all the counters:
 * BATCH(level 0) - 1 for each of the nr_cpu_ids level 0 counters, and
 * BATCH(level N) - BATCH(level N - 1) for each counter of level N above.
 * Counting each level 0 counter as one BATCH(level 0), minus its input
 * step of 1:
 *
 *   accuracy_multiplier = nr_cpu_ids + sum over levels N >= 1 of
 *       (number of counters at level N) *
 *       (BATCH(level N) - BATCH(level N - 1)) / BATCH(level 0)
 *
 *   accuracy = BATCH(level 0) * accuracy_multiplier - nr_cpu_ids
 *
 * In the example above, accuracy_multiplier = 8 + 4 * 1 + 2 * 2 = 16, and:
 *
 *   accuracy = 32 * 16 - 8 = 504
 *
 * Every counter above level 0 is reachable from a CPU below nr_cpu_ids,
 * even when nr_cpu_ids is not a power of 2, so both bounds can be reached
 * when the possible CPUs are contiguous. When they are sparse, the bound
 * also counts the level 0 residuals of the missing CPUs: it holds, but
 * cannot be reached.
 *
 * * Carry propagation
 *
 * The items at each tree position of the counters initialized together
 * share cache lines, which are not shared with other tree positions to
 * prevent false sharing between CPUs.
 *
 * Example with expanded values, for the tree and batch sizes above:
 *
 * counter_add(counter, inc):
 *
 *         if (!inc)
 *                 return;
 *
 *         // Level 0: per-CPU residual within (-32, +32).
 *         old = percpu_read(counter @ Level 0);
 *         do {
 *                 v = old + inc;
 *                 out = v < 0 ? -(-v & ~0b00011111) : (v & ~0b00011111);
 *         } while (!percpu_try_cmpxchg(counter @ Level 0, &old, v - out));
 *         inc = out;
 *         if (!inc)
 *                 return;
 *
 *         // Levels 1 and 2: add the carry, then extract the multiples
 *         // of the level batch held by the counter, truncated toward
 *         // zero. A failed cmpxchg updates res to the current residual.
 *         res = atomic_long_add_return(counter @ Level 1, inc);
 *         do {
 *                 inc = res < 0 ? -(-res & ~0b00111111) : (res & ~0b00111111);
 *                 if (!inc)
 *                         return;
 *         } while (!atomic_long_try_cmpxchg(counter @ Level 1, &res, res - inc));
 *
 *         res = atomic_long_add_return(counter @ Level 2, inc);
 *         do {
 *                 inc = res < 0 ? -(-res & ~0b01111111) : (res & ~0b01111111);
 *                 if (!inc)
 *                         return;
 *         } while (!atomic_long_try_cmpxchg(counter @ Level 2, &res, res - inc));
 *
 *         atomic_long_add(counter @ Level 3, inc);
 */

#define pr_fmt(fmt) "percpu_counter_tree: " fmt

#include <linux/percpu_counter_tree.h>
#include <linux/moduleparam.h>
#include <linux/align.h>
#include <linux/cache.h>
#include <linux/cpumask.h>
#include <linux/atomic.h>
#include <linux/export.h>
#include <linux/percpu.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/math.h>
#include <linux/log2.h>
#include <linux/args.h>
#include <linux/string.h>
#include <kunit/visibility.h>

#define MAX_NR_LEVELS 7

/*
 * The counter configuration is selected at boot time based on the
 * hardware topology.
 */
struct counter_config {
	/*
	 * nr_levels is the number of hierarchical counter tree levels. It
	 * excludes the final level (approximate sum).
	 */
	unsigned char nr_levels;
	/* n-arity of each level from 0 to (nr_levels - 1). */
	unsigned char n_arity_order[MAX_NR_LEVELS];
};

/*
 * The fan-out of each level of the tree is chosen with a heuristic trying to
 * minimize both the tree depth and the fan-out.
 */
#define PCT_COUNTER_CONFIG(order, ...)						\
	[order] = {								\
		.nr_levels = COUNT_ARGS(__VA_ARGS__),				\
		.n_arity_order = { __VA_ARGS__ },				\
	}

/*
 * Up to 32 CPUs, the tree is flat: the level 0 counters propagate directly
 * to the root, like struct percpu_counter. From 33 to 64 CPUs, the level 0
 * counters propagate to 4 intermediate counters, each shared by up to 16
 * CPUs, so the carries of all CPUs do not contend on the root. Beyond, each
 * level has a fan-out of 8. When the order is not a multiple of 3, a
 * remaining order of 1 gives the root a fan-out of 16, and a remaining
 * order of 2 adds a level with a fan-out of 4 below the root: a root
 * fan-out of 16 updates about as fast as a fan-out of 2 below the root,
 * with one level less, which shrinks the accuracy bound and the tree
 * items, while a root fan-out of 32 is slower than a fan-out of 4 below
 * it. This keeps the carry rate reaching each tree item low with the RSS
 * batch size of 32, and the approximation error grows as
 * O(nr_cpus * log(nr_cpus)), compared to O(nr_cpus^2) for struct
 * percpu_counter whose batch size grows with the number of CPUs.
 *
 * The tree is shaped for nr_cpu_ids rounded up to a power of 2, and each
 * tree position takes a cache line. A set of 4 counters initialized together,
 * such as the RSS counters of an mm, uses less memory than 4 struct
 * percpu_counter from a few CPUs up to 64, and beyond when nr_cpu_ids is a
 * power of 2 (at most 0.89 times as much). Between powers of 2, fewer CPUs
 * share the cost of the tree items: up to 1.1 times at 3 * 2^n CPUs, and
 * up to 1.48 times just above a power of 2. A lone counter uses one item
 * of each cache line: 3.0 to 3.5 times the memory of a struct
 * percpu_counter when nr_cpu_ids is a power of 2 beyond 64, and up to 5.7
 * times just above.
 */
static const struct counter_config per_nr_cpu_order_config[] = {
	PCT_COUNTER_CONFIG(0),
	PCT_COUNTER_CONFIG(1, 1),
	PCT_COUNTER_CONFIG(2, 2),
	PCT_COUNTER_CONFIG(3, 3),
	PCT_COUNTER_CONFIG(4, 4),
	PCT_COUNTER_CONFIG(5, 5),
	PCT_COUNTER_CONFIG(6, 4, 2),
	PCT_COUNTER_CONFIG(7, 3, 4),
	PCT_COUNTER_CONFIG(8, 3, 2, 3),
	PCT_COUNTER_CONFIG(9, 3, 3, 3),
	PCT_COUNTER_CONFIG(10, 3, 3, 4),
	PCT_COUNTER_CONFIG(11, 3, 3, 2, 3),
	PCT_COUNTER_CONFIG(12, 3, 3, 3, 3),
	PCT_COUNTER_CONFIG(13, 3, 3, 3, 4),
	PCT_COUNTER_CONFIG(14, 3, 3, 3, 2, 3),
	PCT_COUNTER_CONFIG(15, 3, 3, 3, 3, 3),
	PCT_COUNTER_CONFIG(16, 3, 3, 3, 3, 4),
	PCT_COUNTER_CONFIG(17, 3, 3, 3, 3, 2, 3),
	PCT_COUNTER_CONFIG(18, 3, 3, 3, 3, 3, 3),
	PCT_COUNTER_CONFIG(19, 3, 3, 3, 3, 3, 4),
	PCT_COUNTER_CONFIG(20, 3, 3, 3, 3, 3, 2, 3),
};

#undef PCT_COUNTER_CONFIG

/* The configuration table covers every possible number of CPUs. */
#define MAX_NR_CPUS_ORDER	20
#if NR_CPUS > (1UL << MAX_NR_CPUS_ORDER)
#error "NR_CPUS exceeds the hierarchical counter configuration table"
#endif
static_assert(ARRAY_SIZE(per_nr_cpu_order_config) == MAX_NR_CPUS_ORDER + 1);

/* Tree topology, selected at boot and shared by all the counters. */
static struct {
	/* Hierarchical counter configuration for the hardware topology. */
	const struct counter_config *config;
	/* Tree shape selected with the "arity" boot parameter. */
	struct counter_config boot_config;
	/* Order of nr_cpu_ids. */
	unsigned int nr_cpus_order;
	/*
	 * Tree items per counter, including the root. UINT_MAX catches early
	 * boot misuses.
	 */
	unsigned int nr_items;
	/*
	 * Largest residuals of all the counters, in units of the level 0
	 * batch size: see percpu_counter_tree_accuracy().
	 */
	unsigned long accuracy_multiplier;
	/* Tree items when those of a tree position fit in one cache line. */
	struct kmem_cache *items_cachep;
} tree_topology __ro_after_init = {
	.nr_items = UINT_MAX,
};

/* Value of the "arity" boot parameter, parsed at subsystem initialization. */
static const char *arity_param;

/*
 * Size of the items of one tree position for the counters initialized
 * together by percpu_counter_tree_init_many(), packed in cache lines which
 * are not shared with other tree positions. Each item is the residual of
 * the carries received and not propagated yet, within (-batch, +batch) of
 * its tree level when settled. The items of a tree position are updated
 * by the same CPUs, so packing them does not add contention between
 * unrelated CPUs.
 */
static
size_t percpu_counter_tree_position_size(unsigned int nr_counters)
{
	return ALIGN(nr_counters * sizeof(atomic_long_t), SMP_CACHE_BYTES);
}

/* Distance between the consecutive items of each of @nr_counters counters. */
static inline
unsigned int percpu_counter_tree_items_stride(unsigned int nr_counters)
{
	return percpu_counter_tree_position_size(nr_counters) / sizeof(atomic_long_t);
}

/* Allocate the zeroed tree items of @nr_counters counters, the root last. */
static
atomic_long_t *percpu_counter_tree_alloc_items(unsigned int nr_counters, gfp_t gfp_flags)
{
	size_t position_size = percpu_counter_tree_position_size(nr_counters);

	if (position_size == SMP_CACHE_BYTES)
		return kmem_cache_zalloc(tree_topology.items_cachep, gfp_flags);
	/* kmalloc() aligns power of 2 sizes on their size. */
	return kzalloc(roundup_pow_of_two(tree_topology.nr_items * position_size),
		       gfp_flags);
}

static
void percpu_counter_tree_free_items(atomic_long_t *items, unsigned int nr_counters)
{
	if (percpu_counter_tree_position_size(nr_counters) == SMP_CACHE_BYTES)
		kmem_cache_free(tree_topology.items_cachep, items);
	else
		kfree(items);
}

static
int __percpu_counter_tree_init(struct percpu_counter_tree *counter,
			       unsigned long batch_size,
			       s8 __percpu *level0,
			       atomic_long_t *items, unsigned int items_stride)
{
	/* Batch size must be greater than 1, a power of 2, and fit in a byte. */
	if (WARN_ON(batch_size <= 1 || (batch_size & (batch_size - 1)) ||
		    batch_size > PERCPU_COUNTER_TREE_MAX_BATCH))
		return -EINVAL;
	counter->batch_order = ilog2(batch_size);
	counter->bias = 0;
	atomic_long_set(&counter->atomic_bias, 0);
	counter->level0 = level0;
	counter->items_stride = items_stride;
	/* The root is the last item, allocated by the activation if lazy. */
	counter->approx_sum = items ?
		&items[(tree_topology.nr_items - 1) * items_stride] : NULL;
	return 0;
}

static inline
unsigned long percpu_counter_tree_batch_size(struct percpu_counter_tree *counter)
{
	return 1UL << counter->batch_order;
}

/* This counter's first item. */
static inline
atomic_long_t *percpu_counter_tree_items(struct percpu_counter_tree *counter)
{
	return counter->approx_sum - (tree_topology.nr_items - 1) * counter->items_stride;
}

/*
 * The residuals of all levels are within (-batch, +batch) of their level,
 * so the approximation is within the same distance of the precise sum in
 * both directions: the largest residuals of all the counters, which
 * accuracy_multiplier counts in units of the level 0 batch size, minus the
 * input step of 1 of each level 0 counter. See "Approximation accuracy" at
 * the top of this file.
 */
static inline
unsigned long percpu_counter_tree_accuracy(struct percpu_counter_tree *counter)
{
	unsigned long batch_size = percpu_counter_tree_batch_size(counter);

	return batch_size * tree_topology.accuracy_multiplier - nr_cpu_ids;
}

/* Initialize @nr_counters counters, with their per-CPU counters and tree items, or NULL if lazy. */
static
int __percpu_counter_tree_init_many(struct percpu_counter_tree *counters,
				    unsigned int nr_counters, unsigned long batch_size,
				    s8 __percpu *level0, atomic_long_t *items)
{
	unsigned int items_stride = percpu_counter_tree_items_stride(nr_counters);
	unsigned int i;
	int ret;

	for (i = 0; i < nr_counters; i++) {
		ret = __percpu_counter_tree_init(&counters[i], batch_size,
						 level0 ? level0 + i : NULL,
						 items ? items + i : NULL, items_stride);
		if (ret)
			return ret;
	}
	return 0;
}

/**
 * percpu_counter_tree_init_many() - Initialize many per-CPU counter trees.
 * @counters: An array of @nr_counters counters to initialize.
 *	      Their memory is provided by the caller.
 * @nr_counters: The number of counter trees to initialize
 * @batch_size: The batch size is the increment step at level 0 which triggers a
 *		carry propagation.
 *		The batch size is required to be greater than 1, a power of 2, and at
 *		most PERCPU_COUNTER_TREE_MAX_BATCH.
 * @gfp_flags: gfp flags to pass to the per-CPU and tree item allocators.
 *
 * Initialize many per-CPU counter trees, with a single allocation of
 * per-CPU counters and a single allocation of tree items for the
 * @nr_counters counters. The items at each tree position of the
 * @nr_counters counters share cache lines. When they fit in one cache
 * line (up to 8 counters on 64-bit with 64-byte cache lines), the tree
 * items come from a slab cache; beyond, their allocation is
 * rounded up to a power of 2, which can double it.
 *
 * Return:
 * * %0: Success
 * * %-EINVAL:		- Invalid @batch_size argument
 * * %-ENOMEM:		- Out of memory
 */
int percpu_counter_tree_init_many(struct percpu_counter_tree *counters,
				  unsigned int nr_counters, unsigned long batch_size,
				  gfp_t gfp_flags)
{
	s8 __percpu *level0;
	atomic_long_t *items;
	int ret;

	/* The tree shape is selected by percpu_counter_tree_subsystem_init() at boot. */
	if (WARN_ON_ONCE(tree_topology.nr_items == UINT_MAX))
		return -EINVAL;
	level0 = __alloc_percpu_gfp(nr_counters * sizeof(*level0),
				    __alignof__(*counters->level0), gfp_flags);
	if (!level0)
		return -ENOMEM;
	items = percpu_counter_tree_alloc_items(nr_counters, gfp_flags);
	if (!items) {
		ret = -ENOMEM;
		goto free_level0;
	}
	ret = __percpu_counter_tree_init_many(counters, nr_counters, batch_size, level0, items);
	if (!ret)
		return 0;
	percpu_counter_tree_free_items(items, nr_counters);
free_level0:
	free_percpu(level0);
	return ret;
}
EXPORT_SYMBOL_GPL(percpu_counter_tree_init_many);

/**
 * percpu_counter_tree_init() - Initialize one per-CPU counter tree.
 * @counter: Counter to initialize.
 *	     Its memory is provided by the caller.
 * @batch_size: The batch size is the increment step at level 0 which triggers a
 *		carry propagation.
 *		The batch size is required to be greater than 1, a power of 2, and at
 *		most PERCPU_COUNTER_TREE_MAX_BATCH.
 * @gfp_flags: gfp flags to pass to the per-CPU and tree item allocators.
 *
 * Initialize one per-CPU counter tree.
 *
 * Return:
 * * %0: Success
 * * %-EINVAL:		- Invalid @batch_size argument
 * * %-ENOMEM:		- Out of memory
 */
int percpu_counter_tree_init(struct percpu_counter_tree *counter,
			     unsigned long batch_size, gfp_t gfp_flags)
{
	return percpu_counter_tree_init_many(counter, 1, batch_size, gfp_flags);
}
EXPORT_SYMBOL_GPL(percpu_counter_tree_init);

/**
 * percpu_counter_tree_init_many_lazy() - Initialize many counters, without their tree.
 * @counters: An array of @nr_counters counters to initialize.
 *	      Their memory is provided by the caller.
 * @nr_counters: The number of counter trees to initialize
 * @batch_size: The batch size, as for percpu_counter_tree_init_many().
 *
 * Initialize counters without allocating their per-CPU counters and tree
 * items, for counters which may never need the scalability of the tree,
 * such as the RSS counters of a single-threaded process. Until
 * percpu_counter_tree_activate_many() allocates the tree, the counters
 * are exact: percpu_counter_tree_add_owner() adds to the bias, written
 * only by the owner, percpu_counter_tree_add() adds to the atomic bias,
 * and the precise and approximate sums read both in O(1).
 * percpu_counter_tree_destroy_many() and percpu_counter_tree_set() work on
 * counters which were never activated. The accuracy range of a lazily
 * initialized counter is the range of its tree, even before activation,
 * which keeps the comparison functions correct across an activation.
 * The batch size is fixed here: the activation only publishes the per-CPU
 * counters and the tree, so updaters read the batch size without ordering.
 *
 * Return:
 * * %0: Success
 * * %-EINVAL:		- Invalid @batch_size argument
 */
int percpu_counter_tree_init_many_lazy(struct percpu_counter_tree *counters,
				       unsigned int nr_counters, unsigned long batch_size)
{
	/* The tree shape is selected by percpu_counter_tree_subsystem_init() at boot. */
	if (WARN_ON_ONCE(tree_topology.nr_items == UINT_MAX))
		return -EINVAL;
	return __percpu_counter_tree_init_many(counters, nr_counters, batch_size, NULL, NULL);
}
EXPORT_SYMBOL_GPL(percpu_counter_tree_init_many_lazy);

/*
 * Publication of the tree
 *
 * The updaters and the readers of a counter take no lock, and may run
 * while percpu_counter_tree_activate_many() publishes its tree. The
 * activation allocates the per-CPU counters and the tree items zeroed,
 * then publishes two pointers per counter, each with a store-release:
 *
 * - the release of approx_sum: the root is zeroed before its pointer,
 * - the release of level0, after it: the per-CPU counters, the tree items
 *   and approx_sum are set before level0.
 *
 * level0 opens the tree to updates: until it is published, the tree is
 * empty. Each load of a published pointer pairs with one of the releases:
 *
 * - Update fast path, percpu_counter_tree_add() and
 *   percpu_counter_tree_add_owner(): READ_ONCE(level0), then the per-CPU
 *   counters through it. The address dependency pairs with the release of
 *   level0, as for rcu_dereference().
 * - Carry slow path: smp_rmb() after the read of level0, then
 *   READ_ONCE(approx_sum). Pairs with the release of level0, which
 *   follows the store of approx_sum.
 * - Precise sum: smp_load_acquire(level0), then approx_sum, the tree items
 *   and the per-CPU counters. Pairs with the release of level0.
 * - Approximate sum: READ_ONCE(approx_sum), then the root through it. The
 *   address dependency pairs with the release of approx_sum.
 * - percpu_counter_tree_active(): READ_ONCE(level0), without ordering. It
 *   only selects the update function, which reads level0 again, or lets
 *   the caller skip an activation which is complete.
 * - Activations: serialized by the caller, for instance with a mutex,
 *   which orders a complete activation before the check of level0 at the
 *   start of the next one.
 *
 * The update fast path has no acquire. smp_load_acquire() is free on
 * strongly ordered architectures such as x86, but on weakly ordered ones,
 * such as arm64, powerpc and RISC-V, it costs an ordering load or a barrier
 * on every update, to order a publication which happens at most once in
 * the life of a counter. The address dependency orders the per-CPU
 * counters, reached through level0, at no cost. It does not order
 * approx_sum, a separate field: the carry slow path, taken only when an
 * update carries, pays an smp_rmb() to read it, and the precise sum, which
 * reads the whole tree, reads level0 with an acquire.
 *
 * Missing the publication is harmless. An updater which reads level0 as
 * NULL adds to the bias (owner) or to the atomic bias (others), and a
 * reader which reads a NULL pointer sums the bias and the atomic bias:
 * the sums keep reading both after the activation, and the tree receives
 * no update before level0 is published.
 *
 * The read barriers between the levels of the precise sum are unrelated
 * to the publication: they order carries, see
 * percpu_counter_tree_precise_sum_unbiased().
 */

/**
 * percpu_counter_tree_activate_many() - Activate the tree of lazily initialized counters.
 * @counters: The array of @nr_counters counters passed to
 *	      percpu_counter_tree_init_many_lazy().
 * @nr_counters: The number of counters.
 * @gfp_flags: gfp flags to pass to the per-CPU and tree item allocators.
 *
 * Allocate the per-CPU counters and the tree items, and publish them:
 * later updates go to the tree, and the approximation then has the
 * accuracy of the tree. The updates made before activation stay in the
 * bias and the atomic bias, which the sums keep reading, so updates racing
 * with the activation are not lost: the bias only has the owner as writer,
 * and the atomic bias only receives atomic additions. Activating active
 * counters does nothing. @counters and @nr_counters must be those passed
 * to percpu_counter_tree_init_many_lazy().
 *
 * The activations of the same counters must be serialized by the caller,
 * for instance with a mutex: a concurrent activation would publish a second
 * tree and lose the updates made to the first. The counters are published
 * in order: once percpu_counter_tree_active() reports the last one active,
 * the activation is complete, and a caller can skip it and its
 * serialization. The caller may not observe the other counters active yet,
 * which only delays their updates going to the tree.
 *
 * Return:
 * * %0: Success
 * * %-EINVAL:		- @nr_counters differs from percpu_counter_tree_init_many_lazy()
 * * %-ENOMEM:		- Out of memory
 */
int percpu_counter_tree_activate_many(struct percpu_counter_tree *counters,
				      unsigned int nr_counters, gfp_t gfp_flags)
{
	s8 __percpu *level0;
	atomic_long_t *items, *root;
	unsigned int i;

	/* Serialized by the caller: a previous activation is complete. */
	if (counters[0].level0)
		return 0;
	/* The tree items are sized for @nr_counters, at the stride set by the initialization. */
	if (WARN_ON_ONCE(counters[0].items_stride !=
			 percpu_counter_tree_items_stride(nr_counters)))
		return -EINVAL;
	level0 = __alloc_percpu_gfp(nr_counters * sizeof(*level0),
				    __alignof__(*counters->level0), gfp_flags);
	if (!level0)
		return -ENOMEM;
	items = percpu_counter_tree_alloc_items(nr_counters, gfp_flags);
	if (!items) {
		free_percpu(level0);
		return -ENOMEM;
	}
	root = &items[(tree_topology.nr_items - 1) * counters[0].items_stride];
	/* See "Publication of the tree" above. */
	for (i = 0; i < nr_counters; i++) {
		/* Release of approx_sum: the root is zeroed before its pointer. */
		smp_store_release(&counters[i].approx_sum, root + i);
		/*
		 * Release of level0: the per-CPU counters, the tree items and
		 * approx_sum are set before level0, which opens the tree to
		 * updates.
		 */
		smp_store_release(&counters[i].level0, level0 + i);
	}
	return 0;
}
EXPORT_SYMBOL_GPL(percpu_counter_tree_activate_many);

/**
 * percpu_counter_tree_destroy_many() - Destroy many per-CPU counter trees.
 * @counters: Array of counters trees to destroy.
 * @nr_counters: The number of counter trees to destroy.
 *
 * Release internal resources allocated for @nr_counters per-CPU counter trees.
 * @counters and @nr_counters must be those passed to
 * percpu_counter_tree_init_many() or percpu_counter_tree_init_many_lazy().
 */
void percpu_counter_tree_destroy_many(struct percpu_counter_tree *counters,
				      unsigned int nr_counters)
{
	free_percpu(counters->level0);
	/*
	 * @nr_counters selects the allocator of the tree items: on a mismatch
	 * with the initialization, leak them rather than corrupt the allocator.
	 */
	if (WARN_ON_ONCE(counters->items_stride !=
			 percpu_counter_tree_items_stride(nr_counters)))
		return;
	/* The first item of the first counter is the base of the tree items, if allocated. */
	if (counters->approx_sum)
		percpu_counter_tree_free_items(percpu_counter_tree_items(counters), nr_counters);
}
EXPORT_SYMBOL_GPL(percpu_counter_tree_destroy_many);

/**
 * percpu_counter_tree_destroy() - Destroy one per-CPU counter tree.
 * @counter: Counter to destroy.
 *
 * Release internal resources allocated for one per-CPU counter tree.
 */
void percpu_counter_tree_destroy(struct percpu_counter_tree *counter)
{
	return percpu_counter_tree_destroy_many(counter, 1);
}
EXPORT_SYMBOL_GPL(percpu_counter_tree_destroy);

/*
 * Amount propagated by a counter holding @v: the multiples of @bit_mask,
 * truncated toward zero.
 */
static inline
long percpu_counter_tree_item_carry(long v, unsigned long bit_mask)
{
	if (v < 0)
		return -(long)(-(unsigned long)v & ~(bit_mask - 1));
	return (long)((unsigned long)v & ~(bit_mask - 1));
}

/*
 * It does not matter through which path the carry propagates up the
 * tree, therefore there is no need to disable preemption because the
 * cpu number is only used to favor cache locality.
 */
static
void percpu_counter_tree_add_slowpath(struct percpu_counter_tree *counter, long inc)
{
	unsigned int level_items, nr_levels = tree_topology.config->nr_levels,
		     level, n_arity_order;
	unsigned long bit_mask;
	unsigned int stride = counter->items_stride;
	unsigned int cpu = raw_smp_processor_id();
	atomic_long_t *root, *item;

	/*
	 * Pairs with the release of level0 in
	 * percpu_counter_tree_activate_many(), which follows the store of
	 * approx_sum. The caller read level0, but approx_sum is a separate
	 * field, which the address dependency of that read does not order:
	 * read it after level0.
	 */
	smp_rmb();
	root = READ_ONCE(counter->approx_sum);
	item = root - (tree_topology.nr_items - 1) * stride;
	n_arity_order = tree_topology.config->n_arity_order[0];
	bit_mask = percpu_counter_tree_batch_size(counter) << n_arity_order;
	level_items = 1U << (tree_topology.nr_cpus_order - n_arity_order);

	for (level = 1; level < nr_levels; level++) {
		/*
		 * A multiple of bit_mask would leave the residual of this
		 * level unchanged: propagate it to the next level directly.
		 * This trades the hysteresis of this level for not touching
		 * its cache line.
		 */
		if (abs(inc) & (bit_mask - 1)) {
			atomic_long_t *count = &item[(cpu & (level_items - 1)) * stride];
			long res;

			/*
			 * Add the carry to the residual of this level, then
			 * extract the multiples of bit_mask of the residual,
			 * truncated toward zero, and propagate them to the
			 * next level, which leaves the residual within
			 * (-bit_mask, +bit_mask).
			 *
			 * The extraction is a cmpxchg on the value returned by
			 * the addition, whose cache line is already exclusive,
			 * so it usually succeeds at once. When a concurrent
			 * updater changed the residual in between, the cmpxchg
			 * fails and the carry is recomputed from the current
			 * value, which is 0 if the other updater extracted it.
			 * Each carry is thus extracted once, from an exact
			 * value: the residual only exceeds its range by the
			 * additions in flight, and is within range once all
			 * the updaters are done. An addition which does not
			 * carry stays a single atomic operation, which cannot
			 * fail.
			 *
			 * Relaxed ordering is sufficient for the residual
			 * itself: the reasoning above only involves this
			 * atomic, whose modifications all CPUs see in the same
			 * order, each read-modify-write reading the value
			 * written by the one just before it, whatever its
			 * ordering. Ordering only matters relative to the other
			 * levels, for the precise sum: the release ordering of
			 * the addition orders the departure of the carry from
			 * the level below before its arrival here.
			 */
			res = atomic_long_add_return_release(inc, count);
			do {
				inc = percpu_counter_tree_item_carry(res, bit_mask);
				if (likely(!inc))
					return;
			} while (!atomic_long_try_cmpxchg_relaxed(count, &res, res - inc));
		}
		item += level_items * stride;
		n_arity_order = tree_topology.config->n_arity_order[level];
		level_items >>= n_arity_order;
		bit_mask <<= n_arity_order;
	}
	/* The atomic API has no add with release ordering without a return value. */
	atomic_long_add_return_release(inc, root);
}

/* Add @inc to the per-CPU counters @level0 of an activated tree. */
static inline
void percpu_counter_tree_add_tree(struct percpu_counter_tree *counter,
				  s8 __percpu *level0, long inc)
{
	unsigned long bit_mask = percpu_counter_tree_batch_size(counter);
	s8 old, new;
	long v, carry;

	/*
	 * Keep the residual within (-batch, +batch) and propagate its
	 * multiples of the batch size, truncated toward zero. This
	 * hysteresis prevents updates going back and forth around a batch
	 * boundary from propagating each time.
	 *
	 * Preemption stays enabled: the update may go to the level 0
	 * counter of any CPU, and the cmpxchg validates the value read.
	 * After a migration or an interrupt between the read and the
	 * cmpxchg, the cmpxchg either fails and retries with the current
	 * residual of the current CPU, or succeeds on a residual equal to
	 * the value read, which the new value correctly replaces. Each
	 * this_cpu operation is itself safe against preemption and
	 * interrupts, and the carry may propagate from any CPU, see
	 * percpu_counter_tree_add_slowpath().
	 */
	old = this_cpu_read(*level0);
	do {
		v = old + inc;
		carry = percpu_counter_tree_item_carry(v, bit_mask);
		new = (s8)(v - carry);
	} while (!this_cpu_try_cmpxchg(*level0, &old, new));
	if (likely(!carry))
		return;
	percpu_counter_tree_add_slowpath(counter, carry);
}

/**
 * percpu_counter_tree_add() - Add to a per-CPU counter tree.
 * @counter: Counter added to.
 * @inc: Increment value (either positive or negative).
 *
 * Add @inc to a per-CPU counter tree. This is a fast-path which will
 * typically increment per-CPU counters as long as there is no carry
 * greater or equal to the counter tree batch size. Before the tree is
 * activated, add to the atomic bias.
 */
void percpu_counter_tree_add(struct percpu_counter_tree *counter, long inc)
{
	/*
	 * Pairs with the release of level0 in
	 * percpu_counter_tree_activate_many(): the per-CPU counters are only
	 * reached through this pointer, and the address dependency orders
	 * their initialization before their use, as for rcu_dereference().
	 * No acquire, see "Publication of the tree".
	 */
	s8 __percpu *level0 = READ_ONCE(counter->level0);

	if (!level0) {
		atomic_long_add(inc, &counter->atomic_bias);
		return;
	}
	percpu_counter_tree_add_tree(counter, level0, inc);
}
EXPORT_SYMBOL_GPL(percpu_counter_tree_add);

/**
 * percpu_counter_tree_add_owner() - Add to a per-CPU counter tree as its owner.
 * @counter: Counter added to.
 * @inc: Increment value (either positive or negative).
 *
 * Like percpu_counter_tree_add(), but before the tree is activated, add
 * to the bias without atomic instruction. The calls for a given counter
 * must not run concurrently with each other: they come from a single
 * context, the owner chosen by the user of the counter, such as a task.
 */
void percpu_counter_tree_add_owner(struct percpu_counter_tree *counter, long inc)
{
	/* Address dependency, as in percpu_counter_tree_add(). */
	s8 __percpu *level0 = READ_ONCE(counter->level0);

	if (!level0) {
		WRITE_ONCE(counter->bias, (long)((unsigned long)READ_ONCE(counter->bias) + inc));
		return;
	}
	percpu_counter_tree_add_tree(counter, level0, inc);
}
EXPORT_SYMBOL_GPL(percpu_counter_tree_add_owner);

/*
 * The precise sum is the sum of the residuals of all tree levels. A carry
 * leaves a level before reaching a higher level, and the release ordering
 * of its arrival orders its departure before it. Reading the levels from
 * the root down, separated by read barriers, therefore never counts a carry
 * twice. A carry in flight during the sum may be missed: the result is
 * precise for a counter without concurrent updates.
 */
static
long percpu_counter_tree_precise_sum_unbiased(struct percpu_counter_tree *counter)
{
	unsigned int nr_levels = tree_topology.config->nr_levels, i,
		     level_items[MAX_NR_LEVELS], stride = counter->items_stride;
	/*
	 * Pairs with the release of level0 in
	 * percpu_counter_tree_activate_many(): approx_sum, the tree items and
	 * the per-CPU counters read below are set before level0.
	 */
	s8 __percpu *level0 = smp_load_acquire(&counter->level0);
	unsigned long sum = atomic_long_read(&counter->atomic_bias);
	atomic_long_t *item;
	int level, cpu;

	/* Before activation, the atomic bias holds the whole unbiased sum. */
	if (!level0)
		return (long)sum;
	/* Number of items of each level above level 0. */
	level_items[0] = 1U << tree_topology.nr_cpus_order;
	for (level = 1; level < (int)nr_levels; level++)
		level_items[level] = level_items[level - 1] >>
				     tree_topology.config->n_arity_order[level - 1];
	/* Published before level0, so ordered by its acquire. */
	item = READ_ONCE(counter->approx_sum);
	sum += atomic_long_read(item);
	smp_rmb();	/* Read each level before the level below it. */
	/* The items of each level precede those of the level above it. */
	for (level = (int)nr_levels - 1; level >= 1; level--) {
		item -= level_items[level] * stride;
		for (i = 0; i < level_items[level]; i++)
			sum += atomic_long_read(&item[i * stride]);
		smp_rmb();	/* Read this level before the level below it. */
	}
	for_each_possible_cpu(cpu)
		sum += (long)READ_ONCE(*per_cpu_ptr(level0, cpu));
	return (long)sum;
}

/**
 * percpu_counter_tree_precise_sum() - Return precise counter sum.
 * @counter: The counter to sum.
 *
 * Querying the precise sum is relatively expensive because it needs to
 * iterate over all CPUs.
 * This is meant to be used when accuracy is preferred over speed.
 *
 * The sum is exact when no update runs concurrently. It takes no lock, so
 * a concurrent update which carries from level 0 to the levels above can
 * be counted in part: the sum may read the residual it left below and miss
 * its carry on the way up, an error in either direction of at most the
 * carries in flight. A carry is never counted twice.
 *
 * Return: The current precise counter sum.
 */
long percpu_counter_tree_precise_sum(struct percpu_counter_tree *counter)
{
	return percpu_counter_tree_precise_sum_unbiased(counter) + READ_ONCE(counter->bias);
}
EXPORT_SYMBOL_GPL(percpu_counter_tree_precise_sum);

/*
 * The comparison between counters is on the wrapped difference. It is valid
 * only when the two counters differ by less than LONG_MAX, and results are
 * undefined beyond that.
 */
static inline
long wrapped_delta(long a, long b)
{
	return (long)((unsigned long)a - (unsigned long)b);
}

/*
 * Each counter's approximation lies within its accuracy of its precise sum,
 * in both directions. The comparison only observes the approximations, so
 * it must reason about the possible precise values around each observed
 * approximation, which lie within [approx - accuracy, approx + accuracy]:
 *
 *                 precise_a range
 *            <-----------+----------->
 *         accuracy_a     |     accuracy_a
 *                    approx_a
 *                        |                         precise_b range
 *                        |                    <-----------+----------->
 *                        |                 accuracy_b     |     accuracy_b
 *                        |                             approx_b
 *                        |                                |
 *                        |<------------ gap ------------->|
 *  ----------------------+--------------------------------+-------------> values
 *
 * The two precise ranges are disjoint, and the comparison can return a
 * definitive answer, only once the gap between approximations exceeds the
 * sum of the two accuracies:
 *
 *   gap > accuracy_a + accuracy_b
 *
 * When comparing against a plain value, the value has zero width, so the
 * margin reduces to the accuracy of the counter: the value is above the
 * counter once it exceeds approx + accuracy, and below it once it is less
 * than approx - accuracy.
 */
static
int compare_delta(long delta, unsigned long accuracy)
{
	if (delta >= 0) {
		if (delta <= accuracy)
			return 0;
		else
			return 1;
	} else {
		if (-delta <= accuracy)
			return 0;
		else
			return -1;
	}
}

/**
 * percpu_counter_tree_approximate_compare - Approximated comparison of two counter trees.
 * @a: First counter to compare.
 * @b: Second counter to compare.
 *
 * Evaluate an approximate comparison of two counter trees. This approximation
 * comparison is fast, and provides an accurate answer if the counters are
 * found to be either less than or greater than the other. However, if the
 * approximated comparison returns 0, the counters respective sums are found to
 * be within the two counters accuracy range. The two counters are read
 * independently; the result is not an atomic snapshot of both.
 * The comparison between counters is on the wrapped difference. It is valid
 * only when the two counters differ by less than LONG_MAX, and results are
 * undefined beyond that.
 *
 * Return:
 * * %0		- Counters @a and @b do not differ by more than the sum of their respective
 *                accuracy ranges.
 * * %-1	- Counter @a less than counter @b.
 * * %1		- Counter @a is greater than counter @b.
 */
int percpu_counter_tree_approximate_compare(struct percpu_counter_tree *a,
					    struct percpu_counter_tree *b)
{
	unsigned long accuracy = percpu_counter_tree_accuracy(a) + percpu_counter_tree_accuracy(b);

	/* See the range geometry above compare_delta(). */
	return compare_delta(wrapped_delta(percpu_counter_tree_approximate_sum(a),
					   percpu_counter_tree_approximate_sum(b)),
			     accuracy);
}
EXPORT_SYMBOL_GPL(percpu_counter_tree_approximate_compare);

/**
 * percpu_counter_tree_approximate_compare_value - Approximated comparison of a counter tree
 *						   against a given value.
 * @counter: Counter to compare.
 * @v: Value to compare.
 *
 * Evaluate an approximate comparison of a counter tree against a given value.
 * This approximation comparison is fast, and provides an accurate
 * answer if the counter is found to be either less than or greater
 * than the value. However, if the approximated comparison returns
 * 0, the value is within the counter accuracy range.
 * The comparison between counters is on the wrapped difference. It is valid
 * only when @v and the counter differ by less than LONG_MAX, and results are
 * undefined beyond that.
 *
 * Return:
 * * %0		- The value @v is within the accuracy range of the counter.
 * * %-1	- The value @v is less than the counter.
 * * %1		- The value @v is greater than the counter.
 */
int percpu_counter_tree_approximate_compare_value(struct percpu_counter_tree *counter, long v)
{
	unsigned long accuracy = percpu_counter_tree_accuracy(counter);

	/* See the range geometry above compare_delta(). */
	return compare_delta(wrapped_delta(v, percpu_counter_tree_approximate_sum(counter)),
			     accuracy);
}
EXPORT_SYMBOL_GPL(percpu_counter_tree_approximate_compare_value);

/**
 * percpu_counter_tree_precise_compare - Precise comparison of two counter trees.
 * @a: First counter to compare.
 * @b: Second counter to compare.
 *
 * Evaluate a precise comparison of two counter trees. As an optimization, it
 * uses the approximate counter comparison to quickly compare counters which
 * are far apart. Only cases where counter sums are within the accuracy range
 * require precise counter sums. The two counters are read independently; the
 * result is not an atomic snapshot of both. With concurrent updates, the
 * precise sums can count updates in part, see
 * percpu_counter_tree_precise_sum().
 * The comparison between counters is on the wrapped difference. It is valid
 * only when the two counters differ by less than LONG_MAX, and results are
 * undefined beyond that.
 *
 * Return:
 * * %0		- Counters are equal.
 * * %-1	- Counter @a less than counter @b.
 * * %1		- Counter @a is greater than counter @b.
 */
int percpu_counter_tree_precise_compare(struct percpu_counter_tree *a,
					struct percpu_counter_tree *b)
{
	long count_a = percpu_counter_tree_approximate_sum(a),
	     count_b = percpu_counter_tree_approximate_sum(b);
	unsigned long accuracy_a = percpu_counter_tree_accuracy(a),
		      accuracy_b = percpu_counter_tree_accuracy(b);
	long delta = wrapped_delta(count_a, count_b);
	int res;

	/* See the range geometry above compare_delta(). */
	res = compare_delta(delta, accuracy_a + accuracy_b);
	/* The values are distanced enough for an accurate approximated comparison. */
	if (res)
		return res;

	/*
	 * The approximated comparison is within the accuracy range, therefore at least one
	 * precise sum is needed. Sum the counter which has the largest accuracy first.
	 */
	if (accuracy_b < accuracy_a) {
		count_a = percpu_counter_tree_precise_sum(a);
		/* precise_a - approx_b: only b's precise range remains. */
		res = compare_delta(wrapped_delta(count_a, count_b), accuracy_b);
		if (res)
			return res;
		/* Precise sum of second counter is required. */
		count_b = percpu_counter_tree_precise_sum(b);
	} else {
		count_b = percpu_counter_tree_precise_sum(b);
		/* approx_a - precise_b: only a's precise range remains. */
		res = compare_delta(wrapped_delta(count_a, count_b), accuracy_a);
		if (res)
			return res;
		/* Precise sum of second counter is required. */
		count_a = percpu_counter_tree_precise_sum(a);
	}
	delta = wrapped_delta(count_a, count_b);
	if (delta < 0)
		return -1;
	if (delta > 0)
		return 1;
	return 0;
}
EXPORT_SYMBOL_GPL(percpu_counter_tree_precise_compare);

/**
 * percpu_counter_tree_precise_compare_value - Precise comparison of a counter tree against
 *					       a given value.
 * @counter: Counter to compare.
 * @v: Value to compare.
 *
 * Evaluate a precise comparison of a counter tree against a given value. As
 * an optimization, it uses the approximate counter comparison to quickly
 * identify whether the counter and value are far apart. Only cases where the
 * value is within the counter accuracy range require a precise counter sum.
 * With concurrent updates, the precise sum can count updates in part, see
 * percpu_counter_tree_precise_sum().
 * The comparison between counters is on the wrapped difference. It is valid
 * only when @v and the counter differ by less than LONG_MAX, and results are
 * undefined beyond that.
 *
 * Return:
 * * %0		- The value @v is equal to the counter.
 * * %-1	- The value @v is less than the counter.
 * * %1		- The value @v is greater than the counter.
 */
int percpu_counter_tree_precise_compare_value(struct percpu_counter_tree *counter, long v)
{
	long count = percpu_counter_tree_approximate_sum(counter), delta;
	unsigned long accuracy = percpu_counter_tree_accuracy(counter);
	int res;

	/* See the range geometry above compare_delta(). */
	res = compare_delta(wrapped_delta(v, count), accuracy);
	/* The values are distanced enough for an accurate approximated comparison. */
	if (res)
		return res;

	/* Precise sum is required. */
	count = percpu_counter_tree_precise_sum(counter);
	delta = wrapped_delta(v, count);
	if (delta < 0)
		return -1;
	if (delta > 0)
		return 1;
	return 0;
}
EXPORT_SYMBOL_GPL(percpu_counter_tree_precise_compare_value);

/**
 * percpu_counter_tree_set - Set the counter tree sum to a given value.
 * @counter: Counter to set.
 * @v: Value to set.
 *
 * Set the counter sum to a given value. It can be useful for instance
 * to reset the counter sum to 0. Note that even after setting the
 * counter sum to a given value, the counter sum approximation can
 * return any value within the accuracy range around that value.
 *
 * The caller is responsible for preventing concurrent updates and
 * precise sums of @counter while it is being set.
 */
void percpu_counter_tree_set(struct percpu_counter_tree *counter, long v)
{
	WRITE_ONCE(counter->bias, v - percpu_counter_tree_precise_sum_unbiased(counter));
}
EXPORT_SYMBOL_GPL(percpu_counter_tree_set);

/**
 * percpu_counter_tree_approximate_accuracy - Query the accuracy of a counter tree.
 * @counter: Counter to query.
 *
 * The approximation differs from the precise sum by the residuals not yet propagated
 * up the tree. The residuals of all levels are symmetric, so the approximation is
 * within the same distance of the precise sum in both directions.
 * The accuracy is derived from the hardware topology and the counter tree batch size.
 * It is invariant for a given counter tree.
 * The accuracy describes a settled counter: one with no update in progress.
 * An update in flight between its per-CPU update and its carry propagation is not covered.
 * Using this function should not be typically required, see the following functions instead:
 * * percpu_counter_tree_approximate_compare(),
 * * percpu_counter_tree_approximate_compare_value(),
 * * percpu_counter_tree_precise_compare(),
 * * percpu_counter_tree_precise_compare_value().
 *
 * Return: The accuracy of the counter: the approximate sum of a settled counter
 * is within [precise sum - accuracy, precise sum + accuracy].
 */
unsigned long percpu_counter_tree_approximate_accuracy(struct percpu_counter_tree *counter)
{
	return percpu_counter_tree_accuracy(counter);
}
EXPORT_SYMBOL_GPL(percpu_counter_tree_approximate_accuracy);

/* Calculate the number of tree items and the accuracy multiplier. */
static
void __init calculate_topology(void)
{
	unsigned int nr_levels = tree_topology.config->nr_levels, level;
	unsigned int level_items = 1U << tree_topology.nr_cpus_order;
	unsigned long batch_size = 1;

	/* The root, and the items of each level above level 0. */
	tree_topology.nr_items = 1;
	/* One batch per level 0 counter: see percpu_counter_tree_accuracy(). */
	tree_topology.accuracy_multiplier = nr_cpu_ids;

	/*
	 * The multiplier is derived from a batch size of 1 to speed up
	 * calculating the accuracy at tree initialization. A level N
	 * counter holds at most BATCH(level N) - BATCH(level N - 1) in
	 * either direction.
	 */
	for (level = 1; level < nr_levels; level++) {
		unsigned int n_arity_order = tree_topology.config->n_arity_order[level - 1];

		level_items >>= n_arity_order;
		tree_topology.nr_items += level_items;
		tree_topology.accuracy_multiplier +=
			((batch_size << n_arity_order) - batch_size) * level_items;
		batch_size <<= n_arity_order;
	}
}

#if IS_ENABLED(CONFIG_KUNIT)
/*
 * Validate the configuration table: the n-arity orders of each entry must
 * sum to its order.
 *
 * Return: The number of invalid entries.
 */
unsigned int percpu_counter_tree_check_config(void)
{
	unsigned int order, nr_errors = 0;

	for (order = 0; order < ARRAY_SIZE(per_nr_cpu_order_config); order++) {
		const struct counter_config *c = &per_nr_cpu_order_config[order];
		unsigned int level, shift = 0;

		for (level = 0; level < c->nr_levels; level++)
			shift += c->n_arity_order[level];
		if (shift != order) {
			pr_err("order %u: arity sum %u\n", order, shift);
			nr_errors++;
		}
	}
	return nr_errors;
}
EXPORT_SYMBOL_IF_KUNIT(percpu_counter_tree_check_config);

/*
 * Check the invariants of a settled counter (no update in progress):
 * - a level 0 counter is within (-BATCH(level 0), +BATCH(level 0)),
 * - a counter at level N, above level 0, holds a multiple of
 *   BATCH(level N - 1) within (-BATCH(level N), +BATCH(level N)),
 * - the root holds a multiple of the batch of the last level.
 * Before activation, a lazily initialized counter has no tree to check.
 *
 * Return: The number of violated invariants.
 */
unsigned int percpu_counter_tree_check_residuals(struct percpu_counter_tree *counter)
{
	unsigned int nr_levels = tree_topology.config->nr_levels, level, level_items,
		     nr_errors = 0, stride = counter->items_stride;
	unsigned long lo = percpu_counter_tree_batch_size(counter);
	atomic_long_t *item;
	long root;
	int cpu;

	if (!counter->level0)
		return 0;
	item = percpu_counter_tree_items(counter);
	for_each_possible_cpu(cpu) {
		long v = READ_ONCE(*per_cpu_ptr(counter->level0, cpu));

		if (v <= -(long)lo || v >= (long)lo) {
			pr_err("cpu %d: level 0 residual %ld, batch %lu\n", cpu, v, lo);
			nr_errors++;
		}
	}
	level_items = 1U << tree_topology.nr_cpus_order;
	for (level = 1; level < nr_levels; level++) {
		unsigned int n_arity_order = tree_topology.config->n_arity_order[level - 1], i;
		unsigned long hi = lo << n_arity_order;

		level_items >>= n_arity_order;
		for (i = 0; i < level_items; i++) {
			long v = atomic_long_read(&item[i * stride]);

			if (v <= -(long)hi || v >= (long)hi || ((unsigned long)v & (lo - 1))) {
				pr_err("level %u item %u: residual %ld, batch %lu\n",
				       level, i, v, hi);
				nr_errors++;
			}
		}
		item += level_items * stride;
		lo = hi;
	}
	root = atomic_long_read(counter->approx_sum);
	if ((unsigned long)root & (lo - 1)) {
		pr_err("root %ld: not a multiple of %lu\n", root, lo);
		nr_errors++;
	}
	return nr_errors;
}
EXPORT_SYMBOL_IF_KUNIT(percpu_counter_tree_check_residuals);

/*
 * Copy the state of counter @src into counter @dst, initialized with the
 * same batch size and without concurrent updates: their per-CPU counters,
 * tree items and biases. Both trees are active.
 */
void percpu_counter_tree_copy_state(struct percpu_counter_tree *dst,
				    struct percpu_counter_tree *src)
{
	atomic_long_t *dst_items = percpu_counter_tree_items(dst),
		      *src_items = percpu_counter_tree_items(src);
	unsigned int pos;
	int cpu;

	for_each_possible_cpu(cpu)
		*per_cpu_ptr(dst->level0, cpu) = *per_cpu_ptr(src->level0, cpu);
	for (pos = 0; pos < tree_topology.nr_items; pos++)
		atomic_long_set(&dst_items[pos * dst->items_stride],
				atomic_long_read(&src_items[pos * src->items_stride]));
	dst->bias = src->bias;
	atomic_long_set(&dst->atomic_bias, atomic_long_read(&src->atomic_bias));
}
EXPORT_SYMBOL_IF_KUNIT(percpu_counter_tree_copy_state);
#endif

/* Room for the fan-out of each level, up to 7 digits and a comma, and a NUL. */
#define SHAPE_BUF_SIZE	(8 * MAX_NR_LEVELS + 1)

/* Format the fan-out of each level, from level 0 to the root, e.g. "8,4,8". */
static
int format_shape(char *buf, size_t size, const struct counter_config *config)
{
	unsigned int level;
	int len = 0;

	for (level = 0; level < config->nr_levels; level++)
		len += scnprintf(buf + len, size - len, "%s%u", level ? "," : "",
				 1U << config->n_arity_order[level]);
	return len;
}

static
int arity_param_set(const char *val, const struct kernel_param *kp)
{
	/* Applied once by percpu_counter_tree_subsystem_init(). */
	if (tree_topology.config)
		return -EBUSY;
	arity_param = val;
	return 0;
}

static
int arity_param_get(char *buffer, const struct kernel_param *kp)
{
	int len;

	if (!tree_topology.config)
		return sprintf(buffer, "%s\n", arity_param ? : "");
	/* The buffer is a page, larger than any shape and its newline. */
	len = format_shape(buffer, SHAPE_BUF_SIZE, tree_topology.config);
	buffer[len++] = '\n';
	return len;
}

static const struct kernel_param_ops arity_param_ops = {
	.set = arity_param_set,
	.get = arity_param_get,
};

/*
 * percpu_counter_tree.arity=<fan-out>[,<fan-out>...] overrides the tree
 * shape chosen for nr_cpu_ids, to compare shapes on a given machine.
 * Reading the parameter shows the shape in use.
 */
module_param_cb(arity, &arity_param_ops, NULL, 0444);
MODULE_PARM_DESC(arity, "Fan-out of each tree level from level 0 to the root, e.g. 8,4,8");

/*
 * Parse a list of fan-outs, from level 0 to the root, into @config. Each
 * fan-out is a power of 2 of at least 2, and their product is nr_cpu_ids
 * rounded up to a power of 2.
 *
 * Return: 0 on success, -EINVAL if the list does not describe a tree
 * covering the CPU numbers below nr_cpu_ids.
 */
static
int __init parse_arity(const char *str, struct counter_config *config)
{
	int ints[MAX_NR_LEVELS + 2];	/* Count, levels, and one extra to detect overflow. */
	unsigned int level, order = 0;

	if (*get_options(str, ARRAY_SIZE(ints), ints) || ints[0] > MAX_NR_LEVELS)
		return -EINVAL;
	config->nr_levels = ints[0];
	for (level = 0; level < config->nr_levels; level++) {
		int fan_out = ints[level + 1];

		if (fan_out < 2 || !is_power_of_2(fan_out))
			return -EINVAL;
		config->n_arity_order[level] = ilog2(fan_out);
		order += config->n_arity_order[level];
		if (order > tree_topology.nr_cpus_order)
			return -EINVAL;
	}
	if (order != tree_topology.nr_cpus_order)
		return -EINVAL;
	return 0;
}

int __init percpu_counter_tree_subsystem_init(void)
{
	const char *origin = "default";
	char shape[SHAPE_BUF_SIZE] = "";

	tree_topology.nr_cpus_order = get_count_order(nr_cpu_ids);
	if (tree_topology.nr_cpus_order >= ARRAY_SIZE(per_nr_cpu_order_config)) {
		pr_err("Unsupported number of CPUs (%u)\n", nr_cpu_ids);
		return -EINVAL;
	}
	tree_topology.config = &per_nr_cpu_order_config[tree_topology.nr_cpus_order];
	if (arity_param && *arity_param) {
		if (parse_arity(arity_param, &tree_topology.boot_config)) {
			pr_warn("Invalid arity \"%s\" for %u CPUs, using the default\n",
				arity_param, nr_cpu_ids);
		} else {
			tree_topology.config = &tree_topology.boot_config;
			origin = "boot parameter";
		}
	}
	calculate_topology();
	tree_topology.items_cachep =
		kmem_cache_create("percpu_counter_tree_items",
				  tree_topology.nr_items * SMP_CACHE_BYTES,
				  SMP_CACHE_BYTES, SLAB_PANIC, NULL);
	format_shape(shape, sizeof(shape), tree_topology.config);
	pr_info("%u CPUs, fan-out %s (%s), %u tree items\n",
		nr_cpu_ids, tree_topology.nr_cpus_order ? shape : "none", origin,
		tree_topology.nr_items);
	return 0;
}
