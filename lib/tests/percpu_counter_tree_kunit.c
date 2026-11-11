// SPDX-License-Identifier: GPL-2.0+ OR MIT
// SPDX-FileCopyrightText: 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>

#include <kunit/test.h>
#include <linux/percpu_counter_tree.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/random.h>
#include <linux/cpuhplock.h>
#include <linux/slab.h>
#include <linux/sched/task.h>
#include <linux/workqueue.h>

struct multi_thread_test_data {
	struct work_struct work;
	long increment;
	unsigned int nr_inc;
	int counter_index;
};

#define NR_COUNTERS	2

/* Hierarchical per-CPU counter instances. */
static struct percpu_counter_tree counter[NR_COUNTERS];

/* Global atomic counters for validation. */
static atomic_long_t global_counter[NR_COUNTERS];

static DECLARE_WAIT_QUEUE_HEAD(kernel_threads_wq);
static atomic_t kernel_threads_to_run;

static void complete_work(void)
{
	if (atomic_dec_and_test(&kernel_threads_to_run))
		wake_up(&kernel_threads_wq);
}

/* Random tasks test. */
#define NR_RAND_TASKS	1000

static void hpcc_print_info(struct kunit *test)
{
	kunit_info(test, "Running test with %d CPUs\n", num_online_cpus());
}

static void add_to_counter(int counter_index, unsigned int nr_inc, long increment)
{
	unsigned int i;

	for (i = 0; i < nr_inc; i++) {
		percpu_counter_tree_add(&counter[counter_index], increment);
		atomic_long_add(increment, &global_counter[counter_index]);
	}
}

/* Check the residuals and the carries of a settled counter. */
static void check_residuals(struct kunit *test, struct percpu_counter_tree *c)
{
#ifdef CONFIG_SMP
	KUNIT_EXPECT_EQ(test, 0, percpu_counter_tree_check_residuals(c));
#endif
}

static void check_counters(struct kunit *test)
{
	int counter_index;

	/* Compare each counter with its global counter. */
	for (counter_index = 0; counter_index < NR_COUNTERS; counter_index++) {
		struct percpu_counter_tree *c = &counter[counter_index];
		long v = atomic_long_read(&global_counter[counter_index]);
		long approx_sum = percpu_counter_tree_approximate_sum(c);
		unsigned long accuracy;
		long precise_min, precise_max;

		/* Precise comparison. */
		KUNIT_EXPECT_EQ(test, percpu_counter_tree_precise_sum(c), v);
		check_residuals(test, c);
		KUNIT_EXPECT_EQ(test, 0, percpu_counter_tree_precise_compare_value(c, v));

		/* Approximate comparison. */
		KUNIT_EXPECT_EQ(test, 0, percpu_counter_tree_approximate_compare_value(c, v));

		/* Accuracy limits checks. */
		accuracy = percpu_counter_tree_approximate_accuracy(c);

		/*
		 * The accuracy is an unsigned long, so the inner expression
		 * promotes to unsigned. Compute the difference in unsigned and
		 * cast to signed for the comparison against 0.
		 */
		KUNIT_EXPECT_GE(test, (long)(approx_sum - (v - accuracy)), 0);
		KUNIT_EXPECT_LE(test, (long)(approx_sum - (v + accuracy)), 0);

		/* Precise min/max range check. */
		percpu_counter_tree_approximate_min_max_range(approx_sum, accuracy,
							      &precise_min, &precise_max);

		KUNIT_EXPECT_GE(test, v - precise_min, 0);
		KUNIT_EXPECT_LE(test, v - precise_max, 0);
	}
	/* Compare each counter with the second counter. */
	KUNIT_EXPECT_EQ(test, percpu_counter_tree_precise_sum(&counter[0]),
			percpu_counter_tree_precise_sum(&counter[1]));
	KUNIT_EXPECT_EQ(test, 0, percpu_counter_tree_precise_compare(&counter[0], &counter[1]));
	KUNIT_EXPECT_EQ(test, 0, percpu_counter_tree_approximate_compare(&counter[0], &counter[1]));
}

