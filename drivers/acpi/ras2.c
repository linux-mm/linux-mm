// SPDX-License-Identifier: GPL-2.0-only
/*
 * ACPI RAS2 feature table driver.
 *
 * Copyright (c) 2024-2026 HiSilicon Limited.
 *
 * Support for RAS2 table - ACPI 6.5 Specification, section 5.2.21, which
 * provides interfaces for platform RAS features, e.g., for HW-based memory
 * scrubbing, and logical to physical address translation service. RAS2 uses
 * PCC channel subspace for communicating with the ACPI compliant HW platform.
 */

#undef pr_fmt
#define pr_fmt(fmt) "ACPI RAS2: " fmt

#include <linux/delay.h>
#include <linux/export.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <acpi/pcc.h>
#include <acpi/ras2.h>

/**
 * struct ras2_sspcc - Data structure for PCC communication
 * @mbox_client:	struct mbox_client object
 * @pcc_chan:		Pointer to struct pcc_mbox_chan
 * @comm_addr:		Pointer to RAS2 PCC shared memory region
 * @elem:		List for registered RAS2 PCC channel subspaces
 * @pcc_lock:		PCC lock to provide mutually exclusive access
 *			to PCC channel subspace
 * @deadline_us:	Poll PCC status register timeout in microsecs
 *			for PCC command completion
 * @pcc_mpar:		Maximum Periodic Access Rate (MPAR) for PCC channel
 * @pcc_mrtt:		Minimum Request Turnaround Time (MRTT) in microsecs
 *			OS must wait after completion of a PCC command before
 *			issuing next command
 * @last_cmd_cmpl_time: Completion time of last PCC command
 * @last_mpar_reset:	Time of last MPAR count reset
 * @mpar_count:		MPAR count
 * @pcc_id:		Identifier of the RAS2 platform communication channel
 * @last_cmd:		Last PCC command
 * @kref:		kref object
 */
struct ras2_sspcc {
	struct mbox_client		mbox_client;
	struct pcc_mbox_chan		*pcc_chan;
	struct acpi_ras2_shmem __iomem	*comm_addr;
	struct list_head		elem;
	struct mutex			pcc_lock;
	u64				deadline_us;
	unsigned int			pcc_mpar;
	unsigned int			pcc_mrtt;
	ktime_t				last_cmd_cmpl_time;
	ktime_t				last_mpar_reset;
	int				mpar_count;
	int				pcc_id;
	u16				last_cmd;
	struct kref			kref;
};

/*
 * Arbitrary retries for PCC commands because the remote processor could be
 * much slower to reply. Keep it high enough to cover emulators where the
 * processors run painfully slow.
 */
#define PCC_NUM_RETRIES 600ULL
#define PCC_CHNL_DEFAULT_LATENCY 1000
#define PCC_MIN_POLL_USECS 3

#define RAS2_MAX_NUM_PCC_DESCS 100
#define RAS2_FEAT_TYPE_MEMORY 0x00

/* Static variables for the RAS2 PCC subspaces */
static DEFINE_MUTEX(ras2_pcc_list_lock);
static LIST_HEAD(ras2_sspcc);

