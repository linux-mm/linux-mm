/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DMA-BUF restricted heap exporter for NVIDIA Video-Protection-Region (VPR)
 *
 * Copyright (C) 2024-2026 NVIDIA Corporation
 */

#ifndef DMA_BUF_HEAPS_TEGRA_VPR_H
#define DMA_BUF_HEAPS_TEGRA_VPR_H

#include <linux/list.h>
#include <linux/mutex.h>

struct tegra_vpr_device {
	struct list_head node;
	struct device *dev;
};

struct tegra_vpr;

struct tegra_vpr_chunk {
	phys_addr_t start;
	phys_addr_t limit;
	size_t size;

	struct tegra_vpr *vpr;
	bool invalid;
	bool active;

	struct page *start_page;
	unsigned int offset;
	unsigned long virt;
	pgoff_t num_pages;

	unsigned int num_buffers;
};

struct tegra_vpr {
	struct list_head list;

	struct device_node *dev_node;
	unsigned long align;
	phys_addr_t base;
	phys_addr_t size;
	int nid;

	struct list_head buffers;
	unsigned long *bitmap;
	pgoff_t num_pages;

	/* resizable VPR */
	struct cma *cma;
	unsigned long *active;
	struct tegra_vpr_chunk *chunks;
	unsigned int num_chunks;
	struct page *start_page;
	bool resizable;

	unsigned int first;
	unsigned int last;

	struct list_head devices;

	/**
	 * @lock: Protects concurrent access to the allocation bitmap, as well
	 * as the buffers and devices lists.
	 */
	struct mutex lock;
};

void tegra_vpr_add(struct tegra_vpr *vpr);

#endif
