// SPDX-License-Identifier: GPL-2.0+ OR MIT
/*
 * Userspace test of the hierarchical per-CPU counters (lib/percpu_counter_tree.c).
 *
 * The library is built with a simulated topology, which allows testing
 * every supported tree configuration on a single machine, whereas the
 * KUnit test only covers the topology of the machine it runs on.
 *
 * For each simulated number of CPUs and batch size:
 *
 * - Saturate the counter upward and downward: leave every tree item with
 *   its largest positive (or negative) residual which has not been
 *   propagated yet, check that the distance between the precise and
 *   approximate sums matches the residual capacity of the tree in that
 *   direction, and check the accuracy range contract and the comparison
 *   functions in these worst-case states.
 *
 * - Random walk: apply random increments from random CPUs to two counters,
 *   and check the accuracy range contract and the comparison functions
 *   against shadow sums after each increment.
 *
 * - Lazy counter: random owner and atomic updates of a lazily initialized
 *   counter, exact before its tree is activated halfway, with the same
 *   checks after each increment.
 *
 * - Limit counters: random additions and subtractions near the limit,
 *   which must fit exactly when the value stays within it. Hooks stall an
 *   addition in flight to check the cases where the sums race with it.
 *   Each test runs again with both counters about to wrap, as unsigned and
 *   as signed values.
 *
 * The test is single-threaded, so it only covers settled counters.
 */

#include <stdarg.h>
#include "hpcc_shim.h"

unsigned int nr_cpu_ids;
unsigned int hpcc_current_cpu;
unsigned long hpcc_nr_warnings;
long hpcc_nr_allocs;
atomic_long_t *hpcc_stall_item;
void (*hpcc_stall_fn)(void);

#include "../../../lib/percpu_counter_tree.c"

#define MAX_REPORTED_FAILURES	10

static unsigned long nr_case_failures;

#define CHECK(cond) do {							\
	if (!(cond)) {								\
		if (nr_case_failures++ < MAX_REPORTED_FAILURES)			\
			printf("#   %s:%d: check failed: %s\n",			\
			       __FILE__, __LINE__, #cond);			\
	}									\
} while (0)

static uint64_t rng_state = 88172645463325252ULL;

static uint64_t rnd(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return rng_state;
}

static int sign(long v)
{
	return (v > 0) - (v < 0);
}

struct test_counter {
	struct percpu_counter_tree tree;
	long shadow;	/* Expected precise sum. */
};

int scnprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list args;
	int len;

	if (!size)
		return 0;
	va_start(args, fmt);
	len = vsnprintf(buf, size, fmt, args);
	va_end(args);
	return len < (int)size ? len : (int)size - 1;
}

/* Comma-separated integers, without the ranges of lib/cmdline.c. */
char *get_options(const char *str, int nints, int *ints)
{
	char *end;
	int i = 0;

	while (i < nints - 1 && *str) {
		long v = strtol(str, &end, 0);

		if (end == str)
			break;
		ints[++i] = v;
		str = end;
		if (*str != ',')
			break;
		str++;
	}
	ints[0] = i;
	return (char *)str;
}

static void hpcc_init_topology(unsigned int nr_cpus)
{
	nr_cpu_ids = nr_cpus;
	/* Reset the state accumulated by a prior subsystem initialization. */
	kmem_cache_destroy(tree_topology.items_cachep);
	memset(&tree_topology, 0, sizeof(tree_topology));
	tree_topology.nr_items = UINT_MAX;
	CHECK(!percpu_counter_tree_subsystem_init());
}

static int test_counter_init(struct test_counter *c, unsigned long batch_size)
{
	c->shadow = 0;
	return percpu_counter_tree_init(&c->tree, batch_size, GFP_KERNEL);
}

static void test_counter_destroy(struct test_counter *c)
{
	percpu_counter_tree_destroy(&c->tree);
}

static void test_counter_add(struct test_counter *c, unsigned int cpu, long inc)
{
	hpcc_current_cpu = cpu;
	percpu_counter_tree_add(&c->tree, inc);
	c->shadow += inc;
}

static void test_counter_set(struct test_counter *c, long v)
{
	percpu_counter_tree_set(&c->tree, v);
	c->shadow = v;
}

