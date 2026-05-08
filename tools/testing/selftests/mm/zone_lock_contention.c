// SPDX-License-Identifier: GPL-2.0
/*
 * zone_lock_contention.c - Generate zone->lock contention for tracepoint testing
 *
 * Spawns multiple threads that rapidly allocate and free pages to force
 * PCP (per-cpu pageset) drains and refills, which acquire zone->lock via
 * free_pcppages_bulk() and rmqueue_bulk().
 *
 * Reducing percpu_pagelist_high_fraction makes PCP lists smaller, causing
 * more frequent zone->lock acquisitions and thus more contention.
 *
 * Usage: zone_lock_contention [duration_sec] [nr_threads]
 *        Defaults: 5 seconds, 4 threads
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <errno.h>
#include <time.h>

/* Each thread mmaps/touches/munmaps in a loop to churn pages */
#define CHUNK_SIZE	(2 * 1024 * 1024)	/* 2 MB per iteration */
#define PAGE_SZ		4096

static volatile int stop;

struct thread_stats {
	unsigned long iterations;
	unsigned long pages_touched;
};

static void *churn_thread(void *arg)
{
	struct thread_stats *stats = arg;
	unsigned long iter = 0;
	unsigned long pages = 0;

	while (!stop) {
		char *p;
		size_t i;

		p = mmap(NULL, CHUNK_SIZE, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
		if (p == MAP_FAILED) {
			perror("mmap");
			break;
		}

		/* Touch every page to ensure allocation */
		for (i = 0; i < CHUNK_SIZE; i += PAGE_SZ)
			p[i] = 1;

		pages += CHUNK_SIZE / PAGE_SZ;

		/* Free pages back - forces PCP drain */
		munmap(p, CHUNK_SIZE);
		iter++;
	}

	stats->iterations = iter;
	stats->pages_touched = pages;
	return NULL;
}

static int write_sysctl(const char *path, const char *val)
{
	FILE *f = fopen(path, "w");

	if (!f)
		return -1;
	fputs(val, f);
	fclose(f);
	return 0;
}

static int read_sysctl(const char *path, char *buf, size_t len)
{
	FILE *f = fopen(path, "r");

	if (!f)
		return -1;
	if (!fgets(buf, len, f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	return 0;
}

int main(int argc, char **argv)
{
	int duration = 5;
	int nr_threads = 4;
	char orig_fraction[32] = "";
	const char *sysctl_path = "/proc/sys/vm/percpu_pagelist_high_fraction";
	pthread_t *threads;
	struct thread_stats *stats;
	unsigned long total_iter = 0, total_pages = 0;
	int i;

	if (argc > 1)
		duration = atoi(argv[1]);
	if (argc > 2)
		nr_threads = atoi(argv[2]);

	if (duration <= 0 || nr_threads <= 0) {
		fprintf(stderr, "Usage: %s [duration_sec] [nr_threads]\n", argv[0]);
		return 1;
	}

	printf("zone_lock_contention: %d threads, %d seconds\n",
	       nr_threads, duration);

	/* Shrink PCP lists to force more zone->lock acquisitions */
	read_sysctl(sysctl_path, orig_fraction, sizeof(orig_fraction));
	if (write_sysctl(sysctl_path, "100") < 0)
		fprintf(stderr, "WARNING: cannot write %s (not root?)\n",
			sysctl_path);
	else
		printf("Set percpu_pagelist_high_fraction=100 (was %s)\n",
		       orig_fraction);

	threads = calloc(nr_threads, sizeof(*threads));
	stats = calloc(nr_threads, sizeof(*stats));
	if (!threads || !stats) {
		perror("calloc");
		return 1;
	}

	for (i = 0; i < nr_threads; i++) {
		if (pthread_create(&threads[i], NULL, churn_thread, &stats[i])) {
			perror("pthread_create");
			return 1;
		}
	}

	sleep(duration);
	stop = 1;

	for (i = 0; i < nr_threads; i++) {
		pthread_join(threads[i], NULL);
		total_iter += stats[i].iterations;
		total_pages += stats[i].pages_touched;
	}

	printf("Total: %lu iterations, %lu pages (%lu MB) churned\n",
	       total_iter, total_pages,
	       (total_pages * PAGE_SZ) / (1024 * 1024));

	/* Restore original sysctl */
	if (orig_fraction[0]) {
		/* Strip trailing newline */
		orig_fraction[strcspn(orig_fraction, "\n")] = '\0';
		write_sysctl(sysctl_path, orig_fraction);
		printf("Restored percpu_pagelist_high_fraction=%s\n",
		       orig_fraction);
	}

	free(threads);
	free(stats);
	return 0;
}