/*
 * Workqueue running the updates of the multi-threaded tests. Its workers
 * are reused across rounds, which keeps the tests fast on machines where
 * creating threads on remote CPUs is slow. Work items are CPU intensive,
 * so several of them queued on one CPU run concurrently in separate
 * workers, and can preempt each other in the middle of an update.
 */
static struct workqueue_struct *hpcc_test_wq;

static void multi_thread_work_fn(struct work_struct *work)
{
	struct multi_thread_test_data *td =
		container_of(work, struct multi_thread_test_data, work);

	add_to_counter(td->counter_index, td->nr_inc, td->increment);
	kfree(td);
	complete_work();
}

/* Queue an update of a counter on @target_cpu. Call with the CPU hotplug lock held. */
static int test_queue_on_cpu(int target_cpu, int counter_index, unsigned int nr_inc,
			     long increment)
{
	struct multi_thread_test_data *td = kzalloc(sizeof(*td), GFP_KERNEL);

	if (!td)
		return -ENOMEM;
	INIT_WORK(&td->work, multi_thread_work_fn);
	td->increment = increment;
	td->nr_inc = nr_inc;
	td->counter_index = counter_index;
	atomic_inc(&kernel_threads_to_run);
	queue_work_on(target_cpu, hpcc_test_wq, &td->work);
	return 0;
}

static void init_kthreads(void)
{
	atomic_set(&kernel_threads_to_run, 1);
}

/*
 * The caller needs to wait for work completion _before_ stopping kthreads.
 */
static void fini_kthreads(void)
{
	/* Release our own reference. */
	complete_work();
	/* Wait for all others threads to run. */
	wait_event(kernel_threads_wq, (atomic_read(&kernel_threads_to_run) == 0));
}

static int init_counters(unsigned long batch_size)
{
	int i, ret;

	ret = percpu_counter_tree_init_many(counter, NR_COUNTERS, batch_size, GFP_KERNEL);
	if (ret)
		return ret;
	for (i = 0; i < NR_COUNTERS; i++)
		atomic_long_set(&global_counter[i], 0);
	return 0;
}

static void fini_counters(void)
{
	percpu_counter_tree_destroy_many(counter, NR_COUNTERS);
}

enum up_test_inc_type {
	INC_ONE,
	INC_MINUS_ONE,
	INC_RANDOM,
};

/*
 * Single-threaded tests. Those run the updates on various CPUs, one at a
 * time: work_on_cpu() waits for each update to complete before the next,
 * effectively making sure there are no concurrent updates. Each test
 * stops early after SINGLE_THREAD_MAX_SECONDS, because cross-CPU wakeups
 * can be slow on large virtual machines.
 */
#define SINGLE_THREAD_MAX_SECONDS	10

struct single_thread_update {
	long increment;
	int counter_index;
};

static long single_thread_update_fn(void *arg)
{
	struct single_thread_update *u = arg;

	add_to_counter(u->counter_index, 1, u->increment);
	return 0;
}

/* CPU selection for the updates of each counter. */
enum st_cpu_select {
	ST_FIRST_CPU,		/* First online CPU. */
	ST_RANDOM_CPU,		/* Any online CPU, for each update. */
};

/* Call with the CPU hotplug lock held. */
static int st_select_cpu(enum st_cpu_select select)
{
	if (select == ST_FIRST_CPU)
		return cpumask_first(cpu_online_mask);
	return cpumask_any_distribute(cpu_online_mask);
}

static void do_hpcc_test_single_thread(struct kunit *test, enum st_cpu_select select0,
				       enum st_cpu_select select1, enum up_test_inc_type type)
{
	unsigned long batch_size_order = 5;
	unsigned long deadline = jiffies + SINGLE_THREAD_MAX_SECONDS * HZ;
	int ret, i;