/*
 * Residual capacity of the tree items above level 0, in either direction:
 * sum of the largest residual which each item can hold without propagating
 * a carry. A level L item only receives multiples of the level L - 1 carry,
 * and propagates once its residual reaches the level L carry, so it holds
 * at most bit(L) - bit(L - 1) in either direction.
 */
static unsigned long tree_item_capacity(unsigned long batch_size)
{
	unsigned long capacity = 0, lo = batch_size, hi;
	unsigned int level, level_items;

	level_items = 1U << tree_topology.nr_cpus_order;
	for (level = 1; level < tree_topology.config->nr_levels; level++) {
		unsigned int n_arity_order = tree_topology.config->n_arity_order[level - 1];

		level_items >>= n_arity_order;
		hi = lo << n_arity_order;
		capacity += level_items * (hi - lo);
		lo = hi;
	}
	return capacity;
}

/*
 * Residual capacity of the tree, in either direction: a level 0 counter
 * holds at most batch_size - 1 on top of the item capacity.
 */
static unsigned long tree_residual_capacity(unsigned long batch_size)
{
	return nr_cpu_ids * (batch_size - 1) + tree_item_capacity(batch_size);
}

/* Copy the state of counter @src into counter @dst, of the same topology. */
static void test_counter_copy(struct test_counter *dst, struct test_counter *src)
{
	percpu_counter_tree_copy_state(&dst->tree, &src->tree);
	dst->shadow = src->shadow;
}

/*
 * Check the accuracy range contract, and the comparison of the counter
 * against values around its precise sum and its possible precise range.
 */
static void check_counter(struct test_counter *c)
{
	struct percpu_counter_tree *t = &c->tree;
	long approx = percpu_counter_tree_approximate_sum(t);
	long precise = c->shadow, precise_min, precise_max;
	unsigned long accuracy;
	long values[7];
	unsigned int i;

	CHECK(percpu_counter_tree_precise_sum(t) == precise);
	CHECK(!percpu_counter_tree_check_residuals(t));

	/* precise - accuracy <= approx <= precise + accuracy */
	accuracy = percpu_counter_tree_approximate_accuracy(t);
	CHECK(approx >= precise - (long)accuracy);
	CHECK(approx <= precise + (long)accuracy);

	percpu_counter_tree_approximate_min_max_range(approx, accuracy,
						      &precise_min, &precise_max);
	CHECK(precise >= precise_min);
	CHECK(precise <= precise_max);

	/* Approximate comparison is definitive exactly outside of the precise range. */
	CHECK(percpu_counter_tree_approximate_compare_value(t, precise_min - 1) == -1);
	CHECK(percpu_counter_tree_approximate_compare_value(t, precise_min) == 0);
	CHECK(percpu_counter_tree_approximate_compare_value(t, precise_max) == 0);
	CHECK(percpu_counter_tree_approximate_compare_value(t, precise_max + 1) == 1);

	values[0] = precise - 1;
	values[1] = precise;
	values[2] = precise + 1;
	values[3] = precise_min - 1;
	values[4] = precise_min;
	values[5] = precise_max;
	values[6] = precise_max + 1;
	for (i = 0; i < ARRAY_SIZE(values); i++) {
		long v = values[i];
		int expect = sign(v - precise);
		int res = percpu_counter_tree_approximate_compare_value(t, v);

		CHECK(res == 0 || res == expect);
		CHECK(percpu_counter_tree_precise_compare_value(t, v) == expect);
	}
}

/* Check the comparison of two counters against their shadow sums. */
static void check_counter_pair(struct test_counter *a, struct test_counter *b)
{
	int expect = sign(a->shadow - b->shadow);
	int res = percpu_counter_tree_approximate_compare(&a->tree, &b->tree);

	CHECK(res == 0 || res == expect);
	CHECK(percpu_counter_tree_precise_compare(&a->tree, &b->tree) == expect);
	res = percpu_counter_tree_approximate_compare(&b->tree, &a->tree);
	CHECK(res == 0 || res == -expect);
	CHECK(percpu_counter_tree_precise_compare(&b->tree, &a->tree) == -expect);
}

/*
 * On each CPU, add @inc (1 or -1) until the next addition would propagate a
 * carry to the approximate sum. This leaves every tree item on the path from
 * this CPU to the root holding its largest residual in the direction of @inc.
 * Each addition is first tried on @probe, which holds the same state as @c:
 * reverting an addition which reached the approximate sum would not restore
 * the residuals subtracted on the way. The path from a CPU to the root
 * holds less than the accuracy bound: more additions mean that a carry was
 * lost.
 */
