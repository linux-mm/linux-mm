// SPDX-License-Identifier: GPL-2.0-only
/*
 * DMA Core Batch Migrator (DCBM)
 *
 * Uses DMAEngine memcpy channels to offload batch folio copies during
 * page migration. Reference driver meant for testing the offload
 * infrastructure.
 *
 * Copyright (C) 2024-26 Advanced Micro Devices, Inc.
 */

#include <linux/module.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/migrate.h>
#include <linux/migrate_copy_offload.h>

#define MAX_DMA_CHANNELS	16

static atomic_long_t folios_migrated;
static atomic_long_t folios_failures;

static bool offloading_enabled;
static unsigned int nr_dma_channels = 1;
static DEFINE_MUTEX(dcbm_mutex);

struct dma_work {
	struct dma_chan *chan;
	struct completion done;
	atomic_t pending;
	struct sg_table *src_sgt;
	struct sg_table *dst_sgt;
	bool mapped;
};

static void dma_completion_callback(void *data)
{
	struct dma_work *work = data;

	if (atomic_dec_and_test(&work->pending))
		complete(&work->done);
}

static int setup_sg_tables(struct dma_work *work, struct list_head **src_pos,
		struct list_head **dst_pos, int nr)
{
	struct scatterlist *sg_src, *sg_dst;
	struct device *dev;
	int i, ret;

	work->src_sgt = kmalloc_obj(*work->src_sgt, GFP_KERNEL);
	if (!work->src_sgt)
		return -ENOMEM;
	work->dst_sgt = kmalloc_obj(*work->dst_sgt, GFP_KERNEL);
	if (!work->dst_sgt) {
		ret = -ENOMEM;
		goto err_free_src;
	}

	ret = sg_alloc_table(work->src_sgt, nr, GFP_KERNEL);
	if (ret)
		goto err_free_dst;
	ret = sg_alloc_table(work->dst_sgt, nr, GFP_KERNEL);
	if (ret)
		goto err_free_src_table;

	sg_src = work->src_sgt->sgl;
	sg_dst = work->dst_sgt->sgl;
	for (i = 0; i < nr; i++) {
		struct folio *src = list_entry(*src_pos, struct folio, lru);
		struct folio *dst = list_entry(*dst_pos, struct folio, lru);

		sg_set_folio(sg_src, src, folio_size(src), 0);
		sg_set_folio(sg_dst, dst, folio_size(dst), 0);

		*src_pos = (*src_pos)->next;
		*dst_pos = (*dst_pos)->next;

		if (i < nr - 1) {
			sg_src = sg_next(sg_src);
			sg_dst = sg_next(sg_dst);
		}
	}

	dev = dmaengine_get_dma_device(work->chan);
	if (!dev) {
		ret = -ENODEV;
		goto err_free_dst_table;
	}
	ret = dma_map_sgtable(dev, work->src_sgt, DMA_TO_DEVICE,
			DMA_ATTR_SKIP_CPU_SYNC | DMA_ATTR_NO_KERNEL_MAPPING);
	if (ret)
		goto err_free_dst_table;
	ret = dma_map_sgtable(dev, work->dst_sgt, DMA_FROM_DEVICE,
			DMA_ATTR_SKIP_CPU_SYNC | DMA_ATTR_NO_KERNEL_MAPPING);
	if (ret)
		goto err_unmap_src;

	/*
	 * TODO: IOMMU may merge segments unevenly on the two sides, fall back
	 * bail to CPU copy. In practice, I have not observed merging in tests.
	 * Handling unequal nents is left for follow-up.
	 */
	if (work->src_sgt->nents != work->dst_sgt->nents) {
		ret = -EINVAL;
		goto err_unmap_dst;
	}
	work->mapped = true;
	return 0;

err_unmap_dst:
	dma_unmap_sgtable(dev, work->dst_sgt, DMA_FROM_DEVICE,
			DMA_ATTR_SKIP_CPU_SYNC | DMA_ATTR_NO_KERNEL_MAPPING);
err_unmap_src:
	dma_unmap_sgtable(dev, work->src_sgt, DMA_TO_DEVICE,
			DMA_ATTR_SKIP_CPU_SYNC | DMA_ATTR_NO_KERNEL_MAPPING);
err_free_dst_table:
	sg_free_table(work->dst_sgt);
err_free_src_table:
	sg_free_table(work->src_sgt);
err_free_dst:
	kfree(work->dst_sgt);
	work->dst_sgt = NULL;
err_free_src:
	kfree(work->src_sgt);
	work->src_sgt = NULL;
	return ret;
}