	ret = init_counters(1UL << batch_size_order);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		return;
	cpus_read_lock();
	for (i = 0; i < 10000 && time_before(jiffies, deadline); i++) {
		struct single_thread_update u0 = { .counter_index = 0 };
		struct single_thread_update u1 = { .counter_index = 1 };
		long increment;

		switch (type) {
		case INC_ONE:
			increment = 1;
			break;
		case INC_MINUS_ONE:
			increment = -1;
			break;
		case INC_RANDOM:
			increment = (long)get_random_long() % 50000;
			break;
		default:
			WARN_ON_ONCE(1);
			increment = 0;
		}
		u0.increment = increment;
		u1.increment = increment;
		work_on_cpu(st_select_cpu(select0), single_thread_update_fn, &u0);
		work_on_cpu(st_select_cpu(select1), single_thread_update_fn, &u1);
		check_counters(test);
	}
	cpus_read_unlock();
	fini_counters();
}

static void hpcc_test_single_thread_first(struct kunit *test)
{
	do_hpcc_test_single_thread(test, ST_FIRST_CPU, ST_FIRST_CPU, INC_ONE);
	do_hpcc_test_single_thread(test, ST_FIRST_CPU, ST_FIRST_CPU, INC_MINUS_ONE);
	do_hpcc_test_single_thread(test, ST_FIRST_CPU, ST_FIRST_CPU, INC_RANDOM);
}

static void hpcc_test_single_thread_first_random(struct kunit *test)
{
	do_hpcc_test_single_thread(test, ST_FIRST_CPU, ST_RANDOM_CPU, INC_ONE);
	do_hpcc_test_single_thread(test, ST_FIRST_CPU, ST_RANDOM_CPU, INC_MINUS_ONE);
	do_hpcc_test_single_thread(test, ST_FIRST_CPU, ST_RANDOM_CPU, INC_RANDOM);
}

static void hpcc_test_single_thread_random(struct kunit *test)
{
	do_hpcc_test_single_thread(test, ST_RANDOM_CPU, ST_RANDOM_CPU, INC_ONE);
	do_hpcc_test_single_thread(test, ST_RANDOM_CPU, ST_RANDOM_CPU, INC_MINUS_ONE);
	do_hpcc_test_single_thread(test, ST_RANDOM_CPU, ST_RANDOM_CPU, INC_RANDOM);
}

/* Multi-threaded SMP tests. */

/*
 * Bound the number of CPUs running workers in the multi-threaded tests,
 * so their run time does not grow with the size of the machine. Beyond
 * this bound, the workers run on a subset spread evenly across the
 * online CPUs, so they still reach every branch of the tree.
 */
#define MAX_WORKER_CPUS	64

/*
 * Each multi-threaded test stops early after MULTI_THREAD_MAX_SECONDS,
 * because cross-CPU wakeups can be slow on large virtual machines.
 */
#define MULTI_THREAD_MAX_SECONDS	60

static struct cpumask worker_cpus;

/* Select the CPUs running workers. Call with the CPU hotplug lock held. */
static const struct cpumask *select_worker_cpus(void)
{
	unsigned int nr_online = num_online_cpus(), i = 0;
	int cpu;

	cpumask_clear(&worker_cpus);
	for_each_online_cpu(cpu) {
		/* Pick MAX_WORKER_CPUS of the nr_online CPUs, evenly spaced. */
		if ((i + 1) * MAX_WORKER_CPUS / nr_online !=
		    i * MAX_WORKER_CPUS / nr_online)
			cpumask_set_cpu(cpu, &worker_cpus);
		i++;
	}
	return &worker_cpus;
}

/*
 * CPU selection for the second counter's thread, relative to the CPU
 * running the first counter's thread.
 */
enum mt_cpu_select {
	MT_SAME_CPU,		/* Same CPU as the first counter. */
	MT_EVEN_CPU,		/* Funnel CPU pairs (0, 1), (2, 3), ... to 0, 2, ... */
	MT_FIRST_CPU,		/* First online CPU. */
	MT_RANDOM_CPU,		/* Any online CPU. */
};