static void saturate(struct test_counter *c, struct test_counter *probe, long inc)
{
	unsigned long max_steps;
	unsigned int cpu;

	max_steps = percpu_counter_tree_approximate_accuracy(&c->tree) +
		    (1UL << c->tree.batch_order);
	for (cpu = 0; cpu < nr_cpu_ids; cpu++) {
		unsigned long steps;

		for (steps = 0; steps < max_steps; steps++) {
			long approx = percpu_counter_tree_approximate_sum(&probe->tree);

			test_counter_add(probe, cpu, inc);
			if (percpu_counter_tree_approximate_sum(&probe->tree) != approx) {
				test_counter_copy(probe, c);
				break;
			}
			test_counter_add(c, cpu, inc);
		}
		if (steps == max_steps) {
			CHECK(0);
			return;
		}
	}
}

static void test_saturated(unsigned long batch_size, long inc)
{
	struct test_counter c, probe, exact;
	unsigned long accuracy;
	long approx, delta;

	if (test_counter_init(&c, batch_size)) {
		CHECK(0);
		return;
	}
	if (test_counter_init(&probe, batch_size)) {
		CHECK(0);
		goto destroy_c;
	}
	if (test_counter_init(&exact, batch_size)) {
		CHECK(0);
		goto destroy_probe;
	}
	saturate(&c, &probe, inc);
	approx = percpu_counter_tree_approximate_sum(&c.tree);
	accuracy = percpu_counter_tree_approximate_accuracy(&c.tree);
	/* Symmetric residuals: the accuracy is reached in both directions. */
	CHECK((c.shadow - approx) * inc == (long)tree_residual_capacity(batch_size));
	CHECK((c.shadow - approx) * inc == (long)accuracy);
	check_counter(&c);

	/* Compare with exactly approximated counters around the saturated precise sum. */
	for (delta = -1; delta <= 1; delta++) {
		test_counter_set(&exact, c.shadow + delta);
		CHECK(percpu_counter_tree_approximate_sum(&exact.tree) == exact.shadow);
		check_counter_pair(&c, &exact);
	}
	test_counter_destroy(&exact);
destroy_probe:
	test_counter_destroy(&probe);
destroy_c:
	test_counter_destroy(&c);
}

static long random_increment(unsigned long batch_size)
{
	long v;

	switch (rnd() % 4) {
	case 0:
		v = rnd() % 4;
		break;
	case 1:
		v = rnd() % (4 * batch_size);
		break;
	case 2:
		v = rnd() % (64 * batch_size);
		break;
	default:
		v = rnd() % 1000000;
		break;
	}
	return rnd() & 1 ? v : -v;
}

static void test_random_walk(unsigned long batch_size, unsigned long nr_steps)
{
	struct test_counter c[2];
	unsigned long step;

	if (test_counter_init(&c[0], batch_size)) {
		CHECK(0);
		return;
	}
	if (test_counter_init(&c[1], batch_size)) {
		CHECK(0);
		test_counter_destroy(&c[0]);
		return;
	}
	for (step = 0; step < nr_steps; step++) {
		struct test_counter *t = &c[rnd() & 1];

		test_counter_add(t, rnd() % nr_cpu_ids, random_increment(batch_size));
		/* Bring the counters close to each other to exercise the precise comparison. */
		if (!(rnd() % 8)) {
			long delta = c[0].shadow - c[1].shadow;
			unsigned long accuracy;

			accuracy = percpu_counter_tree_approximate_accuracy(&c[0].tree);
			delta += (long)(rnd() % (2 * accuracy + 1)) - (long)accuracy;
			test_counter_add(&c[1], rnd() % nr_cpu_ids, delta);
		}
		check_counter(t);
		check_counter_pair(&c[0], &c[1]);
	}
	test_counter_destroy(&c[0]);
	test_counter_destroy(&c[1]);
}