static int check_pcc_chan(struct ras2_sspcc *sspcc)
{
	struct acpi_ras2_shmem __iomem *gen_comm_base = sspcc->comm_addr;
	u32 cap_status;
	u16 status;
	int rc;

	/*
	 * As per ACPI spec, the PCC space will be initialized by the
	 * platform and should have set the command completion bit when
	 * PCC can be used by OSPM.
	 *
	 * Poll PCC status register every PCC_MIN_POLL_USECS for maximum of
	 * PCC_NUM_RETRIES * PCC channel latency until PCC command complete
	 * bit is set.
	 */
	rc = readw_relaxed_poll_timeout(&gen_comm_base->status, status,
					status & PCC_STATUS_CMD_COMPLETE,
					PCC_MIN_POLL_USECS, sspcc->deadline_us);
	if (rc) {
		pr_warn("PCC ID: 0x%x: PCC check channel timeout for last command: 0x%x rc=%d\n",
			sspcc->pcc_id, sspcc->last_cmd, rc);

		return rc;
	}

	if (status & PCC_STATUS_ERROR) {
		pr_warn("PCC ID: 0x%x: Error in executing last command: 0x%x\n",
			sspcc->pcc_id, sspcc->last_cmd);
		status &= ~PCC_STATUS_ERROR;
		iowrite16(status, &gen_comm_base->status);
		iowrite32(0x0, &gen_comm_base->set_caps_status);
		return -EIO;
	}

	/* Ensure get updated PCC status */
	rmb();
	cap_status = ioread32(&gen_comm_base->set_caps_status);
	switch (cap_status) {
	case ACPI_RAS2_NOT_VALID:
	case ACPI_RAS2_NOT_SUPPORTED:
		rc = -EPERM;
		break;
	case ACPI_RAS2_BUSY:
		rc = -EBUSY;
		break;
	case ACPI_RAS2_FAILED:
	case ACPI_RAS2_ABORTED:
	case ACPI_RAS2_INVALID_DATA:
		rc = -EINVAL;
		break;
	default:
		rc = 0;
	}

	iowrite32(0x0, &gen_comm_base->set_caps_status);

	return rc;
}

/**
 * ras2_send_pcc_cmd() - Send RAS2 command via PCC channel
 * @ras2_ctx:	pointer to the RAS2 context structure
 * @cmd:	RAS2 command to send
 *
 * Returns: 0 on success, an error otherwise
 */
int ras2_send_pcc_cmd(struct ras2_mem_ctx *ras2_ctx, u16 cmd)
{
	struct acpi_ras2_shmem __iomem *gen_comm_base;
	struct mbox_chan *pcc_channel;
	struct ras2_sspcc *sspcc;
	s64 time_delta;
	u16 val;
	int rc;

	if (!ras2_ctx)
		return -EINVAL;

	lockdep_assert_held(ras2_ctx->pcc_lock);
	sspcc = ras2_ctx->sspcc;
	gen_comm_base = sspcc->comm_addr;

	rc = check_pcc_chan(sspcc);
	if (rc < 0)
		return rc;

	pcc_channel = sspcc->pcc_chan->mchan;

	/*
	 * Handle the Minimum Request Turnaround Time (MRTT): the minimum
	 * amount of time that OSPM must wait after the completion of
	 * a command before issuing the next command, in microseconds.
	 */
	if (sspcc->pcc_mrtt) {
		time_delta = ktime_us_delta(ktime_get(), sspcc->last_cmd_cmpl_time);
		if (sspcc->pcc_mrtt > time_delta)
			fsleep(sspcc->pcc_mrtt - time_delta);
	}

	/*
	 * Handle the non-zero Maximum Periodic Access Rate (MPAR): the
	 * maximum number of periodic requests that the subspace channel can
	 * support, reported in commands per minute. 0 indicates no
	 * limitation.
	 *
	 * This parameter should be ideally zero or large enough so that it
	 * can handle maximum number of requests that all the cores in the
	 * system can collectively generate. If it is not, follow the spec and
	 * just not send the request to the platform after hitting the MPAR
	 * limit in any 60s window.
	 */
	if (sspcc->pcc_mpar) {
		if (!sspcc->mpar_count) {
			time_delta = ktime_ms_delta(ktime_get(), sspcc->last_mpar_reset);
			if ((time_delta < 60 * MSEC_PER_SEC) && sspcc->last_mpar_reset) {
				dev_dbg(ras2_ctx->dev,
					"PCC command 0x%x not sent due to MPAR limit", cmd);
				return -EIO;
			}
			sspcc->last_mpar_reset = ktime_get();
			sspcc->mpar_count = sspcc->pcc_mpar;
		}
		sspcc->mpar_count--;
	}

	/* Write to the shared comm region */
	iowrite16(cmd, &gen_comm_base->command);

	/* Flip CMD COMPLETE bit */
	iowrite16(0, &gen_comm_base->status);

	/* Ring doorbell */
	rc = mbox_send_message(pcc_channel, &cmd);
	/*
	 * mbox_send_message() returns a non-negative integer for successful submission
	 * and a negative value on failure.
	 */
	if (rc < 0) {
		dev_warn(ras2_ctx->dev,
			 "Error sending PCC mbox message command: 0x%x, rc:%d\n", cmd, rc);
		/* Restore CMD COMPLETE bit on error */
		val = ioread16(&gen_comm_base->status);
		val |= PCC_STATUS_CMD_COMPLETE;
		iowrite16(val, &gen_comm_base->status);
		return rc;
	} else {
		rc = 0;
	}

	sspcc->last_cmd = cmd;

	/*
	 * If Minimum Request Turnaround Time is non-zero, need to record the
	 * completion time of both READ and WRITE commands for proper handling
	 * of MRTT, so need to check for pcc_mrtt in addition to PCC_CMD_EXEC_RAS2.
	 */
	if (cmd == PCC_CMD_EXEC_RAS2 || sspcc->pcc_mrtt) {
		rc = check_pcc_chan(sspcc);
		if (sspcc->pcc_mrtt)
			sspcc->last_cmd_cmpl_time = ktime_get();
	}

	if (!pcc_channel->mbox->txdone_irq)
		mbox_client_txdone(pcc_channel, rc);

	return rc;
}
EXPORT_SYMBOL_FOR_MODULES(ras2_send_pcc_cmd, "acpi_ras2");