/* Return the CPU for the second counter's thread, or -1 to skip @cpu. */
static int mt_select_cpu(enum mt_cpu_select select, int cpu)
{
	int even_cpu;

	switch (select) {
	case MT_SAME_CPU:
		return cpu;
	case MT_EVEN_CPU:
		even_cpu = cpu & ~1;
		return cpumask_test_cpu(even_cpu, cpu_online_mask) ? even_cpu : -1;
	case MT_FIRST_CPU:
		return cpumask_first(cpu_online_mask);
	case MT_RANDOM_CPU:
		return cpumask_any_distribute(cpu_online_mask);
	}
	WARN_ON_ONCE(1);
	return -1;
}

static void do_hpcc_multi_thread_increment(struct kunit *test, enum mt_cpu_select select,
					   unsigned long batch_size, unsigned int nr_inc,
					   long increment)
{
	const struct cpumask *cpus;
	int ret, cpu;

	ret = init_counters(batch_size);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		return;
	init_kthreads();
	cpus_read_lock();
	cpus = select_worker_cpus();
	for_each_cpu(cpu, cpus) {
		int cpu1 = mt_select_cpu(select, cpu);

		if (cpu1 < 0)
			continue;
		ret = test_queue_on_cpu(cpu, 0, nr_inc, increment);
		KUNIT_EXPECT_EQ(test, ret, 0);
		if (ret)
			break;
		ret = test_queue_on_cpu(cpu1, 1, nr_inc, increment);
		KUNIT_EXPECT_EQ(test, ret, 0);
		if (ret)
			break;
	}
	fini_kthreads();
	cpus_read_unlock();
	check_counters(test);
	fini_counters();
}

static void hpcc_test_multi_thread_batch_increment(struct kunit *test)
{
	unsigned long deadline = jiffies + MULTI_THREAD_MAX_SECONDS * HZ;
	unsigned long batch_size_order;

	for (batch_size_order = 2; batch_size_order <= 7; batch_size_order++) {
		unsigned long batch_size = 1UL << batch_size_order;
		unsigned int nr_inc;

		for (nr_inc = 1; nr_inc < 1024; nr_inc *= 2) {
			long increment;

			for (increment = 1; increment < 100000; increment *= 10) {
				enum mt_cpu_select select;

				if (time_after(jiffies, deadline))
					return;
				for (select = MT_SAME_CPU; select <= MT_RANDOM_CPU; select++)
					do_hpcc_multi_thread_increment(test, select, batch_size,
								       nr_inc, increment);
			}
		}
	}
}

static void hpcc_test_multi_thread_random_walk(struct kunit *test)
{
	unsigned long deadline = jiffies + MULTI_THREAD_MAX_SECONDS * HZ;
	unsigned long batch_size_order = 5;
	int loop;

	for (loop = 0; loop < 100 && time_before(jiffies, deadline); loop++) {
		int ret, i;

		ret = init_counters(1UL << batch_size_order);
		KUNIT_EXPECT_EQ(test, ret, 0);
		if (ret)
			return;
		init_kthreads();
		cpus_read_lock();
		for (i = 0; i < NR_RAND_TASKS; i++) {
			long increment = (long)get_random_long() % 512;
			unsigned int nr_inc = ((unsigned long)get_random_long()) % 1024;

			ret = test_queue_on_cpu(cpumask_any_distribute(cpu_online_mask),
						0, nr_inc, increment);
			KUNIT_EXPECT_EQ(test, ret, 0);
			if (ret)
				break;
			ret = test_queue_on_cpu(cpumask_any_distribute(cpu_online_mask),
						1, nr_inc, increment);
			KUNIT_EXPECT_EQ(test, ret, 0);
			if (ret)
				break;
		}
		fini_kthreads();
		cpus_read_unlock();
		check_counters(test);
		fini_counters();
	}
}