static void test_lazy(unsigned long batch_size, unsigned long nr_steps)
{
	long nr_allocs = hpcc_nr_allocs;
	struct test_counter c;
	unsigned long step;

	if (percpu_counter_tree_init_many_lazy(&c.tree, 1, batch_size)) {
		CHECK(0);
		return;
	}
	c.shadow = 0;
	CHECK(!c.tree.level0);
	CHECK(!c.tree.approx_sum);
	CHECK(!percpu_counter_tree_active(&c.tree));
	for (step = 0; step < nr_steps; step++) {
		long inc = random_increment(batch_size);

		hpcc_current_cpu = rnd() % nr_cpu_ids;
		if (rnd() & 1)
			percpu_counter_tree_add_owner(&c.tree, inc);
		else
			percpu_counter_tree_add(&c.tree, inc);
		c.shadow += inc;
		if (!c.tree.level0)
			CHECK(percpu_counter_tree_approximate_sum(&c.tree) == c.shadow);
		check_counter(&c);
		if (step == nr_steps / 2) {
			CHECK(!percpu_counter_tree_activate_many(&c.tree, 1, GFP_KERNEL));
			CHECK(c.tree.level0);
			CHECK(c.tree.approx_sum);
			CHECK(percpu_counter_tree_active(&c.tree));
			/* One tree: its per-CPU counters and its tree items. */
			CHECK(hpcc_nr_allocs == nr_allocs + 2);
			CHECK(!percpu_counter_tree_activate_many(&c.tree, 1, GFP_KERNEL));
			CHECK(hpcc_nr_allocs == nr_allocs + 2);
		}
	}
	test_counter_destroy(&c);
	CHECK(hpcc_nr_allocs == nr_allocs);
}

/*
 * Both counters of a limit counter only grow, so they wrap after enough
 * updates. The limit tests start both roots at limit_base, which leaves
 * the value at 0. When limit_wrap is set, the base is a random distance
 * below the wrap point limit_wrap_at, within the range the tests cover.
 * A check which compares the roots without taking their difference then
 * fails while one side of the comparison has wrapped and the other has
 * not.
 */
static bool limit_wrap;
static unsigned long limit_wrap_at, limit_base;

static int limit_init(struct percpu_counter_tree_limit *l, unsigned long batch_size)
{
	unsigned long accuracy;

	if (percpu_counter_tree_limit_init(l, batch_size, GFP_KERNEL))
		return -ENOMEM;
	limit_base = 0;
	if (limit_wrap) {
		accuracy = percpu_counter_tree_approximate_accuracy(&l->counters[0]);
		limit_base = limit_wrap_at - 1 - rnd() % (8 * accuracy);
	}
	atomic_long_set(l->counters[0].approx_sum, limit_base);
	atomic_long_set(l->counters[1].approx_sum, limit_base);
	return 0;
}

/* Each counter of a limit counter errs in one direction only. */
static void check_limit(struct percpu_counter_tree_limit *l, unsigned long add_total,
			unsigned long sub_total)
{
	struct percpu_counter_tree *add = &l->counters[0], *sub = &l->counters[1];
	unsigned long accuracy = percpu_counter_tree_approximate_accuracy(add);
	unsigned long root_add = atomic_long_read(add->approx_sum);
	unsigned long root_sub = atomic_long_read(sub->approx_sum);

	CHECK(percpu_counter_tree_precise_sum_upper(add) == add_total);
	CHECK((unsigned long)percpu_counter_tree_precise_sum(sub) == sub_total);
	/* Modulo the wrap: a root above its total gives a huge difference. */
	CHECK(add_total - root_add <= accuracy);
	CHECK(sub_total - root_sub <= accuracy);
	CHECK(percpu_counter_tree_limit_precise_sum(l) == add_total - sub_total);
}

/*
 * Without concurrency, an addition must fit exactly when the value stays
 * within the limit. The limit is a few accuracies wide, so the walk often
 * runs within the accuracy of the limit, where the precise sums decide.
 */
