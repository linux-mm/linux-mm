/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Virtual swap space
 *
 * Copyright (C) 2026 Nhat Pham
 */
#ifndef _MM_VSWAP_H
#define _MM_VSWAP_H


#include <linux/swap.h>

struct zswap_entry;

static inline bool swap_is_vswap(struct swap_info_struct *si)
{
	return si->flags & SWP_VSWAP;
}

#ifdef CONFIG_VSWAP

#include "swap.h"
#include "swap_table.h"

extern struct swap_info_struct *vswap_si;

/* Rmap cache-only helpers for physical cluster Pointer-tagged entries */

static inline void swap_rmap_mark_cache_only(struct swap_cluster_info *ci,
					     unsigned int off)
{
	atomic_long_t *table;

	table = rcu_dereference_check(ci->table, true);
	atomic_long_or(SWP_RMAP_CACHE_ONLY, &table[off]);
}

static inline void swap_rmap_clear_cache_only(struct swap_cluster_info *ci,
					      unsigned int off)
{
	atomic_long_t *table;

	table = rcu_dereference_check(ci->table, true);
	atomic_long_and(~SWP_RMAP_CACHE_ONLY, &table[off]);
}

static inline bool swap_rmap_is_cache_only(struct swap_cluster_info *ci,
					   unsigned int off)
{
	atomic_long_t *table;
	bool ret;

	VM_WARN_ON_ONCE(off >= SWAPFILE_CLUSTER);
	rcu_read_lock();
	table = rcu_dereference(ci->table);
	ret = table && (atomic_long_read(&table[off]) & SWP_RMAP_CACHE_ONLY);
	rcu_read_unlock();
	return ret;
}

/*
 * Virtual table entry encoding for vswap clusters.
 *
 * Each entry in ci_dyn->virtual_table stores the backing type and
 * pointer for a virtual swap slot. Tag in low 3 bits, payload in
 * upper 61 bits.
 *
 *   NONE:   |----- 0000 ------|000|  — free / unbacked
 *   PHYS:   |-- (type:5,off:N)|001|  — on a physical swapfile (shifted)
 *   ZERO:   |----- 0000 ------|010|  — zero-filled page
 *   ZSWAP:  |--- zswap_entry* |011|  — compressed in zswap (tag in low bits)
 *   FOLIO:  |--- folio* ------|100|  — in-memory only (tag in low bits)
 *
 * PHYS payloads are shifted left by 3. Pointer payloads (ZSWAP, FOLIO)
 * are stored directly with the tag OR'd into the low bits (kernel
 * pointers are >= 8-byte aligned, same approach as xarray).
 */
enum vswap_backing_type {
	VSWAP_NONE	= 0,
	VSWAP_SWAPFILE	= 1,
	VSWAP_ZERO	= 2,
	VSWAP_ZSWAP	= 3,
	VSWAP_FOLIO	= 4,
};

#define VTABLE_TAG_BITS		3
#define VTABLE_TAG_MASK		((1UL << VTABLE_TAG_BITS) - 1)

static inline enum vswap_backing_type vtable_type(unsigned long vt)
{
	return vt & VTABLE_TAG_MASK;
}

static inline unsigned long vtable_payload(unsigned long vt)
{
	return vt >> VTABLE_TAG_BITS;
}

static inline unsigned long vtable_mk(enum vswap_backing_type type,
				       unsigned long payload)
{
	return (payload << VTABLE_TAG_BITS) | type;
}

static inline unsigned long vtable_mk_none(void)
{
	return 0;
}

static inline unsigned long vtable_mk_phys(swp_entry_t entry)
{
	return vtable_mk(VSWAP_SWAPFILE, entry.val);
}

static inline swp_entry_t vtable_to_phys(unsigned long vt)
{
	swp_entry_t entry;

	VM_WARN_ON(vtable_type(vt) != VSWAP_SWAPFILE);
	entry.val = vtable_payload(vt);
	return entry;
}

static inline unsigned long vtable_mk_zero(void)
{
	return VSWAP_ZERO;
}

static inline unsigned long vtable_mk_zswap(struct zswap_entry *ze)
{
	return (unsigned long)ze | VSWAP_ZSWAP;
}

static inline struct zswap_entry *vtable_to_zswap(unsigned long vt)
{
	VM_WARN_ON(vtable_type(vt) != VSWAP_ZSWAP);
	return (struct zswap_entry *)(vt & ~VTABLE_TAG_MASK);
}

static inline unsigned long vtable_mk_folio(struct folio *folio)
{
	return (unsigned long)folio | VSWAP_FOLIO;
}

static inline struct folio *vtable_to_folio(unsigned long vt)
{
	VM_WARN_ON(vtable_type(vt) != VSWAP_FOLIO);
	return (struct folio *)(vt & ~VTABLE_TAG_MASK);
}

/* Virtual table accessors */