#define CONTENTION_NR_ROUNDS	20000
#define CONTENTION_NR_INC	8

struct contention_shared {
	unsigned long batch_size;
	atomic_t round;			/* Round started by the coordinator. */
	atomic_t nr_done;		/* Rounds completed by the other workers. */
	unsigned int nr_workers;
	unsigned int nr_errors;		/* Rounds ending with invalid residuals. */
	bool started;
};

struct contention_data {
	struct contention_shared *sh;
	long sum;
	u32 seed;
	bool coordinator;
} ____cacheline_aligned_in_smp;

static DECLARE_WAIT_QUEUE_HEAD(contention_wq);

/*
 * Add random multiples of the batch size, of either sign, so each
 * addition carries into the counters above level 0. Increments of
 * opposite signs and of more than one batch size exercise concurrent
 * carry extractions retrying after a concurrent update of the residual.
 */
static void contention_add(struct contention_data *cd)
{
	unsigned int i;

	for (i = 0; i < CONTENTION_NR_INC; i++) {
		u32 x = cd->seed;
		long inc;

		/* xorshift32: get_random_u32() would dominate the loop. */
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		cd->seed = x;
		inc = (long)((x & 15) + 1) * cd->sh->batch_size;
		if (x & 16)
			inc = -inc;
		percpu_counter_tree_add(&counter[0], inc);
		cd->sum += inc;
	}
}

/*
 * The workers run rounds of a few additions each, all starting together.
 * Between rounds, the coordinator checks the residuals of the settled
 * counter: an invalid residual left by the last updaters of a round would
 * otherwise be repaired by the next additions.
 */
static int contention_worker_fn(void *data)
{
	struct contention_data *cd = data;
	struct contention_shared *sh = cd->sh;
	int round;

	/* Pairs with the release of started: nr_workers is final. */
	wait_event(contention_wq, smp_load_acquire(&sh->started));
	for (round = 1; round <= CONTENTION_NR_ROUNDS; round++) {
		if (cd->coordinator) {
			atomic_set_release(&sh->round, round);
			contention_add(cd);
			while (atomic_read_acquire(&sh->nr_done) < round * (sh->nr_workers - 1))
				cond_resched();
#ifdef CONFIG_SMP
			if (percpu_counter_tree_check_residuals(&counter[0]))
				sh->nr_errors++;
#endif
		} else {
			while (atomic_read_acquire(&sh->round) < round)
				cond_resched();
			contention_add(cd);
			/* Order the additions before the coordinator check. */
			atomic_inc_return_release(&sh->nr_done);
		}
	}
	complete_work();
	return 0;
}