static void test_limit(unsigned long batch_size, unsigned long nr_steps)
{
	unsigned long value = 0, add_total, sub_total, accuracy, max, step, amount;
	struct percpu_counter_tree_limit l;
	bool added;

	if (limit_init(&l, batch_size)) {
		CHECK(0);
		return;
	}
	add_total = limit_base;
	sub_total = limit_base;
	accuracy = percpu_counter_tree_approximate_accuracy(&l.counters[0]);
	max = 3 * accuracy + rnd() % (accuracy + 1) + 1;
	for (step = 0; step < nr_steps; step++) {
		hpcc_current_cpu = rnd() % nr_cpu_ids;
		if (value && !(rnd() % 3)) {
			amount = 1 + rnd() % value;
			percpu_counter_tree_limit_sub(&l, amount);
			value -= amount;
			sub_total += amount;
		} else {
			amount = 1 + rnd() % ((rnd() & 1) ? batch_size : 8 * batch_size);
			added = percpu_counter_tree_limit_add(&l, max, amount);
			CHECK(added == (value + amount <= max));
			if (added) {
				value += amount;
				add_total += amount;
			} else if (amount <= batch_size) {
				/* Added, then undone by a subtraction. */
				add_total += amount;
				sub_total += amount;
			}	/* A larger amount which cannot fit is not added. */
		}
		check_limit(&l, add_total, sub_total);
	}
	/* Fill up: the value stops exactly at the limit. */
	while (value < max) {
		hpcc_current_cpu = rnd() % nr_cpu_ids;
		amount = (max - value + 1) / 2;
		CHECK(percpu_counter_tree_limit_add(&l, max, amount));
		value += amount;
		add_total += amount;
	}
	CHECK(!percpu_counter_tree_limit_add(&l, max, 1));
	add_total++;
	sub_total++;
	CHECK(value == max);
	check_limit(&l, add_total, sub_total);
	percpu_counter_tree_limit_destroy(&l);
}

/* State of test_limit_stall(), shared with its stall hook. */
static struct {
	struct percpu_counter_tree_limit l;
	unsigned long max, value;
	unsigned int stalled_cpu, share_mask;
} stall;

/*
 * While one addition is stalled, the CPUs sharing its stalled item add.
 * Their carries meet its pending items, and go straight to the root.
 */
static void stall_others_add(void)
{
	struct percpu_counter_tree *add = &stall.l.counters[0];
	unsigned long batch_size = percpu_counter_tree_batch_size(add);
	unsigned long root = atomic_long_read(add->approx_sum);
	unsigned int saved = hpcc_current_cpu, cpu, i;
	bool others = false;

	for (cpu = 0; cpu < nr_cpu_ids; cpu++) {
		if (cpu == stall.stalled_cpu ||
		    (cpu & stall.share_mask) != (stall.stalled_cpu & stall.share_mask))
			continue;
		others = true;
		hpcc_current_cpu = cpu;
		for (i = 0; i < batch_size; i++) {
			if (percpu_counter_tree_limit_add(&stall.l, stall.max, 1))
				stall.value++;
		}
		CHECK(stall.value <= stall.max);
		CHECK(percpu_counter_tree_limit_precise_sum(&stall.l) >= stall.value);
	}
	if (others)
		CHECK((long)(atomic_long_read(add->approx_sum) - root) > 0);
	hpcc_current_cpu = saved;
}

/*
 * Fill each residual of the additions to its largest value. The value is
 * then the accuracy, and the root is 0. The limit is set to the value.
 * Then an addition on CPU 0 carries up to the root through all the
 * levels. Stall it right after it arrives at its item of @stall_level,
 * or at the root when @stall_level is nr_levels. Its items below the
 * root then hold their carries on the way up. The CPUs sharing the
 * stalled item add while it is stalled, and must fail.
 */
