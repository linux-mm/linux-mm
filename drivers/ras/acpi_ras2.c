// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ACPI RAS2 memory driver
 *
 * Copyright (c) 2024-2026 HiSilicon Limited.
 *
 */

#undef pr_fmt
#define pr_fmt(fmt)	"ACPI RAS2 MEMORY: " fmt

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/edac.h>
#include <linux/kthread.h>
#include <linux/platform_device.h>
#include <acpi/ras2.h>

#define RAS2_SUPPORT_HW_PARTOL_SCRUB BIT(0)
#define RAS2_TYPE_PATROL_SCRUB 0x0000

#define RAS2_GET_PATROL_PARAMETERS 0x01
#define RAS2_START_PATROL_SCRUBBER 0x02
#define RAS2_STOP_PATROL_SCRUBBER 0x03

/*
 * RAS2 patrol scrub
 */
#define RAS2_PS_SC_HRS_IN_MASK GENMASK(15, 8)
#define RAS2_PS_EN_BACKGROUND BIT(0)
#define RAS2_PS_SC_HRS_OUT_MASK GENMASK(7, 0)
#define RAS2_PS_MIN_SC_HRS_OUT_MASK GENMASK(15, 8)
#define RAS2_PS_MAX_SC_HRS_OUT_MASK GENMASK(23, 16)
#define RAS2_PS_FLAG_SCRUB_RUNNING BIT(0)

#define RAS2_SCRUB_NAME_LEN 128
#define RAS2_HOUR_IN_SECS 3600

struct acpi_ras2_ps_shared_mem {
	struct acpi_ras2_shmem common;
	struct acpi_ras2_patrol_scrub_param params;
};

#define TO_ACPI_RAS2_PS_SHMEM(_addr) \
	container_of(_addr, struct acpi_ras2_ps_shared_mem, common)

static int __ras2_hw_scrub_set_enabled_bg(struct device *dev, void *drv_data, bool enable);

static int ras2_is_patrol_scrub_support(struct ras2_mem_ctx *ras2_ctx)
{
	struct acpi_ras2_shmem __iomem *common = (void *)ras2_ctx->comm_addr;

	guard(mutex)(ras2_ctx->pcc_lock);
	iowrite8(0, &common->set_caps[0]);

	return ioread8(&common->features[0]) & RAS2_SUPPORT_HW_PARTOL_SCRUB;
}

static int ras2_update_patrol_scrub_params_cache(struct ras2_mem_ctx *ras2_ctx)
{
	struct acpi_ras2_ps_shared_mem __iomem *ps_sm =
		TO_ACPI_RAS2_PS_SHMEM(ras2_ctx->comm_addr);
	u32 scrub_params_out;
	int ret;

	iowrite8(RAS2_SUPPORT_HW_PARTOL_SCRUB, &ps_sm->common.set_caps[0]);
	iowrite16(RAS2_GET_PATROL_PARAMETERS, &ps_sm->params.command);
	iowrite64(ras2_ctx->mem_base, &ps_sm->params.req_addr_range[0]);
	iowrite64(ras2_ctx->mem_size, &ps_sm->params.req_addr_range[1]);
	ret = ras2_send_pcc_cmd(ras2_ctx, PCC_CMD_EXEC_RAS2);
	if (ret) {
		dev_err(ras2_ctx->dev, "Failed to read patrol scrub parameters\n");
		return ret;
	}

	scrub_params_out = ioread32(&ps_sm->params.scrub_params_out);
	ras2_ctx->min_scrub_cycle = FIELD_GET(RAS2_PS_MIN_SC_HRS_OUT_MASK,
					      scrub_params_out);
	ras2_ctx->max_scrub_cycle = FIELD_GET(RAS2_PS_MAX_SC_HRS_OUT_MASK,
					      scrub_params_out);
	ras2_ctx->scrub_cycle_hrs = FIELD_GET(RAS2_PS_SC_HRS_OUT_MASK,
					      scrub_params_out);
	if (ras2_ctx->bg_scrub) {
		ras2_ctx->od_scrub = false;
		ras2_ctx->base = 0;
		ras2_ctx->size = 0;
		return 0;
	}

	if  (ioread32(&ps_sm->params.flags) & RAS2_PS_FLAG_SCRUB_RUNNING) {
		ras2_ctx->od_scrub = true;
		ras2_ctx->base = ioread64(&ps_sm->params.actl_addr_range[0]);
		ras2_ctx->size = ioread64(&ps_sm->params.actl_addr_range[1]);
	} else {
		ras2_ctx->od_scrub = false;
	}

	return 0;
}

