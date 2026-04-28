/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MIGRATE_COPY_OFFLOAD_H
#define _LINUX_MIGRATE_COPY_OFFLOAD_H

#include <linux/errno.h>
#include <linux/jump_label.h>
#include <linux/srcu.h>
#include <linux/types.h>

struct list_head;
struct module;

#define MIGRATOR_NAME_LEN 32

/**
 * struct migrator - batch-copy provider for page migration.
 * @name: name of the provider.
 * @offload_copy: copy @folio_cnt folios from @src_list to @dst_list.
 *
 *	The migrator may inspect @folio_cnt to decide whether the batch
 * 	is worth offloading, e.g. skip when the batch is too small to
 * 	amortize setup cost. If returns error, the core falls back to CPU copy.
 *
 * @owner: module providing the migrator.
 */
struct migrator {
	char name[MIGRATOR_NAME_LEN];
	int (*offload_copy)(struct list_head *dst_list,
			    struct list_head *src_list,
			    unsigned int folio_cnt);
	struct module *owner;
};

#ifdef CONFIG_MIGRATION_COPY_OFFLOAD
extern struct static_key_false migrate_offload_enabled;
extern struct srcu_struct migrate_offload_srcu;
int migrate_offload_register(struct migrator *m);
int migrate_offload_unregister(struct migrator *m);
#else
static inline int migrate_offload_register(struct migrator *m) { return -EOPNOTSUPP; }
static inline int migrate_offload_unregister(struct migrator *m) { return -EOPNOTSUPP; }
#endif

#endif /* _LINUX_MIGRATE_COPY_OFFLOAD_H */