static void do_hpcc_test_contended_carries(struct kunit *test, unsigned long batch_size)
{
	struct percpu_counter_tree *c = &counter[0];
	struct contention_shared sh = { .batch_size = batch_size };
	struct contention_data *cd;
	struct task_struct **task;
	const struct cpumask *cpus;
	unsigned long accuracy;
	long expected = 0, approx;
	bool first = true;
	int ret, cpu;

	cd = kvcalloc(nr_cpu_ids, sizeof(*cd), GFP_KERNEL);
	task = kvcalloc(nr_cpu_ids, sizeof(*task), GFP_KERNEL);
	KUNIT_EXPECT_NOT_NULL(test, cd);
	KUNIT_EXPECT_NOT_NULL(test, task);
	if (!cd || !task)
		goto free;
	ret = init_counters(batch_size);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		goto free;
	init_kthreads();
	cpus_read_lock();
	cpus = select_worker_cpus();
	for_each_cpu(cpu, cpus) {
		cd[cpu].sh = &sh;
		cd[cpu].coordinator = first;
		cd[cpu].seed = get_random_u32() | 1;
		task[cpu] = kthread_create_on_cpu(contention_worker_fn, &cd[cpu], cpu,
						  "kunit_hpcc_contention/%u");
		KUNIT_EXPECT_FALSE(test, IS_ERR(task[cpu]));
		if (IS_ERR(task[cpu]))
			break;
		atomic_inc(&kernel_threads_to_run);
		get_task_struct(task[cpu]);
		wake_up_process(task[cpu]);
		sh.nr_workers++;
		first = false;
	}
	/*
	 * Start the workers once their number is known. The coordinator may
	 * already be running and see started without the wait queue lock.
	 */
	smp_store_release(&sh.started, true);
	wake_up_all(&contention_wq);
	fini_kthreads();
	for_each_cpu(cpu, cpus) {
		if (IS_ERR_OR_NULL(task[cpu]))
			continue;
		kthread_stop_put(task[cpu]);
		expected += cd[cpu].sum;
	}
	cpus_read_unlock();

	kunit_info(test, "batch_size=%lu workers=%u rounds with invalid residuals=%u\n",
		   batch_size, sh.nr_workers, sh.nr_errors);
	KUNIT_EXPECT_EQ(test, sh.nr_errors, 0);
	KUNIT_EXPECT_EQ(test, percpu_counter_tree_precise_sum(c), expected);
	check_residuals(test, c);
	approx = percpu_counter_tree_approximate_sum(c);
	accuracy = percpu_counter_tree_approximate_accuracy(c);
	KUNIT_EXPECT_GE(test, approx, expected - (long)accuracy);
	KUNIT_EXPECT_LE(test, approx, expected + (long)accuracy);
	fini_counters();
free:
	kvfree(task);
	kvfree(cd);
}

/*
 * Concurrent carries from the worker CPUs into the counters above level 0
 * which they share, with a check that the residuals are back within range
 * and that no carry was lost or duplicated after each round.
 */
static void hpcc_test_contended_carries(struct kunit *test)
{
	unsigned long batch_size_order;

	if (num_online_cpus() < 2)
		kunit_skip(test, "needs at least 2 online CPUs");
	for (batch_size_order = 1; batch_size_order <= 5; batch_size_order += 2)
		do_hpcc_test_contended_carries(test, 1UL << batch_size_order);
}

static void hpcc_test_init_one(struct kunit *test)
{
	struct percpu_counter_tree pct, zeroed = {};
	int ret;

	/* A zeroed counter reads as 0 before its initialization. */
	KUNIT_EXPECT_EQ(test, percpu_counter_tree_approximate_sum(&zeroed), 0);
	ret = percpu_counter_tree_init(&pct, 32, GFP_KERNEL);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		return;

	percpu_counter_tree_destroy(&pct);
}

static void hpcc_test_set(struct kunit *test)
{
	static long values[] = {
		5, 100, 127, 128, 255, 256, 4095, 4096, 500000, 0,
		-5, -100, -127, -128, -255, -256, -4095, -4096, -500000,
	};
	struct percpu_counter_tree pct;
	int i, ret;

	ret = percpu_counter_tree_init(&pct, 32, GFP_KERNEL);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		return;

	for (i = 0; i < ARRAY_SIZE(values); i++) {
		long v = values[i];

		percpu_counter_tree_set(&pct, v);
		KUNIT_EXPECT_EQ(test, percpu_counter_tree_precise_sum(&pct), v);
		KUNIT_EXPECT_EQ(test, 0, percpu_counter_tree_approximate_compare_value(&pct, v));

		percpu_counter_tree_add(&pct, v);
		KUNIT_EXPECT_EQ(test, percpu_counter_tree_precise_sum(&pct), 2 * v);
		KUNIT_EXPECT_EQ(test, 0,
				percpu_counter_tree_approximate_compare_value(&pct, 2 * v));

		percpu_counter_tree_add(&pct, -2 * v);
		KUNIT_EXPECT_EQ(test, percpu_counter_tree_precise_sum(&pct), 0);
		KUNIT_EXPECT_EQ(test, 0, percpu_counter_tree_approximate_compare_value(&pct, 0));
	}

	percpu_counter_tree_destroy(&pct);
}