/* Context - PCC lock must be held */
static int ras2_get_demand_scrub_running(struct ras2_mem_ctx *ras2_ctx, bool *running)
{
	struct acpi_ras2_ps_shared_mem __iomem *ps_sm =
		TO_ACPI_RAS2_PS_SHMEM(ras2_ctx->comm_addr);
	int ret;

	if (!ras2_ctx->od_scrub) {
		*running = false;
		return 0;
	}

	iowrite8(RAS2_SUPPORT_HW_PARTOL_SCRUB, &ps_sm->common.set_caps[0]);
	iowrite16(RAS2_GET_PATROL_PARAMETERS, &ps_sm->params.command);
	iowrite64(ras2_ctx->mem_base, &ps_sm->params.req_addr_range[0]);
	iowrite64(ras2_ctx->mem_size, &ps_sm->params.req_addr_range[1]);

	ret = ras2_send_pcc_cmd(ras2_ctx, PCC_CMD_EXEC_RAS2);
	if (ret) {
		dev_err(ras2_ctx->dev, "Failed to read patrol scrub parameters\n");
		return ret;
	}

	*running = ioread32(&ps_sm->params.flags) & RAS2_PS_FLAG_SCRUB_RUNNING;
	if (!(*running))
		ras2_ctx->od_scrub = false;

	return 0;
}

static int ras2_scrub_monitor_thread(void *p)
{
	struct ras2_mem_ctx *ras2_ctx = (struct ras2_mem_ctx *)p;
	bool running;
	int ret;

	while (!kthread_should_stop()) {
		if (!ras2_ctx->reenable_bg_scrub)
			break;

		/*
		 * If ras2_get_demand_scrub_running() fails here, re-enabling background
		 * scrubbing immediately may not be possible or correct. In that case,
		 * the admin or firmware may need to re-enable background scrubbing
		 * after demand scrubbing has finished.
		 */
		mutex_lock(ras2_ctx->pcc_lock);
		ret = ras2_get_demand_scrub_running(ras2_ctx, &running);
		if (ret) {
			mutex_unlock(ras2_ctx->pcc_lock);
			break;
		}

		if (!running) {
			ret = __ras2_hw_scrub_set_enabled_bg(ras2_ctx->dev, ras2_ctx, true);
			if (ret)
				dev_err(ras2_ctx->dev,
					"Failed to enable background scrub ret=%d\n", ret);

			mutex_unlock(ras2_ctx->pcc_lock);
			break;
		}

		mutex_unlock(ras2_ctx->pcc_lock);
		msleep(1000);
	}

	mutex_lock(ras2_ctx->pcc_lock);
	ras2_ctx->thread = NULL;
	mutex_unlock(ras2_ctx->pcc_lock);

	return 0;
}

static int ras2_hw_scrub_read_min_scrub_cycle(struct device *dev, void *drv_data, u32 *min)
{
	struct ras2_mem_ctx *ras2_ctx = drv_data;

	guard(mutex)(ras2_ctx->pcc_lock);
	*min = ras2_ctx->min_scrub_cycle * RAS2_HOUR_IN_SECS;

	return 0;
}

static int ras2_hw_scrub_read_max_scrub_cycle(struct device *dev, void *drv_data, u32 *max)
{
	struct ras2_mem_ctx *ras2_ctx = drv_data;

	guard(mutex)(ras2_ctx->pcc_lock);
	*max = ras2_ctx->max_scrub_cycle * RAS2_HOUR_IN_SECS;

	return 0;
}