static void test_limit_stall(unsigned long batch_size, unsigned int stall_level)
{
	unsigned int nr_levels = tree_topology.config->nr_levels, level, level_items, i, cpu;
	unsigned long step, bit_mask;
	struct percpu_counter_tree *add;
	atomic_long_t *item;

	if (limit_init(&stall.l, batch_size)) {
		CHECK(0);
		return;
	}
	add = &stall.l.counters[0];
	stall.max = percpu_counter_tree_approximate_accuracy(add);
	stall.value = 0;
	/*
	 * An amount of bit_mask - step is a multiple of the batch sizes of the
	 * levels below. It goes through them and stays in item i of this
	 * level. CPU i updates that item.
	 */
	level_items = 1U << tree_topology.nr_cpus_order;
	bit_mask = batch_size;
	item = percpu_counter_tree_items(add);
	stall.share_mask = 0;		/* At the root, all CPUs share it. */
	for (level = 1; level < nr_levels; level++) {
		step = bit_mask;
		level_items >>= tree_topology.config->n_arity_order[level - 1];
		bit_mask <<= tree_topology.config->n_arity_order[level - 1];
		if (level == stall_level) {
			hpcc_stall_item = item;		/* CPU 0's item. */
			stall.share_mask = level_items - 1;
		}
		for (i = 0; i < level_items; i++) {
			hpcc_current_cpu = i;
			CHECK(percpu_counter_tree_limit_add(&stall.l, stall.max, bit_mask - step));
			stall.value += bit_mask - step;
		}
		item += level_items * add->items_stride;
	}
	if (stall_level >= nr_levels)
		hpcc_stall_item = add->approx_sum;
	for (cpu = 0; cpu < nr_cpu_ids; cpu++) {
		hpcc_current_cpu = cpu;
		CHECK(percpu_counter_tree_limit_add(&stall.l, stall.max, batch_size - 1));
		stall.value += batch_size - 1;
	}
	CHECK(stall.value == stall.max);
	CHECK((unsigned long)atomic_long_read(add->approx_sum) == limit_base);

	stall.stalled_cpu = 0;
	hpcc_stall_fn = stall_others_add;
	hpcc_current_cpu = stall.stalled_cpu;
	if (percpu_counter_tree_limit_add(&stall.l, stall.max, 1))
		stall.value++;
	CHECK(!hpcc_stall_fn);	/* The stall happened. */
	hpcc_stall_fn = NULL;
	CHECK(stall.value <= stall.max);
	CHECK(percpu_counter_tree_limit_precise_sum(&stall.l) == stall.value);
	percpu_counter_tree_limit_destroy(&stall.l);
}

/* State of test_limit_retry(), shared with its stall hook. */
static struct {
	struct percpu_counter_tree_limit l;
	unsigned long in_flight;
	unsigned int in_flight_cpu;
} retry;

/* The concurrent addition in flight fails, and is undone. */
static void retry_undo_in_flight(void)
{
	unsigned int saved = hpcc_current_cpu;

	hpcc_current_cpu = retry.in_flight_cpu;
	percpu_counter_tree_limit_sub(&retry.l, retry.in_flight);
	hpcc_current_cpu = saved;
}

/*
 * With one unit free, CPU 0 adds 1 while another addition of 1 is in
 * flight, about to fail. CPU 0 fails, and that addition is undone while
 * CPU 0 undoes its own. CPU 0 must then retry, and fit.
 */
static void test_limit_retry(unsigned long batch_size)
{
	struct percpu_counter_tree *add = &retry.l.counters[0], *sub = &retry.l.counters[1];
	unsigned long max, value = 0, amount;

	if (limit_init(&retry.l, batch_size)) {
		CHECK(0);
		return;
	}
	max = 2 * percpu_counter_tree_approximate_accuracy(add) + 1;
	hpcc_current_cpu = 1 % nr_cpu_ids;
	while (value < max - 1) {
		amount = max - 1 - value < batch_size ? max - 1 - value : batch_size;
		CHECK(percpu_counter_tree_limit_add(&retry.l, max, amount));
		value += amount;
	}
	/* CPU 0's subtractions at batch_size - 1: its undo carries. */
	hpcc_current_cpu = 0;
	percpu_counter_tree_limit_sub(&retry.l, batch_size - 1);
	hpcc_current_cpu = 1 % nr_cpu_ids;
	CHECK(percpu_counter_tree_limit_add(&retry.l, max, batch_size - 1));
	/* The addition in flight, from another CPU. */
	retry.in_flight = 1;
	retry.in_flight_cpu = 2 % nr_cpu_ids;
	hpcc_current_cpu = retry.in_flight_cpu;
	percpu_counter_tree_add_arrive_first(add, retry.in_flight);
	/* CPU 0's undo carries to the first item of the subtractions. */
	hpcc_stall_item = tree_topology.config->nr_levels > 1 ?
			  percpu_counter_tree_items(sub) : sub->approx_sum;
	hpcc_stall_fn = retry_undo_in_flight;
	hpcc_current_cpu = 0;
	CHECK(percpu_counter_tree_limit_add(&retry.l, max, 1));
	CHECK(!hpcc_stall_fn);	/* The other addition was undone during the check. */
	hpcc_stall_fn = NULL;
	CHECK(percpu_counter_tree_limit_precise_sum(&retry.l) == max);
	percpu_counter_tree_limit_destroy(&retry.l);
}

/*
 * An amount larger than the batch size which cannot fit fails before it
 * is added. The additions do not change, and a fitting amount still fits.
 */
