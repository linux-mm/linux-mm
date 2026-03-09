/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MIGRATE_COPY_OFFLOAD_H
#define _LINUX_MIGRATE_COPY_OFFLOAD_H

#include <linux/jump_label.h>
#include <linux/srcu.h>
#include <linux/types.h>

struct list_head;
struct module;

#define MIGRATOR_NAME_LEN 32

struct migrator {
	char name[MIGRATOR_NAME_LEN];
	int (*offload_copy)(struct list_head *dst_list,
			    struct list_head *src_list,
			    unsigned int folio_cnt);
	bool (*should_batch)(int reason);
	struct module *owner;
};

#ifdef CONFIG_MIGRATION_COPY_OFFLOAD
extern struct static_key_false migrate_offload_enabled;
extern struct srcu_struct migrate_offload_srcu;
bool migrate_should_batch_default(int reason);
int migrate_offload_start(struct migrator *m);
int migrate_offload_stop(struct migrator *m);
#else
static inline int migrate_offload_start(struct migrator *m) { return 0; }
static inline int migrate_offload_stop(struct migrator *m) { return 0; }
#endif /* CONFIG_MIGRATION_COPY_OFFLOAD */

#endif /* _LINUX_MIGRATE_COPY_OFFLOAD_H */
