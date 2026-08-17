/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef _LINUX_FOLIO_POOL_H
#define _LINUX_FOLIO_POOL_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/page-flags.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/jump_label.h>
#include <linux/moduleparam.h>
#include <linux/cleanup.h>

DECLARE_STATIC_KEY_TRUE(folio_pool_enabled_key);

#define DEFINE_FOLIO_POOL_STATIC_KEY_PARAM(key_name, param_name, desc)		\
	DEFINE_STATIC_KEY_TRUE(key_name);					\
	static int key_name##_set(const char *val, const struct kernel_param *kp) \
	{									\
		bool enable;							\
		int ret = kstrtobool(val, &enable);				\
		if (ret)							\
			return ret;						\
		if (enable)							\
			static_branch_enable(&key_name);			\
		else								\
			static_branch_disable(&key_name);			\
		return 0;							\
	}									\
	static int key_name##_get(char *buffer, const struct kernel_param *kp)	\
	{									\
		return sprintf(buffer, "%c\n",					\
			       static_branch_likely(&key_name) ? 'Y' : 'N');	\
	}									\
	static const struct kernel_param_ops key_name##_ops = {			\
		.set = key_name##_set,						\
		.get = key_name##_get,						\
	};									\
	module_param_cb(param_name, &key_name##_ops, NULL, 0644);		\
	MODULE_PARM_DESC(param_name, desc)

#define DEFINE_FOLIO_POOL_STATIC_KEY_PARAM_FALSE(key_name, param_name, desc)	\
	DEFINE_STATIC_KEY_FALSE(key_name);					\
	static int key_name##_set(const char *val, const struct kernel_param *kp) \
	{									\
		bool enable;							\
		int ret = kstrtobool(val, &enable);				\
		if (ret)							\
			return ret;						\
		if (enable)							\
			static_branch_enable(&key_name);			\
		else								\
			static_branch_disable(&key_name);			\
		return 0;							\
	}									\
	static int key_name##_get(char *buffer, const struct kernel_param *kp)	\
	{									\
		return sprintf(buffer, "%c\n",					\
			       static_branch_unlikely(&key_name) ? 'Y' : 'N');	\
	}									\
	static const struct kernel_param_ops key_name##_ops = {			\
		.set = key_name##_set,						\
		.get = key_name##_get,						\
	};									\
	module_param_cb(param_name, &key_name##_ops, NULL, 0644);		\
	MODULE_PARM_DESC(param_name, desc)

#define FOLIO_POOL_64K_ORDER (PAGE_SHIFT < 16 ? 16 - PAGE_SHIFT : 0)

struct folio_pool_chunk {
	struct list_head link;
	struct folio *folio;
};

/*
 * 1. Variable-Sized Scratchpad (Core Bump Allocator Engine)
 */
struct folio_scratchpad {
	struct list_head chunks;
	void *free_ptr;
	size_t remaining;
	unsigned int chunk_order;
	struct static_key *key;
	spinlock_t lock;
};

#define FOLIO_SCRATCHPAD_INIT(name, _order) {				\
	.chunks = LIST_HEAD_INIT((name).chunks),			\
	.chunk_order = (_order),					\
	.key = NULL,							\
	.lock = __SPIN_LOCK_UNLOCKED((name).lock),			\
}

#define FOLIO_SCRATCHPAD_INIT_KEY(name, _order, _key) {			\
	.chunks = LIST_HEAD_INIT((name).chunks),			\
	.chunk_order = (_order),					\
	.key = (struct static_key *)(_key),				\
	.lock = __SPIN_LOCK_UNLOCKED((name).lock),			\
}

void folio_scratchpad_init(struct folio_scratchpad *sp, unsigned int order);
void folio_scratchpad_init_key(struct folio_scratchpad *sp, unsigned int order,
			       struct static_key *key);
void *folio_scratchpad_alloc(struct folio_scratchpad *sp, size_t size,
			     size_t align, gfp_t gfp);
void folio_scratchpad_reset(struct folio_scratchpad *sp);
void folio_scratchpad_free(struct folio_scratchpad *sp);
void folio_scratchpad_stats(struct folio_scratchpad *sp, unsigned int *nr_chunks,
			    size_t *chunk_size, size_t *tail_used);

DEFINE_FREE(folio_scratchpad, struct folio_scratchpad *, if (_T) folio_scratchpad_free(_T))

/**
 * is_folio_pool_ptr - Check whether an address resides in a folio pool/scratchpad
 * @ptr: Object pointer to test
 *
 * Direct-map large folios allocated via folio_alloc() are not slab pages,
 * unlike objects returned by kmalloc/kzalloc.
 */
static inline bool is_folio_pool_ptr(const void *ptr)
{
	return ptr && !is_vmalloc_addr(ptr) && !folio_test_slab(virt_to_folio(ptr));
}

/**
 * folio_pool_free_elem - Safely release a pool object or SLUB fallback element
 * @ptr: Object pointer to release
 *
 * If @ptr belongs to a direct-map large folio, individual deallocation is a safe
 * no-op (reclaimed in bulk by folio_scratchpad_free/reset). If @ptr was allocated
 * via SLUB/vmalloc fallback, releases it immediately.
 */
static inline void folio_pool_free_elem(const void *ptr)
{
	if (!ptr || is_folio_pool_ptr(ptr))
		return;
	kvfree(ptr);
}

static inline void folio_scratchpad_free_elem(const void *ptr)
{
	folio_pool_free_elem(ptr);
}

