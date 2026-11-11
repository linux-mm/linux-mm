/* SPDX-License-Identifier: GPL-2.0+ OR MIT */
/*
 * Minimal userspace environment to build lib/percpu_counter_tree.c.
 *
 * The test is single-threaded: per-CPU data is an array indexed by the
 * simulated current CPU, and atomic operations are plain operations.
 */
#ifndef _HPCC_SHIM_H
#define _HPCC_SHIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* From tools/include. */
#include <linux/compiler.h>
#include <linux/gfp_types.h>
#include <linux/init.h>
#include <linux/kconfig.h>
#include <linux/kernel.h>
#include <linux/types.h>

#define __ro_after_init
#define __percpu
#define SMP_CACHE_BYTES			64
#define ____cacheline_aligned_in_smp	__aligned(SMP_CACHE_BYTES)
#define S8_MAX				INT8_MAX

#undef abs
#define abs(x) ({ typeof(x) __x = (x); __x < 0 ? -__x : __x; })

#define pr_err(...)		fprintf(stderr, __VA_ARGS__)
#define pr_info(...)		do { if (0) printf(__VA_ARGS__); } while (0)

extern unsigned long hpcc_nr_warnings;

#define WARN(cond, ...) ({						\
	int __ret_warn = !!(cond);					\
	if (__ret_warn) {						\
		hpcc_nr_warnings++;					\
		fprintf(stderr, "WARNING at %s:%d: ", __FILE__, __LINE__); \
		fprintf(stderr, __VA_ARGS__);				\
	}								\
	__ret_warn;							\
})
#define WARN_ON(cond)		WARN(cond, "%s\n", #cond)
#define WARN_ON_ONCE(cond)	WARN_ON(cond)

static inline int get_count_order(unsigned int count)
{
	int order;

	if (!count)
		return -1;
	order = 31 - __builtin_clz(count);
	if (count & (count - 1))
		order++;
	return order;
}

#define atomic_long_read(v)			READ_ONCE((v)->counter)
#define atomic_long_set(v, i)			WRITE_ONCE((v)->counter, (i))
#define atomic_long_add(i, v)			((void)((v)->counter += (i)))
#define atomic_long_add_return_relaxed(i, v)	((v)->counter += (i))
#define atomic_long_add_return_release(i, v)	((v)->counter += (i))
#define smp_rmb()				barrier()
/* The harness runs on one thread: ordering primitives are plain accesses. */
#define smp_load_acquire(p)			READ_ONCE(*(p))
#define smp_store_release(p, v)			WRITE_ONCE(*(p), (v))

static inline bool atomic_long_try_cmpxchg_relaxed(atomic_long_t *v, long *old, long new)
{
	if (v->counter == *old) {
		v->counter = new;
		return true;
	}
	*old = v->counter;
	return false;
}

/* Simulated topology, and CPU on which the current code runs. */
extern unsigned int nr_cpu_ids;
extern unsigned int hpcc_current_cpu;

#define raw_smp_processor_id()		hpcc_current_cpu
#define for_each_possible_cpu(cpu)	for ((cpu) = 0; (cpu) < (int)nr_cpu_ids; (cpu)++)

/*
 * Each per-CPU allocation reserves HPCC_PCPU_UNIT bytes per CPU, so
 * per_cpu_ptr() can be applied to a pointer within an allocation.
 */
#define HPCC_PCPU_UNIT	64

/* Outstanding allocations, to check for leaks. */
extern long hpcc_nr_allocs;

static inline void *hpcc_alloc_account(void *p)
{
	if (p)
		hpcc_nr_allocs++;
	return p;
}

static inline void hpcc_free_account(void *p)
{
	if (p)
		hpcc_nr_allocs--;
}

static inline void *__alloc_percpu_gfp(size_t size, size_t align, gfp_t gfp)
{
	if (size > HPCC_PCPU_UNIT || align > HPCC_PCPU_UNIT)
		return NULL;
	return hpcc_alloc_account(calloc(nr_cpu_ids, HPCC_PCPU_UNIT));
}

static inline void free_percpu(void *ptr)
{
	hpcc_free_account(ptr);
	free(ptr);
}

#define SLAB_PANIC	0

struct kmem_cache {
	size_t size;
	size_t align;
};

static inline struct kmem_cache *kmem_cache_create(const char *name, unsigned int size,
						   unsigned int align, unsigned int flags,
						   void (*ctor)(void *))
{
	struct kmem_cache *s = malloc(sizeof(*s));

	if (!s)
		abort();
	s->size = size;
	s->align = align;
	return s;
}

static inline void kmem_cache_destroy(struct kmem_cache *s)
{
	free(s);
}

static inline void *kmem_cache_zalloc(struct kmem_cache *s, gfp_t gfp)
{
	/* The allocation size needs to be a multiple of the alignment. */
	void *p = aligned_alloc(s->align, (s->size + s->align - 1) / s->align * s->align);

	if (p)
		memset(p, 0, s->size);
	return hpcc_alloc_account(p);
}

static inline void kmem_cache_free(struct kmem_cache *s, void *p)
{
	hpcc_free_account(p);
	free(p);
}

static inline void *kzalloc(size_t size, gfp_t gfp)
{
	return hpcc_alloc_account(calloc(1, size));
}

static inline void kfree(void *p)
{
	hpcc_free_account(p);
	free(p);
}

#define per_cpu_ptr(ptr, cpu)	\
	((typeof(ptr))((char *)(ptr) + (size_t)(cpu) * HPCC_PCPU_UNIT))

/* Level 0 counters are the only per-CPU data: s8. */
static inline bool __hpcc_this_cpu_try_cmpxchg(s8 *pcp, s8 *old, s8 new)
{
	s8 *p = per_cpu_ptr(pcp, hpcc_current_cpu);

	if (*p != *old) {
		*old = *p;
		return false;
	}
	*p = new;
	return true;
}

#define this_cpu_read(pcp)	(*per_cpu_ptr(&(pcp), hpcc_current_cpu))
#define this_cpu_try_cmpxchg(pcp, ovalp, nval)	\
	__hpcc_this_cpu_try_cmpxchg(&(pcp), (ovalp), (nval))

#endif /* _HPCC_SHIM_H */