struct saturate_data {
	struct percpu_counter_tree *c, *probe;
	long inc;
	unsigned long max_steps;
	bool overrun;
};

/* Copy the state of counter @src into counter @dst, of the same topology. */
static void copy_counter_state(struct percpu_counter_tree *dst,
			       struct percpu_counter_tree *src)
{
#ifdef CONFIG_SMP
	percpu_counter_tree_copy_state(dst, src);
#else
	atomic_long_set(&dst->count, atomic_long_read(&src->count));
#endif
}

/*
 * Add inc (1 or -1) until the next addition would propagate a carry to the
 * approximate sum. This leaves every tree item on the path from this CPU to
 * the root holding its largest residual in the direction of inc which has
 * not been propagated yet, without knowledge of the tree topology. Each
 * addition is first tried on the probe counter, which holds the same state:
 * reverting an addition which reached the approximate sum would not restore
 * the residuals subtracted on the way. The path from a CPU to the root
 * holds less than the accuracy bound: more additions mean that a carry was
 * lost.
 */
static long saturate_worker_fn(void *data)
{
	struct saturate_data *sd = data;
	unsigned long steps;

	for (steps = 0; steps < sd->max_steps; steps++) {
		long approx = percpu_counter_tree_approximate_sum(sd->probe);

		percpu_counter_tree_add(sd->probe, sd->inc);
		if (percpu_counter_tree_approximate_sum(sd->probe) != approx) {
			copy_counter_state(sd->probe, sd->c);
			break;
		}
		percpu_counter_tree_add(sd->c, sd->inc);
	}
	if (steps == sd->max_steps)
		sd->overrun = true;
	return 0;
}

static void do_hpcc_test_saturated_compare(struct kunit *test, unsigned long batch_size,
					   long inc)
{
	struct percpu_counter_tree *c = &counter[0], *exact = &counter[1], probe;
	struct saturate_data sd = { .c = c, .probe = &probe, .inc = inc };
	unsigned long accuracy;
	long approx, precise, precise_min, precise_max;
	bool all_saturated;
	int ret, cpu;

	ret = percpu_counter_tree_init(&probe, batch_size, GFP_KERNEL);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		return;
	ret = init_counters(batch_size);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		goto destroy_probe;
	accuracy = percpu_counter_tree_approximate_accuracy(c);
	sd.max_steps = batch_size + accuracy;
	cpus_read_lock();
	for_each_online_cpu(cpu) {
		work_on_cpu(cpu, saturate_worker_fn, &sd);
		KUNIT_EXPECT_FALSE(test, sd.overrun);
		if (sd.overrun)
			break;
	}
	/* Whether every possible CPU was online, thus saturated. */
	all_saturated = num_online_cpus() == nr_cpu_ids;
	cpus_read_unlock();

	precise = percpu_counter_tree_precise_sum(c);
	approx = percpu_counter_tree_approximate_sum(c);
	kunit_info(test, "batch_size=%lu inc=%ld precise=%ld approx=%ld accuracy=%lu\n",
		   batch_size, inc, precise, approx, accuracy);

	check_residuals(test, c);
	check_residuals(test, &probe);

	/* Accuracy contract. */
	KUNIT_EXPECT_GE(test, approx, precise - (long)accuracy);
	KUNIT_EXPECT_LE(test, approx, precise + (long)accuracy);
	/* Saturating every possible CPU reaches the accuracy exactly. */
	if (all_saturated)
		KUNIT_EXPECT_EQ(test, (precise - approx) * inc, (long)accuracy);
	percpu_counter_tree_approximate_min_max_range(approx, accuracy,
						      &precise_min, &precise_max);
	KUNIT_EXPECT_GE(test, precise, precise_min);
	KUNIT_EXPECT_LE(test, precise, precise_max);

	/* Comparisons against a value. */
	KUNIT_EXPECT_EQ(test, 0, percpu_counter_tree_approximate_compare_value(c, precise));
	KUNIT_EXPECT_EQ(test, 0, percpu_counter_tree_precise_compare_value(c, precise));
	KUNIT_EXPECT_EQ(test, -1, percpu_counter_tree_precise_compare_value(c, precise - 1));
	KUNIT_EXPECT_EQ(test, 1, percpu_counter_tree_precise_compare_value(c, precise + 1));

	/* Comparisons against a counter with the same precise sum, exactly approximated. */
	percpu_counter_tree_set(exact, precise);
	KUNIT_EXPECT_EQ(test, percpu_counter_tree_approximate_sum(exact), precise);
	KUNIT_EXPECT_EQ(test, 0, percpu_counter_tree_approximate_compare(c, exact));
	KUNIT_EXPECT_EQ(test, 0, percpu_counter_tree_approximate_compare(exact, c));
	KUNIT_EXPECT_EQ(test, 0, percpu_counter_tree_precise_compare(c, exact));
	KUNIT_EXPECT_EQ(test, 0, percpu_counter_tree_precise_compare(exact, c));

	fini_counters();
destroy_probe:
	percpu_counter_tree_destroy(&probe);
}