static void test_limit_precheck(unsigned long batch_size)
{
	struct percpu_counter_tree_limit l;
	unsigned long max, value = 0, amount, before;

	if (limit_init(&l, batch_size)) {
		CHECK(0);
		return;
	}
	max = 2 * percpu_counter_tree_approximate_accuracy(&l.counters[0]) + 1;
	hpcc_current_cpu = 1 % nr_cpu_ids;
	while (value < max - batch_size) {
		amount = max - batch_size - value;
		if (amount > batch_size)
			amount = batch_size;
		CHECK(percpu_counter_tree_limit_add(&l, max, amount));
		value += amount;
	}
	before = percpu_counter_tree_precise_sum_upper(&l.counters[0]);
	hpcc_current_cpu = 0;
	CHECK(!percpu_counter_tree_limit_add(&l, max, 2 * batch_size));
	CHECK(percpu_counter_tree_precise_sum_upper(&l.counters[0]) == before);
	CHECK(percpu_counter_tree_limit_add(&l, max, batch_size));
	CHECK(percpu_counter_tree_limit_precise_sum(&l) == max);
	percpu_counter_tree_limit_destroy(&l);
}

/* The batch size chosen for an accuracy is the largest within it. */
static void test_batch_for_accuracy(void)
{
	unsigned long multiplier = tree_topology.accuracy_multiplier, accuracy, batch_size;

	for (accuracy = 1; accuracy < 512 * multiplier; accuracy = accuracy * 3 + 1) {
		batch_size = percpu_counter_tree_batch_for_accuracy(accuracy);
		CHECK(batch_size >= 2 && batch_size <= PERCPU_COUNTER_TREE_MAX_BATCH);
		if (batch_size > 2)
			CHECK(batch_size * multiplier - nr_cpu_ids <= accuracy);
		if (batch_size < PERCPU_COUNTER_TREE_MAX_BATCH)
			CHECK(2 * batch_size * multiplier - nr_cpu_ids > accuracy);
	}
}

static const unsigned int test_nr_cpus[] = {
	1, 2, 3, 4, 5, 8, 9, 16, 17, 32, 33, 64, 65, 100, 128, 256, 512,
	1000, 1024, 2048, 4096, 8192, 16384, 32768, 65536,
};

/* Exhaustive mode: also cover the largest supported configurations. */
static const unsigned int test_nr_cpus_large[] = {
	1U << 17, 1U << 18, 1U << 19, 1U << 20,
};

static const unsigned long test_batch_sizes[] = { 2, 4, 8, 16, 32, 64, 128 };

static int test_num;

static int run_case(unsigned int nr_cpus, unsigned long batch_size)
{
	/* No wrap, then below the unsigned and the signed wrap points. */
	const struct {
		bool wrap;
		unsigned long at;
	} wraps[] = { { false, 0 }, { true, 0 }, { true, 1UL << (BITS_PER_LONG - 1) } };
	unsigned int level, i;
	/* Each check iterates over all CPUs: bound the random walk cost. */
	unsigned long nr_steps = 10000000 / nr_cpus;

	if (nr_steps < 100)
		nr_steps = 100;
	if (nr_steps > 50000)
		nr_steps = 50000;

	nr_case_failures = 0;
	hpcc_nr_warnings = 0;
	hpcc_init_topology(nr_cpus);
	test_saturated(batch_size, 1);
	test_saturated(batch_size, -1);
	test_random_walk(batch_size, nr_steps);
	test_lazy(batch_size, nr_steps / 2);
	for (i = 0; i < ARRAY_SIZE(wraps); i++) {
		limit_wrap = wraps[i].wrap;
		limit_wrap_at = wraps[i].at;
		test_limit(batch_size, nr_steps / 2);
		/* These fill the tree near the limit, where each addition sums all CPUs. */
		if (nr_cpus > 4096)
			continue;
		for (level = 1; level <= tree_topology.config->nr_levels; level++)
			test_limit_stall(batch_size, level);
		test_limit_retry(batch_size);
		test_limit_precheck(batch_size);
	}
	limit_wrap = false;
	test_batch_for_accuracy();
	CHECK(!hpcc_nr_warnings);
	printf("%sok %d - nr_cpus=%u batch_size=%lu\n", nr_case_failures ? "not " : "",
	       ++test_num, nr_cpus, batch_size);
	if (nr_case_failures > MAX_REPORTED_FAILURES)
		printf("#   ... %lu failed checks in total\n", nr_case_failures);
	return nr_case_failures ? 1 : 0;
}