static int ras2_hw_scrub_cycle_read(struct device *dev, void *drv_data, u32 *scrub_cycle_secs)
{
	struct ras2_mem_ctx *ras2_ctx = drv_data;

	guard(mutex)(ras2_ctx->pcc_lock);
	*scrub_cycle_secs = ras2_ctx->scrub_cycle_hrs * RAS2_HOUR_IN_SECS;

	return 0;
}

static int ras2_hw_scrub_cycle_write(struct device *dev, void *drv_data, u32 scrub_cycle_secs)
{
	u32 scrub_cycle_hrs = scrub_cycle_secs / RAS2_HOUR_IN_SECS;
	struct ras2_mem_ctx *ras2_ctx = drv_data;
	bool running;
	int ret;

	guard(mutex)(ras2_ctx->pcc_lock);
	ret = ras2_get_demand_scrub_running(ras2_ctx, &running);
	if (ret)
		return ret;

	if (running)
		return -EBUSY;

	if (scrub_cycle_hrs < ras2_ctx->min_scrub_cycle ||
	    scrub_cycle_hrs > ras2_ctx->max_scrub_cycle)
		return -EINVAL;

	ras2_ctx->set_scrub_cycle = scrub_cycle_hrs;

	return 0;
}

static int ras2_hw_scrub_read_addr(struct device *dev, void *drv_data, u64 *base)
{
	struct ras2_mem_ctx *ras2_ctx = drv_data;
	int ret;

	guard(mutex)(ras2_ctx->pcc_lock);
	/*
	 * When BG scrubbing is enabled the actual address range is not valid.
	 * Return -EBUSY now unless find out a method to retrieve actual full PA range.
	 */
	if (ras2_ctx->bg_scrub)
		return -EBUSY;

	ret = ras2_update_patrol_scrub_params_cache(ras2_ctx);
	if (ret)
		return ret;

	*base = ras2_ctx->base;

	return 0;
}

static int ras2_hw_scrub_read_size(struct device *dev, void *drv_data, u64 *size)
{
	struct ras2_mem_ctx *ras2_ctx = drv_data;
	int ret;

	guard(mutex)(ras2_ctx->pcc_lock);
	if (ras2_ctx->bg_scrub)
		return -EBUSY;

	ret = ras2_update_patrol_scrub_params_cache(ras2_ctx);
	if (ret)
		return ret;

	*size = ras2_ctx->size;

	return 0;
}

static int ras2_hw_scrub_write_addr(struct device *dev, void *drv_data, u64 base)
{
	struct ras2_mem_ctx *ras2_ctx = drv_data;
	bool running;
	int ret;

	guard(mutex)(ras2_ctx->pcc_lock);
	ret = ras2_get_demand_scrub_running(ras2_ctx, &running);
	if (ret)
		return ret;

	if (running)
		return -EBUSY;

	ras2_ctx->base = base;

	return 0;
}

static int ras2_hw_scrub_write_size(struct device *dev, void *drv_data, u64 size)
{
	struct ras2_mem_ctx *ras2_ctx = drv_data;
	bool running;
	int ret;

	if (!size)
		return -EINVAL;

	guard(mutex)(ras2_ctx->pcc_lock);
	ret = ras2_get_demand_scrub_running(ras2_ctx, &running);
	if (ret)
		return ret;

	if (running)
		return -EBUSY;

	ras2_ctx->size = size;

	return 0;
}

static int ras2_hw_scrub_get_enabled_bg(struct device *dev, void *drv_data, bool *enabled)
{
	struct ras2_mem_ctx *ras2_ctx = drv_data;

	guard(mutex)(ras2_ctx->pcc_lock);
	*enabled = ras2_ctx->bg_scrub;

	return 0;
}

