// SPDX-License-Identifier: GPL-2.0
/*
 * DMA-BUF restricted heap exporter for NVIDIA Video-Protection-Region (VPR)
 *
 * Copyright (C) 2024-2026 NVIDIA Corporation
 */

#define pr_fmt(fmt) "tegra-vpr: " fmt

#include "tegra-vpr.h"

#include <linux/cma.h>
#include <linux/of_reserved_mem.h>

static DEFINE_MUTEX(vpr_lock);
static LIST_HEAD_GUARDED(vpr_list, vpr_lock);

static int __init tegra_vpr_node_init(unsigned long offset,
				      struct reserved_mem *rmem)
{
	struct cma *cma;
	int err;

	if (!IS_ALIGNED(rmem->base, SZ_1M)) {
		pr_err("%s: base is not aligned to 1 MiB\n", rmem->name);
		return -EINVAL;
	}

	if (!IS_ALIGNED(rmem->size, SZ_1M)) {
		pr_err("%s: size is not aligned to 1 MiB\n", rmem->name);
		return -EINVAL;
	}

	err = cma_init_reserved_mem(rmem->base, rmem->size, 0, rmem->name,
				    &cma);
	if (err < 0) {
		pr_err("%s: failed to initialize CMA: %d\n", __func__, err);
		return err;
	}

	rmem->priv = cma;

	return 0;
}

static struct tegra_vpr *tegra_vpr_lookup(struct cma *cma)
{
	struct tegra_vpr *vpr;

	mutex_lock(&vpr_lock);

	list_for_each_entry(vpr, &vpr_list, list) {
		if (vpr->cma == cma) {
			mutex_unlock(&vpr_lock);
			return vpr;
		}
	}

	mutex_unlock(&vpr_lock);

	return ERR_PTR(-EPROBE_DEFER);
}

static int tegra_vpr_device_init(struct reserved_mem *rmem, struct device *dev)
{
	const struct dev_pm_ops *pm = dev->driver->pm;
	struct tegra_vpr_device *node;
	struct cma *cma = rmem->priv;
	struct tegra_vpr *vpr;

	vpr = tegra_vpr_lookup(cma);
	if (IS_ERR(vpr))
		return PTR_ERR(vpr);

	if (!pm || !pm->freeze || !pm->thaw)
		return -EINVAL;

	node = kzalloc_obj(*node, GFP_KERNEL);
	if (!node)
		return -ENOMEM;

	INIT_LIST_HEAD(&node->node);
	node->dev = dev;

	mutex_lock(&vpr->lock);
	list_add_tail(&node->node, &vpr->devices);
	mutex_unlock(&vpr->lock);

	return 0;
}

static void tegra_vpr_device_release(struct reserved_mem *rmem,
				     struct device *dev)
{
	struct tegra_vpr_device *node, *tmp;
	struct cma *cma = rmem->priv;
	struct tegra_vpr *vpr;

	vpr = tegra_vpr_lookup(cma);
	if (IS_ERR(vpr)) {
		dev_WARN(dev, "failed to find VPR for CMA '%s'\n",
			 cma_get_name(cma));
		return;
	}

	mutex_lock(&vpr->lock);

	list_for_each_entry_safe(node, tmp, &vpr->devices, node) {
		if (node->dev == dev) {
			list_del(&node->node);
			kfree(node);
		}
	}

	mutex_unlock(&vpr->lock);
}

static const struct reserved_mem_ops tegra_vpr_rmem_ops = {
	.node_init = tegra_vpr_node_init,
	.device_init = tegra_vpr_device_init,
	.device_release = tegra_vpr_device_release,
};

RESERVEDMEM_OF_DECLARE(tegra_vpr, "nvidia,tegra-video-protection-region",
		       &tegra_vpr_rmem_ops);

void tegra_vpr_add(struct tegra_vpr *vpr)
{
	mutex_lock(&vpr_lock);
	list_add_tail(&vpr->list, &vpr_list);
	mutex_unlock(&vpr_lock);
}
EXPORT_SYMBOL(tegra_vpr_add);
