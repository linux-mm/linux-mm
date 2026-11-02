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
 * The test is single-threaded, so it only covers settled counters.
 */

#include <stdarg.h>
#include "hpcc_shim.h"

unsigned int nr_cpu_ids;
unsigned int hpcc_current_cpu;
unsigned long hpcc_nr_warnings;

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

	if (test_counter_init(&c, batch_size) || test_counter_init(&probe, batch_size) ||
	    test_counter_init(&exact, batch_size)) {
		CHECK(0);
		return;
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
	test_counter_destroy(&c);
	test_counter_destroy(&probe);
	test_counter_destroy(&exact);
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

	if (test_counter_init(&c[0], batch_size) || test_counter_init(&c[1], batch_size)) {
		CHECK(0);
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
	CHECK(!hpcc_nr_warnings);
	printf("%sok %d - nr_cpus=%u batch_size=%lu\n", nr_case_failures ? "not " : "",
	       ++test_num, nr_cpus, batch_size);
	if (nr_case_failures > MAX_REPORTED_FAILURES)
		printf("#   ... %lu failed checks in total\n", nr_case_failures);
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

	nr_tests = 1 + ARRAY_SIZE(test_nr_cpus) * ARRAY_SIZE(test_batch_sizes);
	if (large)
		nr_tests += ARRAY_SIZE(test_nr_cpus_large) * ARRAY_SIZE(test_batch_sizes);
	printf("1..%u\n", nr_tests);

	failed += run_check_config();
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
