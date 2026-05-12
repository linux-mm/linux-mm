// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include <linux/iomap.h>
#include <linux/swap.h>

static int iomap_swapfile_fail(struct file *file, const char *str)
{
	char *buf, *p = ERR_PTR(-ENOMEM);

	buf = kmalloc(PATH_MAX, GFP_KERNEL);
	if (buf)
		p = file_path(file, buf, PATH_MAX);
	pr_err("swapon: file %s %s\n", IS_ERR(p) ? "<unknown>" : p, str);
	kfree(buf);
	return -EINVAL;
}

/*
 * Report physical extents for this swap file.  Physical extents reported to the
 * swap code must be trimmed to align to a page boundary.  The logical offset
 * within the file is irrelevant since the swapfile code maps logical page
 * numbers of the swap device to the physical page-aligned extents.
 */
static int iomap_swapfile_iter(struct iomap_iter *iter, struct file *file,
		struct swap_info_struct *sis)
{
	struct iomap *iomap = &iter->iomap;
	uint64_t first_ppage;
	uint64_t next_ppage;
	int error;

	switch (iomap->type) {
	case IOMAP_MAPPED:
	case IOMAP_UNWRITTEN:
		/* Only real or unwritten extents. */
		break;
	case IOMAP_INLINE:
		/* No inline data. */
		return iomap_swapfile_fail(file, "is inline");
	default:
		return iomap_swapfile_fail(file, "has unallocated extents");
	}

	/* No uncommitted metadata or shared blocks. */
	if (iomap->flags & IOMAP_F_DIRTY)
		return iomap_swapfile_fail(file, "is not committed");
	if (iomap->flags & IOMAP_F_SHARED)
		return iomap_swapfile_fail(file, "has shared extents");

	/* Only one bdev per swap file. */
	if (iomap->bdev != sis->bdev)
		return iomap_swapfile_fail(file, "outside the main device");

	/*
	 * Round the start up and the end down so that the physical extent
	 * aligns to a page boundary.
	 */
	first_ppage = ALIGN(iomap->addr, PAGE_SIZE) >> PAGE_SHIFT;
	next_ppage = ALIGN_DOWN(iomap->addr + iomap->length, PAGE_SIZE) >>
			PAGE_SHIFT;
	error = add_swap_extent(sis, next_ppage - first_ppage, first_ppage);
	if (error)
		return error;
	return iomap_iter_advance_full(iter);
}

/*
 * Iterate a swap file's iomaps to construct physical extents that can be
 * passed to the swapfile subsystem.
 */
int iomap_swap_activate(struct file *file, struct swap_info_struct *sis,
		const struct iomap_ops *ops)
{
	struct inode *inode = file->f_mapping->host;
	struct iomap_iter iter = {
		.inode	= inode,
		.pos	= 0,
		.len	= ALIGN_DOWN(i_size_read(inode), PAGE_SIZE),
		.flags	= IOMAP_REPORT,
	};
	int ret;

	/*
	 * Persist all file mapping metadata so that we won't have any
	 * IOMAP_F_DIRTY iomaps.
	 */
	ret = vfs_fsync(file, 1);
	if (ret)
		return ret;

	while ((ret = iomap_iter(&iter, ops)) > 0)
		iter.status = iomap_swapfile_iter(&iter, file, sis);
	return ret;
}
EXPORT_SYMBOL_GPL(iomap_swap_activate);