static inline unsigned long __vtable_get(struct swap_cluster_info_dynamic *ci_dyn,
					 unsigned int off)
{
	VM_WARN_ON_ONCE(off >= SWAPFILE_CLUSTER);
	return atomic_long_read(&ci_dyn->virtual_table[off]);
}

static inline void __vtable_set(struct swap_cluster_info_dynamic *ci_dyn,
				unsigned int off, unsigned long vt)
{
	VM_WARN_ON_ONCE(off >= SWAPFILE_CLUSTER);
	atomic_long_set(&ci_dyn->virtual_table[off], vt);
}

/*
 * Lock a vswap cluster and return the dynamic info + slot offset.
 * Returns NULL if cluster not found.
 * Caller must spin_unlock(&ci_dyn->ci.lock) when done.
 */
static inline struct swap_cluster_info_dynamic *
vswap_lock_cluster(swp_entry_t entry, unsigned int *voff)
{
	struct swap_cluster_info *ci;
	struct swap_cluster_info_dynamic *ci_dyn;

	ci = __swap_entry_to_cluster(entry);
	if (!ci)
		return NULL;
	ci_dyn = container_of(ci, struct swap_cluster_info_dynamic, ci);
	*voff = swp_cluster_offset(entry);
	spin_lock(&ci->lock);
	return ci_dyn;
}

/* High-level vswap lookup */

static inline swp_entry_t vswap_to_phys(swp_entry_t entry)
{
	struct swap_cluster_info_dynamic *ci_dyn;
	unsigned int voff;
	unsigned long vt;

	ci_dyn = vswap_lock_cluster(entry, &voff);
	if (!ci_dyn)
		return (swp_entry_t){};

	vt = __vtable_get(ci_dyn, voff);
	spin_unlock(&ci_dyn->ci.lock);

	if (vtable_type(vt) != VSWAP_SWAPFILE)
		return (swp_entry_t){};

	return vtable_to_phys(vt);
}

/* Zswap entry helpers — store/load/erase in virtual_table */

void vswap_release_backing(struct swap_cluster_info *ci,
			   unsigned int ci_start, unsigned int nr);

static inline void vswap_zswap_store(swp_entry_t entry,
				     struct zswap_entry *ze)
{
	struct swap_cluster_info_dynamic *ci_dyn;
	unsigned int voff;

	ci_dyn = vswap_lock_cluster(entry, &voff);
	if (!ci_dyn)
		return;
	vswap_release_backing(&ci_dyn->ci, voff, 1);
	__vtable_set(ci_dyn, voff, vtable_mk_zswap(ze));
	spin_unlock(&ci_dyn->ci.lock);
}

static inline struct zswap_entry *vswap_zswap_load(swp_entry_t entry)
{
	struct swap_cluster_info_dynamic *ci_dyn;
	unsigned int voff;
	unsigned long vt;

	ci_dyn = vswap_lock_cluster(entry, &voff);
	if (!ci_dyn)
		return NULL;
	vt = __vtable_get(ci_dyn, voff);
	spin_unlock(&ci_dyn->ci.lock);

	if (vtable_type(vt) != VSWAP_ZSWAP)
		return NULL;
	return vtable_to_zswap(vt);
}


void vswap_store_folio(swp_entry_t entry, struct folio *folio);
void vswap_prepare_writeout(swp_entry_t entry, struct folio *folio);

/*
 * Check that all nr vtable entries starting at entry have the same
 * backing type. Returns the number of matching entries (< nr on
 * mismatch).
 */
static inline int vswap_check_backing(swp_entry_t entry, int nr,
				      enum vswap_backing_type *typep)
{
	struct swap_cluster_info_dynamic *ci_dyn;
	enum vswap_backing_type first_type;
	unsigned int voff;
	unsigned long vt;
	swp_entry_t first_phys;
	int i;

	ci_dyn = vswap_lock_cluster(entry, &voff);
	if (!ci_dyn)
		return 0;

	for (i = 0; i < nr; i++) {
		vt = __vtable_get(ci_dyn, voff + i);
		if (!i) {
			first_type = vtable_type(vt);
			if (first_type == VSWAP_SWAPFILE)
				first_phys = vtable_to_phys(vt);
		} else if (vtable_type(vt) != first_type) {
			break;
		} else if (first_type == VSWAP_SWAPFILE &&
			   vtable_to_phys(vt).val != first_phys.val + i) {
			break;
		}
	}
	spin_unlock(&ci_dyn->ci.lock);

	if (typep)
		*typep = first_type;
	return i;
}

static inline bool vswap_swapfile_backed(swp_entry_t entry, int nr)
{
	enum vswap_backing_type type;

	return vswap_check_backing(entry, nr, &type) == nr &&
	       type == VSWAP_SWAPFILE;
}