/* Tree shapes given with the arity boot parameter. */
static const struct {
	unsigned int nr_cpus;
	const char *arity;
	const char *shape;	/* Shape in use, NULL if the arity is invalid. */
} test_arity[] = {
	{ 16, "16", "16" },
	{ 16, "4,4", "4,4" },
	{ 16, "2,2,2,2", "2,2,2,2" },
	{ 16, "4,2", NULL },		/* Covers 8 CPUs. */
	{ 16, "4,8", NULL },		/* Covers 32 CPUs. */
	{ 16, "4,4,1", NULL },		/* Fan-out of 1. */
	{ 16, "4,3", NULL },		/* Not a power of 2. */
	{ 16, "4,4x", NULL },		/* Trailing characters. */
	{ 16, "-4,-4", NULL },
	{ 17, "8,4", "8,4" },
	{ 100, "128", "128" },
	{ 192, "8,4,8", "8,4,8" },
	{ 192, "2,2,2,2,2,2,2,2", NULL },	/* More than MAX_NR_LEVELS. */
	{ 1024, "32,32", "32,32" },
	{ 1, "2", NULL },
};

static int run_arity_case(unsigned int i)
{
	const char *expect = test_arity[i].shape;
	char buf[SHAPE_BUF_SIZE];
	int len;

	nr_case_failures = 0;
	hpcc_nr_warnings = 0;
	tree_topology.config = NULL;
	CHECK(!arity_param_set(test_arity[i].arity, NULL));
	hpcc_init_topology(test_arity[i].nr_cpus);
	arity_param = NULL;
	CHECK(arity_param_set("16", NULL) == -EBUSY);
	CHECK((tree_topology.config == &tree_topology.boot_config) == !!expect);
	len = arity_param_get(buf, NULL);
	CHECK(len > 0 && buf[len - 1] == '\n');
	buf[len - 1] = '\0';
	if (expect) {
		CHECK(!strcmp(buf, expect));
		test_saturated(2, 1);
		test_saturated(32, -1);
		test_random_walk(32, 20000);
	}
	CHECK(!hpcc_nr_warnings);
	printf("%sok %d - arity nr_cpus=%u \"%s\" -> \"%s\"\n", nr_case_failures ? "not " : "",
	       ++test_num, test_arity[i].nr_cpus, test_arity[i].arity, buf);
	return nr_case_failures ? 1 : 0;
}

static int run_check_config(void)
{
	unsigned int nr_errors = percpu_counter_tree_check_config();

	printf("%sok %d - configuration table\n", nr_errors ? "not " : "", ++test_num);
	return nr_errors ? 1 : 0;
}

static void usage(const char *name)
{
	fprintf(stderr, "Usage: %s [-a]\n", name);
	fprintf(stderr, "  -a  Also test the largest supported numbers of CPUs (slow).\n");
}

int main(int argc, char **argv)
{
	unsigned int i, j, nr_tests;
	bool large = false;
	int failed = 0;

	if (argc > 2 || (argc == 2 && strcmp(argv[1], "-a"))) {
		usage(argv[0]);
		return 2;
	}
	if (argc == 2)
		large = true;

	nr_tests = 1 + ARRAY_SIZE(test_arity) +
		   ARRAY_SIZE(test_nr_cpus) * ARRAY_SIZE(test_batch_sizes);
	if (large)
		nr_tests += ARRAY_SIZE(test_nr_cpus_large) * ARRAY_SIZE(test_batch_sizes);
	printf("1..%u\n", nr_tests);

	failed += run_check_config();
	for (i = 0; i < ARRAY_SIZE(test_arity); i++)
		failed += run_arity_case(i);
	for (i = 0; i < ARRAY_SIZE(test_nr_cpus); i++)
		for (j = 0; j < ARRAY_SIZE(test_batch_sizes); j++)
			failed += run_case(test_nr_cpus[i], test_batch_sizes[j]);
	if (large) {
		for (i = 0; i < ARRAY_SIZE(test_nr_cpus_large); i++)
			for (j = 0; j < ARRAY_SIZE(test_batch_sizes); j++)
				failed += run_case(test_nr_cpus_large[i], test_batch_sizes[j]);
	}
	printf("# %d/%u test cases failed\n", failed, nr_tests);
	return failed ? 1 : 0;
}