static int __ras2_hw_scrub_set_enabled_bg(struct device *dev, void *drv_data, bool enable)
{
	struct ras2_mem_ctx *ras2_ctx = drv_data;
	struct acpi_ras2_ps_shared_mem __iomem *ps_sm = TO_ACPI_RAS2_PS_SHMEM(ras2_ctx->comm_addr);
	u32 scrub_params_in;
	bool running;
	int ret;

	ret = ras2_get_demand_scrub_running(ras2_ctx, &running);
	if (ret)
		return ret;

	iowrite8(RAS2_SUPPORT_HW_PARTOL_SCRUB, &ps_sm->common.set_caps[0]);
	if (enable) {
		if (ras2_ctx->bg_scrub || running)
			return -EBUSY;

		iowrite64(0, &ps_sm->params.req_addr_range[0]);
		iowrite64(0, &ps_sm->params.req_addr_range[1]);
		scrub_params_in = ioread32(&ps_sm->params.scrub_params_in);
		scrub_params_in &= ~RAS2_PS_SC_HRS_IN_MASK;
		scrub_params_in |= FIELD_PREP(RAS2_PS_SC_HRS_IN_MASK, ras2_ctx->set_scrub_cycle);
		iowrite32(scrub_params_in, &ps_sm->params.scrub_params_in);
		iowrite16(RAS2_START_PATROL_SCRUBBER, &ps_sm->params.command);
	} else {
		if (!ras2_ctx->bg_scrub)
			return -EPERM;

		iowrite16(RAS2_STOP_PATROL_SCRUBBER, &ps_sm->params.command);
	}

	scrub_params_in = ioread32(&ps_sm->params.scrub_params_in);
	scrub_params_in &= ~RAS2_PS_EN_BACKGROUND;
	scrub_params_in |= FIELD_PREP(RAS2_PS_EN_BACKGROUND, enable);
	iowrite32(scrub_params_in, &ps_sm->params.scrub_params_in);
	ret = ras2_send_pcc_cmd(ras2_ctx, PCC_CMD_EXEC_RAS2);
	if (ret) {
		dev_err(dev, "Failed to %s background scrubbing\n",
			str_enable_disable(enable));
		return ret;
	}

	ras2_ctx->bg_scrub = enable;
	if (enable) {
		ras2_ctx->reenable_bg_scrub = false;
		/* Update the cache to account for rounding of supplied parameters and similar */
		return ras2_update_patrol_scrub_params_cache(ras2_ctx);
	}

	return 0;
}

static int ras2_hw_scrub_set_enabled_bg(struct device *dev, void *drv_data, bool enable)
{
	struct ras2_mem_ctx *ras2_ctx = drv_data;

	guard(mutex)(ras2_ctx->pcc_lock);

	return __ras2_hw_scrub_set_enabled_bg(dev, drv_data, enable);
}

static int ras2_hw_scrub_get_enabled_od(struct device *dev, void *drv_data, bool *enabled)
{
	struct ras2_mem_ctx *ras2_ctx = drv_data;
	bool running;
	int ret;

	guard(mutex)(ras2_ctx->pcc_lock);
	ret = ras2_get_demand_scrub_running(ras2_ctx, &running);
	if (ret)
		return ret;

	*enabled = running;

	return 0;
}