/*
 * Saturate the unpropagated residuals of every tree item reachable from
 * online CPUs, upward and downward, which yields the largest distance
 * between the precise and approximate sums of a settled counter in each
 * direction, and check that the comparison
 * functions do not return a wrong definitive answer. Small batch sizes
 * are the most likely to expose margin errors, because the fan-out of
 * the tree levels can be larger than the batch size.
 */
static void hpcc_test_saturated_compare(struct kunit *test)
{
	unsigned long batch_size_order;

	for (batch_size_order = 1; batch_size_order <= 5; batch_size_order++) {
		do_hpcc_test_saturated_compare(test, 1UL << batch_size_order, 1);
		do_hpcc_test_saturated_compare(test, 1UL << batch_size_order, -1);
	}
}

static void hpcc_test_check_config(struct kunit *test)
{
#ifdef CONFIG_SMP
	KUNIT_EXPECT_EQ(test, 0, percpu_counter_tree_check_config());
#else
	kunit_skip(test, "The configuration table requires CONFIG_SMP");
#endif
}

static struct kunit_case hpcc_test_cases[] = {
	KUNIT_CASE(hpcc_test_check_config),
	KUNIT_CASE(hpcc_print_info),
	KUNIT_CASE(hpcc_test_single_thread_first),
	KUNIT_CASE(hpcc_test_single_thread_first_random),
	KUNIT_CASE_SLOW(hpcc_test_single_thread_random),
	KUNIT_CASE(hpcc_test_multi_thread_batch_increment),
	KUNIT_CASE_SLOW(hpcc_test_multi_thread_random_walk),
	KUNIT_CASE(hpcc_test_init_one),
	KUNIT_CASE(hpcc_test_set),
	KUNIT_CASE(hpcc_test_saturated_compare),
	KUNIT_CASE(hpcc_test_contended_carries),
	{}
};

static int hpcc_test_suite_init(struct kunit_suite *suite)
{
	hpcc_test_wq = alloc_workqueue("hpcc_test", WQ_PERCPU | WQ_CPU_INTENSIVE, 0);
	return hpcc_test_wq ? 0 : -ENOMEM;
}

static void hpcc_test_suite_exit(struct kunit_suite *suite)
{
	destroy_workqueue(hpcc_test_wq);
}

static struct kunit_suite hpcc_test_suite = {
	.name = "percpu_counter_tree",
	.suite_init = hpcc_test_suite_init,
	.suite_exit = hpcc_test_suite_exit,
	.test_cases = hpcc_test_cases,
};

kunit_test_suite(hpcc_test_suite);

MODULE_DESCRIPTION("Test cases for hierarchical per-CPU counters");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