/**
 * folio_pool_realloc - Reallocate memory for an object, handling pool vs slab backing
 * @ptr: Existing object pointer (may be from folio_pool/scratchpad or SLUB)
 * @old_size: Size of original object
 * @new_size: Desired new size
 * @gfp: Allocation flags
 *
 * If @ptr is SLUB-backed, delegates directly to krealloc(). If @ptr resides
 * in a direct-map large folio, allocates a fresh @new_size buffer from SLUB
 * and copies @old_size bytes; the original scratchpad slot remains abandoned
 * until the entire scratchpad is released or reset at batch boundary.
 */
static inline void *folio_pool_realloc(void *ptr, size_t old_size,
				       size_t new_size, gfp_t gfp)
{
	void *new_ptr;

	if (!ptr)
		return kmalloc(new_size, gfp);

	if (!is_folio_pool_ptr(ptr))
		return krealloc(ptr, new_size, gfp);

	new_ptr = kmalloc(new_size, gfp);
	if (new_ptr)
		memcpy(new_ptr, ptr, min(old_size, new_size));
	return new_ptr;
}

static inline void *folio_scratchpad_realloc(void *ptr, size_t old_size,
					     size_t new_size, gfp_t gfp)
{
	return folio_pool_realloc(ptr, old_size, new_size, gfp);
}

/**
 * folio_scratchpad_alloc_obj - Allocate a typed object from an embedded scratchpad
 * @ptr: Pointer to container struct (e.g. nft_net)
 * @member: Name of the struct folio_scratchpad field (e.g. trans_scratchpad)
 * @type: Type of object being allocated
 * @gfp: Allocation flags
 */
#define folio_scratchpad_alloc_obj(ptr, member, type, gfp)			\
	((type *)folio_scratchpad_alloc(&(ptr)->member,				\
					sizeof(type),				\
					__alignof__(type),			\
					gfp))

/**
 * folio_scratchpad_alloc_bytes - Allocate variable-sized bytes from an embedded scratchpad
 * @ptr: Pointer to container struct (e.g. nft_net)
 * @member: Name of the struct folio_scratchpad field (e.g. trans_scratchpad)
 * @size: Size of memory to allocate
 * @align: Alignment requirement
 * @gfp: Allocation flags
 */
#define folio_scratchpad_alloc_bytes(ptr, member, size, align, gfp)		\
	folio_scratchpad_alloc(&(ptr)->member, size, align, gfp)

/**
 * folio_scratchpad_alloc_type - Allocate a typed object from a scratchpad pointer
 * @sp: Pointer to struct folio_scratchpad
 * @type: Type of object being allocated
 * @gfp: Allocation flags
 */
#define folio_scratchpad_alloc_type(sp, type, gfp)				\
	((type *)folio_scratchpad_alloc(sp,					\
					sizeof(type),				\
					__alignof__(type),			\
					gfp))

/*
 * 2. Fixed-Slot Uniform Pool (Specialized Thin Wrapper on Scratchpad)
 */
struct folio_pool {
	struct folio_scratchpad base;
	size_t elem_size;
	size_t elem_align;
};

#define FOLIO_POOL_INIT(name, _elem_size, _order) {			\
	.base = FOLIO_SCRATCHPAD_INIT((name).base, _order),		\
	.elem_size = (_elem_size),					\
	.elem_align = ((_elem_size) > sizeof(void *) ? (_elem_size) : sizeof(void *)), \
}

#define FOLIO_POOL_INIT_KEY(name, _elem_size, _order, _key) {		\
	.base = FOLIO_SCRATCHPAD_INIT_KEY((name).base, _order, _key),	\
	.elem_size = (_elem_size),					\
	.elem_align = ((_elem_size) > sizeof(void *) ? (_elem_size) : sizeof(void *)), \
}

void folio_pool_init(struct folio_pool *fp, size_t elem_size, unsigned int order);
void folio_pool_init_key(struct folio_pool *fp, size_t elem_size, unsigned int order,
			 struct static_key *key);
void folio_pool_init_align(struct folio_pool *fp, size_t elem_size,
			   size_t elem_align, unsigned int order);

static inline void *folio_pool_alloc(struct folio_pool *fp, gfp_t gfp)
{
	return folio_scratchpad_alloc(&fp->base, fp->elem_size, fp->elem_align, gfp);
}

static inline void folio_pool_free(struct folio_pool *fp)
{
	folio_scratchpad_free(&fp->base);
}

static inline void folio_pool_stats(struct folio_pool *fp, unsigned int *nr_chunks,
				    size_t *chunk_size, size_t *tail_used)
{
	folio_scratchpad_stats(&fp->base, nr_chunks, chunk_size, tail_used);
}

/**
 * folio_pool_alloc_obj - Allocate a typed object from a container's embedded folio_pool
 * @ptr: Pointer to container struct (e.g. env)
 * @member: Name of the struct folio_pool field (e.g. state_pool)
 * @type: Type of object being allocated (e.g. struct bpf_verifier_stack_elem)
 * @gfp: Allocation flags
 */
#define folio_pool_alloc_obj(ptr, member, type, gfp)				\
	((type *)folio_pool_alloc(&(ptr)->member, gfp))

/**
 * folio_pool_alloc_type - Allocate a typed object from a struct folio_pool pointer
 * @fp: Pointer to struct folio_pool
 * @type: Type of object being allocated
 * @gfp: Allocation flags
 */
#define folio_pool_alloc_type(fp, type, gfp)					\
	((type *)folio_pool_alloc(fp, gfp))

#endif /* _LINUX_FOLIO_POOL_H */
