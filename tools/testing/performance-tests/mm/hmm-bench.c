// SPDX-License-Identifier: GPL-2.0
#include <limits.h>
#include <sys/time.h>
#include <mm/hmm-common.h>

struct benchmark_results {
	double sys_to_dev_time;
	double dev_to_sys_time;
	double throughput_s2d;
	double throughput_d2s;
};

static double get_time_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (tv.tv_sec * 1000.0) + (tv.tv_usec / 1000.0);
}

static void print_benchmark_results(const char *test_name, size_t buffer_size,
				     struct benchmark_results *thp,
				     struct benchmark_results *regular)
{
	double s2d_improvement = ((regular->sys_to_dev_time - thp->sys_to_dev_time) /
				 regular->sys_to_dev_time) * 100.0;
	double d2s_improvement = ((regular->dev_to_sys_time - thp->dev_to_sys_time) /
				 regular->dev_to_sys_time) * 100.0;
	double throughput_s2d_improvement = ((thp->throughput_s2d - regular->throughput_s2d) /
					    regular->throughput_s2d) * 100.0;
	double throughput_d2s_improvement = ((thp->throughput_d2s - regular->throughput_d2s) /
					    regular->throughput_d2s) * 100.0;

	printf("\n=== %s (%.1f MB) ===\n", test_name, buffer_size / (1024.0 * 1024.0));
	printf("                     | With THP        | Without THP     | Improvement\n");
	printf("---------------------------------------------------------------------\n");
	printf("Sys->Dev Migration   | %.3f ms        | %.3f ms        | %.1f%%\n",
	       thp->sys_to_dev_time, regular->sys_to_dev_time, s2d_improvement);
	printf("Dev->Sys Migration   | %.3f ms        | %.3f ms        | %.1f%%\n",
	       thp->dev_to_sys_time, regular->dev_to_sys_time, d2s_improvement);
	printf("S->D Throughput      | %.2f GB/s      | %.2f GB/s      | %.1f%%\n",
	       thp->throughput_s2d, regular->throughput_s2d, throughput_s2d_improvement);
	printf("D->S Throughput      | %.2f GB/s      | %.2f GB/s      | %.1f%%\n",
	       thp->throughput_d2s, regular->throughput_d2s, throughput_d2s_improvement);
	fflush(stdout);
}

/*
 * Run a single migration benchmark
 * fd: file descriptor for hmm device
 * use_thp: whether to use THP
 * buffer_size: size of buffer to allocate
 * iterations: number of iterations
 * results: where to store results
 */
static inline int run_migration_benchmark(int fd, int use_thp, size_t buffer_size,
					   int iterations, struct benchmark_results *results)
{
	struct hmm_buffer *buffer;
	unsigned long npages = buffer_size / sysconf(_SC_PAGESIZE);
	double start, end;
	double s2d_total = 0, d2s_total = 0;
	int ret, i;
	int *ptr;

	buffer = hmm_buffer_alloc(buffer_size, buffer_size,
				  PROT_READ | PROT_WRITE,
				  MAP_PRIVATE | MAP_ANONYMOUS, -1);
	if (!buffer) {
		ret = -1;
		goto cleanup;
	}
	memset(buffer->mirror, 0xFF, buffer_size);

	/* Apply THP hint if requested */
	if (use_thp)
		ret = madvise(buffer->ptr, buffer_size, MADV_HUGEPAGE);
	else
		ret = madvise(buffer->ptr, buffer_size, MADV_NOHUGEPAGE);

	if (ret)
		goto cleanup;

	/* Initialize memory to make sure pages are allocated */
	ptr = (int *)buffer->ptr;
	for (i = 0; i < buffer_size / sizeof(int); i++)
		ptr[i] = i & 0xFF;

	/* Warmup iteration */
	ret = hmm_migrate_sys_to_dev(fd, buffer, npages);
	if (ret)
		goto cleanup;

	ret = hmm_migrate_dev_to_sys(fd, buffer, npages);
	if (ret)
		goto cleanup;

	/* Benchmark iterations */
	for (i = 0; i < iterations; i++) {
		/* System to device migration */
		start = get_time_ms();

		ret = hmm_migrate_sys_to_dev(fd, buffer, npages);
		if (ret)
			goto cleanup;

		end = get_time_ms();
		s2d_total += (end - start);

		/* Device to system migration */
		start = get_time_ms();

		ret = hmm_migrate_dev_to_sys(fd, buffer, npages);
		if (ret)
			goto cleanup;

		end = get_time_ms();
		d2s_total += (end - start);
	}

	/* Calculate average times and throughput */
	results->sys_to_dev_time = s2d_total / iterations;
	results->dev_to_sys_time = d2s_total / iterations;
	results->throughput_s2d = (buffer_size / (1024.0 * 1024.0 * 1024.0)) /
				 (results->sys_to_dev_time / 1000.0);
	results->throughput_d2s = (buffer_size / (1024.0 * 1024.0 * 1024.0)) /
				 (results->dev_to_sys_time / 1000.0);

cleanup:
	hmm_buffer_free(buffer);
	return ret;
}

/*
 * Benchmark THP migration with different buffer sizes
 */
TEST_F_TIMEOUT(hmm, benchmark_thp_migration, 120)
{
	struct benchmark_results thp_results, regular_results;
	size_t thp_size = pmd_psize();
	int iterations = 5;

	if (!thp_size)
		thp_size = TWOMEG;

	printf("\nHMM THP Migration Benchmark\n");
	printf("---------------------------\n");
	printf("System page size: %ld bytes\n", sysconf(_SC_PAGESIZE));

	/* Test different buffer sizes */
	size_t test_sizes[] = {
		thp_size / 4,      /* quarter THP */
		thp_size / 2,      /* half THP */
		thp_size,          /* single THP */
		thp_size * 2,      /* two THPs */
		thp_size * 4,      /* four THPs */
		thp_size * 8,      /* eight THPs */
	};

	static const char *const test_names[] = {
		"Small Buffer",
		"Half THP Size",
		"Single THP Size",
		"Two THP Size",
		"Four THP Size",
		"Eight THP Size",
	};

	int num_tests = ARRAY_SIZE(test_sizes);

	/* Run all tests */
	for (int i = 0; i < num_tests; i++) {
		/* Skip test sizes exceeding INT_MAX to avoid overflow */
		if (test_sizes[i] > INT_MAX)
			break;

		/* Test with THP */
		ASSERT_EQ(run_migration_benchmark(self->fd, 1, test_sizes[i],
					iterations, &thp_results), 0);

		/* Test without THP */
		ASSERT_EQ(run_migration_benchmark(self->fd, 0, test_sizes[i],
					iterations, &regular_results), 0);

		/* Print results */
		print_benchmark_results(test_names[i], test_sizes[i],
					&thp_results, &regular_results);
	}
}

TEST_HARNESS_MAIN