static inline bool vswap_can_swapin_thp(swp_entry_t entry, int nr)
{
	enum vswap_backing_type type;

	return vswap_check_backing(entry, nr, &type) == nr &&
	       (type == VSWAP_ZERO || type == VSWAP_SWAPFILE);
}

static inline int vswap_cluster_alloc_vtable(struct swap_cluster_info_dynamic *ci_dyn)
{
	ci_dyn->virtual_table = kcalloc(SWAPFILE_CLUSTER,
					sizeof(*ci_dyn->virtual_table),
					GFP_ATOMIC);
	return ci_dyn->virtual_table ? 0 : -ENOMEM;
}

static inline void vswap_cluster_free_vtable(struct swap_cluster_info *ci)
{
	struct swap_cluster_info_dynamic *ci_dyn;

	ci_dyn = container_of(ci, struct swap_cluster_info_dynamic, ci);
	kfree(ci_dyn->virtual_table);
	ci_dyn->virtual_table = NULL;
}

/* Low-level setter for callers already holding the cluster lock */
static inline void vswap_set_zswap(struct swap_cluster_info *ci,
				   unsigned int ci_off,
				   struct zswap_entry *ze)
{
	struct swap_cluster_info_dynamic *ci_dyn;

	ci_dyn = container_of(ci, struct swap_cluster_info_dynamic, ci);
	__vtable_set(ci_dyn, ci_off, vtable_mk_zswap(ze));
}

/* Zeromap helpers — test/set ZERO backing in virtual_table */

static inline bool vswap_test_zero(struct swap_cluster_info *ci,
				   unsigned int ci_off)
{
	struct swap_cluster_info_dynamic *ci_dyn;

	ci_dyn = container_of(ci, struct swap_cluster_info_dynamic, ci);
	return vtable_type(__vtable_get(ci_dyn, ci_off)) == VSWAP_ZERO;
}

static inline void vswap_set_zero(struct swap_cluster_info *ci,
				  unsigned int ci_off)
{
	struct swap_cluster_info_dynamic *ci_dyn;

	ci_dyn = container_of(ci, struct swap_cluster_info_dynamic, ci);
	__vtable_set(ci_dyn, ci_off, vtable_mk_zero());
}

#else /* !CONFIG_VSWAP */

static inline swp_entry_t vswap_to_phys(swp_entry_t entry)
{
	return (swp_entry_t){};
}

static inline bool vswap_swapfile_backed(swp_entry_t entry, int nr)
{
	return false;
}

static inline bool swap_rmap_is_cache_only(struct swap_cluster_info *ci,
					   unsigned int off)
{
	return false;
}

static inline void vswap_release_backing(struct swap_cluster_info *ci,
					 unsigned int ci_start,
					 unsigned int nr) {}

static inline void vswap_zswap_store(swp_entry_t entry,
				     struct zswap_entry *ze) {}

static inline struct zswap_entry *vswap_zswap_load(swp_entry_t entry)
{
	return NULL;
}

static inline void vswap_store_folio(swp_entry_t entry,
				     struct folio *folio) {}
static inline void vswap_prepare_writeout(swp_entry_t entry,
					  struct folio *folio) {}

static inline bool vswap_can_swapin_thp(swp_entry_t entry, int nr)
{
	return false;
}

struct swap_cluster_info_dynamic;
static inline int vswap_cluster_alloc_vtable(struct swap_cluster_info_dynamic *ci_dyn)
{
	return 0;
}

static inline void vswap_cluster_free_vtable(struct swap_cluster_info *ci) {}

static inline void vswap_set_zswap(struct swap_cluster_info *ci,
				   unsigned int ci_off,
				   struct zswap_entry *ze) {}

static inline bool vswap_test_zero(struct swap_cluster_info *ci,
				   unsigned int ci_off)
{
	return false;
}

static inline void vswap_set_zero(struct swap_cluster_info *ci,
				  unsigned int ci_off) {}

#endif /* CONFIG_VSWAP */

/*
 * Test a per-backend swap flag (SWP_SYNCHRONOUS_IO, SWP_STABLE_WRITES, ...)
 * for @entry. For a vswap entry the property belongs to the current
 * physical backing, not vswap_si — resolve and test that. Returns false
 * for zswap/zero/unbacked vswap entries: they don't go through bdev IO,
 * so per-bdev flags don't apply.
 */
static inline bool swap_entry_backend_has_flag(struct swap_info_struct *si,
					       swp_entry_t entry,
					       unsigned long flag)
{
	struct swap_info_struct *phys_si;
	swp_entry_t phys;
	bool has_flag;

	if (!swap_is_vswap(si))
		return data_race(si->flags & flag);

	phys = vswap_to_phys(entry);
	if (!phys.val)
		return false;

	phys_si = get_swap_device(phys);
	if (!phys_si)
		return false;

	has_flag = data_race(phys_si->flags & flag);
	put_swap_device(phys_si);
	return has_flag;
}

#endif /* _MM_VSWAP_H */