static void ras2_list_pcc_release(struct kref *kref)
{
	struct ras2_sspcc *sspcc =
		container_of(kref, struct ras2_sspcc, kref);

	guard(mutex)(&ras2_pcc_list_lock);
	list_del(&sspcc->elem);
	pcc_mbox_free_channel(sspcc->pcc_chan);
	kfree(sspcc);
}

static void ras2_sspcc_get(struct ras2_sspcc *sspcc)
{
	kref_get(&sspcc->kref);
}

static void ras2_sspcc_put(struct ras2_sspcc *sspcc)
{
	kref_put(&sspcc->kref,  &ras2_list_pcc_release);
}

static struct ras2_sspcc *ras2_get_sspcc(int pcc_id)
{
	struct ras2_sspcc *sspcc;

	guard(mutex)(&ras2_pcc_list_lock);
	list_for_each_entry(sspcc, &ras2_sspcc, elem) {
		if (sspcc->pcc_id != pcc_id)
			continue;
		ras2_sspcc_get(sspcc);
		return sspcc;
	}

	return NULL;
}

static int register_pcc_channel(struct ras2_mem_ctx *ras2_ctx, int pcc_id)
{
	struct pcc_mbox_chan *pcc_chan;
	struct ras2_sspcc *sspcc;

	if (pcc_id < 0)
		return -EINVAL;

	sspcc = ras2_get_sspcc(pcc_id);
	if (sspcc) {
		ras2_ctx->sspcc		= sspcc;
		ras2_ctx->comm_addr	= sspcc->comm_addr;
		ras2_ctx->dev		=
			sspcc->pcc_chan->mchan->mbox->dev;
		ras2_ctx->pcc_lock	= &sspcc->pcc_lock;
		return 0;
	}

	sspcc = kzalloc(sizeof(*sspcc), GFP_KERNEL);
	if (!sspcc)
		return -ENOMEM;

	pcc_chan = pcc_mbox_request_channel(&sspcc->mbox_client, pcc_id);
	if (IS_ERR(pcc_chan)) {
		kfree(sspcc);
		return PTR_ERR(pcc_chan);
	}

	if (!pcc_chan->shmem) {
		pcc_mbox_free_channel(sspcc->pcc_chan);
		kfree(sspcc);
		return -EINVAL;
	}

	sspcc->pcc_id		= pcc_id;
	sspcc->pcc_chan		= pcc_chan;
	sspcc->comm_addr	= pcc_chan->shmem;
	if (pcc_chan->latency)
		sspcc->deadline_us = PCC_NUM_RETRIES * pcc_chan->latency;
	else
		sspcc->deadline_us = PCC_NUM_RETRIES * PCC_CHNL_DEFAULT_LATENCY;
	sspcc->pcc_mrtt		= pcc_chan->min_turnaround_time;
	sspcc->pcc_mpar		= pcc_chan->max_access_rate;
	sspcc->mbox_client.knows_txdone	= true;

	kref_init(&sspcc->kref);

	mutex_lock(&ras2_pcc_list_lock);
	list_add(&sspcc->elem, &ras2_sspcc);
	ras2_sspcc_get(sspcc);
	mutex_unlock(&ras2_pcc_list_lock);

	ras2_ctx->sspcc		= sspcc;
	ras2_ctx->comm_addr	= sspcc->comm_addr;
	ras2_ctx->dev		= pcc_chan->mchan->mbox->dev;

	mutex_init(&sspcc->pcc_lock);
	ras2_ctx->pcc_lock	= &sspcc->pcc_lock;

	return 0;
}