static int ras2_hw_scrub_set_enabled_od(struct device *dev, void *drv_data, bool enable)
{
	struct ras2_mem_ctx *ras2_ctx = drv_data;
	struct acpi_ras2_ps_shared_mem __iomem *ps_sm = TO_ACPI_RAS2_PS_SHMEM(ras2_ctx->comm_addr);
	u32 scrub_params_in;
	bool running;
	int ret;

	if (!enable)
		return -EOPNOTSUPP;

	guard(mutex)(ras2_ctx->pcc_lock);
	ret = ras2_get_demand_scrub_running(ras2_ctx, &running);
	if (ret)
		return ret;
	if (running)
		return -EBUSY;

	/* Stop any background scrub currently in progress */
	if (ras2_ctx->bg_scrub) {
		ret = __ras2_hw_scrub_set_enabled_bg(dev, drv_data, false);
		if (ret)
			return ret;

		ras2_ctx->reenable_bg_scrub = true;
	}

	/*
	 * The validity checks for the address range to scrub need to be updated
	 * with (base + size) > (mem_base + mem_size) check in the future once a
	 * proper method for determining the NUMA node memory range is available.
	 */
	if (!ras2_ctx->size || ras2_ctx->base < ras2_ctx->mem_base) {
		dev_err(dev, "%s: Invalid address range, base=0x%llx size=0x%llx\n",
			__func__, ras2_ctx->base, ras2_ctx->size);
		ret = -ERANGE;
		goto enable_bg_scrub;
	}

	iowrite8(RAS2_SUPPORT_HW_PARTOL_SCRUB, &ps_sm->common.set_caps[0]);
	scrub_params_in = ioread32(&ps_sm->params.scrub_params_in);
	scrub_params_in &= ~RAS2_PS_SC_HRS_IN_MASK;
	scrub_params_in |= FIELD_PREP(RAS2_PS_SC_HRS_IN_MASK, ras2_ctx->set_scrub_cycle);
	scrub_params_in &= ~RAS2_PS_EN_BACKGROUND;
	iowrite32(scrub_params_in, &ps_sm->params.scrub_params_in);
	iowrite64(ras2_ctx->base, &ps_sm->params.req_addr_range[0]);
	iowrite64(ras2_ctx->size, &ps_sm->params.req_addr_range[1]);
	iowrite16(RAS2_START_PATROL_SCRUBBER, &ps_sm->params.command);

	ret = ras2_send_pcc_cmd(ras2_ctx, PCC_CMD_EXEC_RAS2);
	if (ret) {
		dev_err(dev, "Failed to start demand scrubbing rc(%d)\n", ret);
		if (ret != -EBUSY) {
			iowrite64(0, &ps_sm->params.req_addr_range[0]);
			iowrite64(0, &ps_sm->params.req_addr_range[1]);
			ras2_ctx->od_scrub = false;
			ras2_ctx->base = 0;
			ras2_ctx->size = 0;
		}
		goto enable_bg_scrub;
	}

	ras2_ctx->od_scrub = enable;

	ret = ras2_update_patrol_scrub_params_cache(ras2_ctx);

	if (ras2_ctx->reenable_bg_scrub && ras2_ctx->driver_active && !ras2_ctx->thread) {
		ras2_ctx->thread = kthread_run(ras2_scrub_monitor_thread, ras2_ctx,
					       "ras2_scrub_nid%d", ras2_ctx->sys_comp_nid);
		if (IS_ERR(ras2_ctx->thread)) {
			ret = PTR_ERR(ras2_ctx->thread);
			ras2_ctx->thread = NULL;
			/*
			 * If kthread_run() fails and the demand scrubbing has started running,
			 * re-enabling background scrub will fail. Thus admin/firmware may need
			 * to re-enable background scrub after demand scrubbing has finished.
			 */
			goto enable_bg_scrub;
		}
	}

	return ret;

enable_bg_scrub:
	if (ras2_ctx->reenable_bg_scrub) {
		ras2_ctx->reenable_bg_scrub = false;
		__ras2_hw_scrub_set_enabled_bg(dev, drv_data, true);
	}

	return ret;
}

static const struct edac_scrub_ops ras2_scrub_ops = {
	.read_addr = ras2_hw_scrub_read_addr,
	.read_size = ras2_hw_scrub_read_size,
	.write_addr = ras2_hw_scrub_write_addr,
	.write_size = ras2_hw_scrub_write_size,
	.get_enabled_bg = ras2_hw_scrub_get_enabled_bg,
	.set_enabled_bg = ras2_hw_scrub_set_enabled_bg,
	.get_enabled_od = ras2_hw_scrub_get_enabled_od,
	.set_enabled_od = ras2_hw_scrub_set_enabled_od,
	.get_min_cycle = ras2_hw_scrub_read_min_scrub_cycle,
	.get_max_cycle = ras2_hw_scrub_read_max_scrub_cycle,
	.get_cycle_duration = ras2_hw_scrub_cycle_read,
	.set_cycle_duration = ras2_hw_scrub_cycle_write,
};

