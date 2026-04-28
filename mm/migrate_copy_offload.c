// SPDX-License-Identifier: GPL-2.0
#include <linux/jump_label.h>
#include <linux/module.h>
#include <linux/srcu.h>
#include <linux/migrate.h>
#include <linux/migrate_copy_offload.h>
#include <linux/static_call.h>

static DEFINE_MUTEX(migrator_mutex);
static struct migrator *active_migrator;

DECLARE_STATIC_CALL(migrate_offload_copy, folios_mc_copy);

/**
 * migrate_offload_register - register a batch-copy provider for page migration.
 * @m: migrator to install.
 *
 * Only one provider can be active at a time, returns -EBUSY if another migrator
 * is already registered.
 *
 * Return: 0 on success, negative errno on failure.
 */
int migrate_offload_register(struct migrator *m)
{
	int ret = 0;

	if (!m || !m->offload_copy || !m->owner)
		return -EINVAL;

	mutex_lock(&migrator_mutex);
	if (active_migrator) {
		ret = -EBUSY;
		goto unlock;
	}

	if (!try_module_get(m->owner)) {
		ret = -ENODEV;
		goto unlock;
	}

	static_call_update(migrate_offload_copy, m->offload_copy);
	active_migrator = m;
	static_branch_enable(&migrate_offload_enabled);

unlock:
	mutex_unlock(&migrator_mutex);

	if (ret)
		pr_err("migrate_offload: %s: failed to register (%d)\n",
		       m->name, ret);
	else
		pr_info("migrate_offload: enabled by %s\n", m->name);
	return ret;
}
EXPORT_SYMBOL_GPL(migrate_offload_register);

/**
 * migrate_offload_unregister - unregister the active batch-copy provider.
 * @m: migrator to remove (must be the currently active one).
 *
 * Reverts static_call targets and waits for SRCU grace period so that
 * no in-flight migration is still calling the driver functions before
 * releasing the module.
 *
 * Return: 0 on success, negative errno on failure.
 */
int migrate_offload_unregister(struct migrator *m)
{
	struct module *owner;

	mutex_lock(&migrator_mutex);
	if (active_migrator != m) {
		mutex_unlock(&migrator_mutex);
		return -EINVAL;
	}

	/*
	 * Disable the static branch first so new migrate_pages_batch calls
	 * won't enter the batch copy path.
	 */
	static_branch_disable(&migrate_offload_enabled);
	static_call_update(migrate_offload_copy, folios_mc_copy);
	owner = active_migrator->owner;
	active_migrator = NULL;
	mutex_unlock(&migrator_mutex);

	/* Wait for all in-flight callers to finish before module_put(). */
	synchronize_srcu(&migrate_offload_srcu);
	module_put(owner);

	pr_info("migrate_offload: disabled by %s\n", m->name);
	return 0;
}
EXPORT_SYMBOL_GPL(migrate_offload_unregister);