static DEFINE_IDA(ras2_ida);
static void ras2_release(struct device *device)
{
	struct auxiliary_device *auxdev = to_auxiliary_dev(device);
	struct ras2_mem_ctx *ras2_ctx = container_of(auxdev, struct ras2_mem_ctx, adev);

	ida_free(&ras2_ida, auxdev->id);
	ras2_sspcc_put(ras2_ctx->sspcc);
	kfree(ras2_ctx);
}

static struct ras2_mem_ctx *add_aux_device(char *name, int channel, u32 pxm_inst)
{
	struct ras2_mem_ctx *ras2_ctx;
	struct ras2_sspcc *sspcc;
	u32 comp_nid;
	int id, rc;

	comp_nid = pxm_to_node(pxm_inst);
	if (comp_nid == NUMA_NO_NODE) {
		pr_debug("Invalid NUMA node, channel=%d pxm_inst=%d\n", channel, pxm_inst);
		return ERR_PTR(-ENXIO);
	}

	ras2_ctx = kzalloc(sizeof(*ras2_ctx), GFP_KERNEL);
	if (!ras2_ctx)
		return ERR_PTR(-ENOMEM);

	ras2_ctx->sys_comp_nid = comp_nid;

	rc = register_pcc_channel(ras2_ctx, channel);
	if (rc < 0) {
		pr_debug("Failed to register PCC channel=%d pxm_inst=%d rc=%d\n", channel,
			 pxm_inst, rc);
		goto ctx_free;
	}

	id = ida_alloc(&ras2_ida, GFP_KERNEL);
	if (id < 0) {
		rc = id;
		goto pcc_free;
	}

	ras2_ctx->adev.id		= id;
	ras2_ctx->adev.name		= name;
	ras2_ctx->adev.dev.release	= ras2_release;
	ras2_ctx->adev.dev.parent	= ras2_ctx->dev;

	rc = auxiliary_device_init(&ras2_ctx->adev);
	if (rc)
		goto ida_free;

	rc = auxiliary_device_add(&ras2_ctx->adev);
	if (rc) {
		auxiliary_device_uninit(&ras2_ctx->adev);
		return ERR_PTR(rc);
	}

	return ras2_ctx;

ida_free:
	ida_free(&ras2_ida, id);
pcc_free:
	sspcc = ras2_ctx->sspcc;
	pcc_mbox_free_channel(sspcc->pcc_chan);
	kfree(sspcc);
ctx_free:
	kfree(ras2_ctx);