static void ras2_mem_drv_remove(struct auxiliary_device *auxdev)
{
	struct ras2_mem_ctx *ras2_ctx = container_of(auxdev, struct ras2_mem_ctx, adev);

	if (!ras2_ctx)
		return;

	mutex_lock(ras2_ctx->pcc_lock);
	ras2_ctx->driver_active = false;
	if (ras2_ctx->thread) {
		mutex_unlock(ras2_ctx->pcc_lock);
		kthread_stop(ras2_ctx->thread);
		ras2_ctx->thread = NULL;
		return;
	}
	mutex_unlock(ras2_ctx->pcc_lock);
}

static int ras2_mem_drv_probe(struct auxiliary_device *auxdev, const struct auxiliary_device_id *id)
{
	struct ras2_mem_ctx *ras2_ctx = container_of(auxdev, struct ras2_mem_ctx, adev);
	struct edac_dev_feature ras_features;
	char scrub_name[RAS2_SCRUB_NAME_LEN];
	unsigned long start_pfn, num_spanned_pages;
	int ret;

	if (!ras2_is_patrol_scrub_support(ras2_ctx))
		return -EOPNOTSUPP;

	/*
	 * Retrieve the PA range of the NUMA domain and use it as the
	 * 'Requested Address Range', when send RAS2 command
	 * GET_PATROL_PARAMETERS to get parameters that apply to all addresses
	 * in the NUMA domain as well as when send command START_PATROL_SCRUBBER
	 * to start the demand scrubbing.
	 */
	start_pfn = node_start_pfn(ras2_ctx->sys_comp_nid);
	num_spanned_pages = node_spanned_pages(ras2_ctx->sys_comp_nid);
	if (!num_spanned_pages) {
		pr_debug("Failed to find PA range of NUMA node(%u)\n", ras2_ctx->sys_comp_nid);
		return -EPERM;
	}

	ras2_ctx->mem_base = __pfn_to_phys(start_pfn);
	ras2_ctx->mem_size = (u64)num_spanned_pages * PAGE_SIZE;
	guard(mutex)(ras2_ctx->pcc_lock);
	ret = ras2_update_patrol_scrub_params_cache(ras2_ctx);
	if (ret)
		return ret;

	/* Initialize set_scrub_cycle */
	if (ras2_ctx->scrub_cycle_hrs > ras2_ctx->min_scrub_cycle)
		ras2_ctx->set_scrub_cycle = ras2_ctx->scrub_cycle_hrs;
	else
		ras2_ctx->set_scrub_cycle = ras2_ctx->min_scrub_cycle;

	sprintf(scrub_name, "acpi_ras_mem%d", auxdev->id);

	ras_features.ft_type	= RAS_FEAT_SCRUB;
	ras_features.instance	= 0;
	ras_features.scrub_ops	= &ras2_scrub_ops;
	ras_features.ctx	= ras2_ctx;

	ras2_ctx->driver_active = true;

	return edac_dev_register(&auxdev->dev, scrub_name, NULL, 1, &ras_features);
}

static const struct auxiliary_device_id ras2_mem_dev_id_table[] = {
	{ .name = RAS2_AUX_DEV_NAME "." RAS2_MEM_DEV_ID_NAME, },
	{ }
};

MODULE_DEVICE_TABLE(auxiliary, ras2_mem_dev_id_table);

static struct auxiliary_driver ras2_mem_driver = {
	.name = RAS2_MEM_DEV_ID_NAME,
	.probe = ras2_mem_drv_probe,
	.remove = ras2_mem_drv_remove,
	.id_table = ras2_mem_dev_id_table,
};
module_auxiliary_driver(ras2_mem_driver);

MODULE_IMPORT_NS("ACPI_RAS2");
MODULE_DESCRIPTION("ACPI RAS2 memory driver");
MODULE_LICENSE("GPL");