static void cleanup_dma_work(struct dma_work *works, int actual_channels)
{
	struct device *dev;
	int i;

	if (!works)
		return;

	for (i = 0; i < actual_channels; i++) {
		if (!works[i].chan)
			continue;

		dev = dmaengine_get_dma_device(works[i].chan);

		if (works[i].mapped)
			dmaengine_terminate_sync(works[i].chan);

		if (dev && works[i].mapped) {
			if (works[i].src_sgt) {
				dma_unmap_sgtable(dev, works[i].src_sgt,
						DMA_TO_DEVICE,
						DMA_ATTR_SKIP_CPU_SYNC |
						DMA_ATTR_NO_KERNEL_MAPPING);
				sg_free_table(works[i].src_sgt);
				kfree(works[i].src_sgt);
			}
			if (works[i].dst_sgt) {
				dma_unmap_sgtable(dev, works[i].dst_sgt,
						DMA_FROM_DEVICE,
						DMA_ATTR_SKIP_CPU_SYNC |
						DMA_ATTR_NO_KERNEL_MAPPING);
				sg_free_table(works[i].dst_sgt);
				kfree(works[i].dst_sgt);
			}
		}
		dma_release_channel(works[i].chan);
	}
	kfree(works);
}

static int submit_dma_transfers(struct dma_work *work)
{
	struct scatterlist *sg_src, *sg_dst;
	struct dma_async_tx_descriptor *tx;
	unsigned long flags = DMA_CTRL_ACK;
	dma_cookie_t cookie;
	int i;

	atomic_set(&work->pending, 1);

	sg_src = work->src_sgt->sgl;
	sg_dst = work->dst_sgt->sgl;
	for_each_sgtable_dma_sg(work->src_sgt, sg_src, i) {
		if (i == work->src_sgt->nents - 1)
			flags |= DMA_PREP_INTERRUPT;

		tx = dmaengine_prep_dma_memcpy(work->chan,
				sg_dma_address(sg_dst),
				sg_dma_address(sg_src),
				sg_dma_len(sg_src), flags);
		if (!tx) {
			atomic_set(&work->pending, 0);
			return -EIO;
		}

		if (i == work->src_sgt->nents - 1) {
			tx->callback = dma_completion_callback;
			tx->callback_param = work;
		}

		cookie = dmaengine_submit(tx);
		if (dma_submit_error(cookie)) {
			atomic_set(&work->pending, 0);
			return -EIO;
		}
		sg_dst = sg_next(sg_dst);
	}
	return 0;
}

/**
 * folios_copy_dma - copy a batch of folios via DMA memcpy
 * @dst_list: destination folio list
 * @src_list: source folio list
 * @nr_folios: number of folios in each list
 *
 * Return: 0 on success, negative errno on failure.
 */
static int folios_copy_dma(struct list_head *dst_list,
		struct list_head *src_list, unsigned int nr_folios)
{
	struct dma_work *works;
	struct list_head *src_pos = src_list->next;
	struct list_head *dst_pos = dst_list->next;
	int i, folios_per_chan, ret;
	dma_cap_mask_t mask;
	int actual_channels = 0;
	unsigned int max_channels;

	max_channels = min3(nr_dma_channels, nr_folios,
			(unsigned int)MAX_DMA_CHANNELS);

	works = kcalloc(max_channels, sizeof(*works), GFP_KERNEL);
	if (!works)
		return -ENOMEM;

	dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY, mask);

	for (i = 0; i < max_channels; i++) {
		works[actual_channels].chan = dma_request_chan_by_mask(&mask);
		if (IS_ERR(works[actual_channels].chan))
			break;
		init_completion(&works[actual_channels].done);
		actual_channels++;
	}

	if (actual_channels == 0) {
		kfree(works);
		return -ENODEV;
	}

	for (i = 0; i < actual_channels; i++) {
		folios_per_chan = nr_folios * (i + 1) / actual_channels -
				(nr_folios * i) / actual_channels;
		if (folios_per_chan == 0)
			continue;

		ret = setup_sg_tables(&works[i], &src_pos, &dst_pos,
				folios_per_chan);
		if (ret)
			goto err_cleanup;
	}

	for (i = 0; i < actual_channels; i++) {
		ret = submit_dma_transfers(&works[i]);
		if (ret)
			goto err_cleanup;
	}

	for (i = 0; i < actual_channels; i++) {
		if (atomic_read(&works[i].pending) > 0)
			dma_async_issue_pending(works[i].chan);
	}

	for (i = 0; i < actual_channels; i++) {
		if (atomic_read(&works[i].pending) == 0)
			continue;
		if (!wait_for_completion_timeout(&works[i].done,
				msecs_to_jiffies(10000))) {
			ret = -ETIMEDOUT;
			goto err_cleanup;
		}
	}

	cleanup_dma_work(works, actual_channels);

	atomic_long_add(nr_folios, &folios_migrated);
	return 0;

err_cleanup:
	pr_warn_ratelimited("dcbm: DMA copy failed (%d), falling back to CPU\n",
			ret);
	cleanup_dma_work(works, actual_channels);

	atomic_long_add(nr_folios, &folios_failures);
	return ret;
}

static struct migrator dma_migrator = {
	.name = "DCBM",
	.offload_copy = folios_copy_dma,
	.owner = THIS_MODULE,
};

static ssize_t offloading_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", offloading_enabled);
}

static ssize_t offloading_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret)
		return ret;

	mutex_lock(&dcbm_mutex);

	if (enable == offloading_enabled)
		goto out;

	if (enable) {
		ret = migrate_offload_register(&dma_migrator);
		if (ret) {
			mutex_unlock(&dcbm_mutex);
			return ret;
		}
		offloading_enabled = true;
	} else {
		migrate_offload_unregister(&dma_migrator);
		offloading_enabled = false;
	}
out:
	mutex_unlock(&dcbm_mutex);
	return count;
}

static ssize_t folios_migrated_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%lu\n", atomic_long_read(&folios_migrated));
}

static ssize_t folios_migrated_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	atomic_long_set(&folios_migrated, 0);
	return count;
}

static ssize_t folios_failures_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%lu\n", atomic_long_read(&folios_failures));
}

static ssize_t folios_failures_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	atomic_long_set(&folios_failures, 0);
	return count;
}

static ssize_t nr_dma_chan_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", nr_dma_channels);
}

static ssize_t nr_dma_chan_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret)
		return ret;

	if (val < 1 || val > MAX_DMA_CHANNELS)
		return -EINVAL;

	mutex_lock(&dcbm_mutex);
	nr_dma_channels = val;
	mutex_unlock(&dcbm_mutex);
	return count;
}

static struct kobj_attribute offloading_attr = __ATTR_RW(offloading);
static struct kobj_attribute nr_dma_chan_attr = __ATTR_RW(nr_dma_chan);
static struct kobj_attribute folios_migrated_attr = __ATTR_RW(folios_migrated);
static struct kobj_attribute folios_failures_attr = __ATTR_RW(folios_failures);

static struct attribute *dcbm_attrs[] = {
	&offloading_attr.attr,
	&nr_dma_chan_attr.attr,
	&folios_migrated_attr.attr,
	&folios_failures_attr.attr,
	NULL
};

static const struct attribute_group dcbm_attr_group = {
	.attrs = dcbm_attrs,
};

static int __init dcbm_init(void)
{
	int ret;

	ret = sysfs_create_group(&THIS_MODULE->mkobj.kobj, &dcbm_attr_group);
	if (ret)
		return ret;

	pr_info("dcbm: DMA Core Batch Migrator initialized\n");
	return 0;
}

static void __exit dcbm_exit(void)
{
	mutex_lock(&dcbm_mutex);
	if (offloading_enabled) {
		migrate_offload_unregister(&dma_migrator);
		offloading_enabled = false;
	}
	mutex_unlock(&dcbm_mutex);

	sysfs_remove_group(&THIS_MODULE->mkobj.kobj, &dcbm_attr_group);
	pr_info("dcbm: DMA Core Batch Migrator unloaded\n");
}

module_init(dcbm_init);
module_exit(dcbm_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Shivank Garg");
MODULE_DESCRIPTION("DMA Core Batch Migrator");