	return ERR_PTR(rc);
}

static void remove_aux_device(struct ras2_mem_ctx *ras2_ctx)
{
	if (!ras2_ctx)
		return;

	auxiliary_device_delete(&ras2_ctx->adev);
	auxiliary_device_uninit(&ras2_ctx->adev);
}

static int parse_ras2_table(struct acpi_table_ras2 *ras2_tab)
{
	struct acpi_ras2_pcc_desc *pcc_desc_list;
	struct ras2_mem_ctx **pctx_list;
	struct ras2_mem_ctx *ras2_ctx;
	u16 tot_tbl_len;
	u16 i;

	if (ras2_tab->header.length < sizeof(*ras2_tab)) {
		pr_warn(FW_WARN "ACPI RAS2 table present but broken (too short, size=%u)\n",
			ras2_tab->header.length);
		return -EINVAL;
	}

	if (!ras2_tab->num_pcc_descs || ras2_tab->num_pcc_descs > RAS2_MAX_NUM_PCC_DESCS) {
		pr_warn(FW_WARN "No/Invalid number of PCC descs(%d) in ACPI RAS2 table\n",
			ras2_tab->num_pcc_descs);
		return -EINVAL;
	}

	tot_tbl_len = sizeof(*ras2_tab) + ras2_tab->num_pcc_descs * sizeof(*pcc_desc_list);
	if (ras2_tab->header.length < tot_tbl_len) {
		pr_warn(FW_WARN "RAS2 table is not large enough to contain PCC descs=%d size=%u)\n",
			ras2_tab->num_pcc_descs, ras2_tab->header.length);
		return -EINVAL;
	}

	pctx_list = kcalloc(ras2_tab->num_pcc_descs, sizeof(*pctx_list), GFP_KERNEL);
	if (!pctx_list)
		return -ENOMEM;

	pcc_desc_list = (struct acpi_ras2_pcc_desc *)(ras2_tab + 1);
	for (i = 0; i < ras2_tab->num_pcc_descs; i++, pcc_desc_list++) {
		if (pcc_desc_list->feature_type != RAS2_FEAT_TYPE_MEMORY)
			continue;

		ras2_ctx = add_aux_device(RAS2_MEM_DEV_ID_NAME, pcc_desc_list->channel_id,
					  pcc_desc_list->instance);
		/* Invalid NUMA node, continue parsing next node */
		if (PTR_ERR(ras2_ctx) == -ENXIO)
			continue;

		if (IS_ERR(ras2_ctx)) {
			pr_warn("Failed to add RAS2 auxiliary device rc=%ld\n", PTR_ERR(ras2_ctx));
			for (; i > 0; i--) {
				if (pctx_list[i - 1])
					remove_aux_device(pctx_list[i - 1]);
			}
			kfree(pctx_list);
			return PTR_ERR(ras2_ctx);
		}
		pctx_list[i] = ras2_ctx;
	}
	kfree(pctx_list);

	return 0;
}

/**
 * acpi_ras2_init - RAS2 driver initialization function.
 *
 * Extracts the ACPI RAS2 table and retrieves ID for the PCC channel subspace
 * for communicating with the ACPI compliant HW platform. Driver adds an
 * auxiliary device, which binds to the memory ACPI RAS2 driver, for each RAS2
 * memory feature.
 *
 * Returns: none.
 */
void __init acpi_ras2_init(void)
{
	struct acpi_table_ras2 *ras2_tab;
	acpi_status status;

	status = acpi_get_table(ACPI_SIG_RAS2, 0, (struct acpi_table_header **)&ras2_tab);
	if (ACPI_FAILURE(status)) {
		pr_debug("Failed to get table, %s\n", acpi_format_exception(status));
		return;
	}

	if (parse_ras2_table(ras2_tab))
		pr_debug("Failed to parse RAS2 table\n");

	acpi_put_table((struct acpi_table_header *)ras2_tab);
}
