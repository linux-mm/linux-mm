/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ACPI RAS2 (RAS Feature Table) methods.
 *
 * Copyright (c) 2024-2026 HiSilicon Limited
 */

#ifndef _ACPI_RAS2_H
#define _ACPI_RAS2_H

#include <linux/acpi.h>
#include <linux/auxiliary_bus.h>
#include <linux/mailbox_client.h>
#include <linux/mutex.h>
#include <linux/types.h>

struct device;

/*
 * ACPI spec 6.5 Table 5.82: PCC command codes used by
 * RAS2 platform communication channel.
 */
#define PCC_CMD_EXEC_RAS2 0x01

#define RAS2_AUX_DEV_NAME "ras2"
#define RAS2_MEM_DEV_ID_NAME "acpi_ras2_mem"

/**
 * struct ras2_mem_ctx - Context for RAS2 memory features
 * @adev:		Auxiliary device object
 * @comm_addr:		Pointer to RAS2 PCC shared memory region
 * @dev:		Pointer to device backing struct mbox_controller for PCC
 * @sspcc:		Pointer to local data structure for PCC communication
 * @pcc_lock:		Pointer to PCC lock to provide mutually exclusive access
 *			to PCC channel subspace
 * @sys_comp_nid:	Node ID of the system component that the RAS feature
 *			is associated with. See ACPI spec 6.5 Table 5.80: RAS2
 *			Platform Communication Channel Descriptor format,
 *			Field: Instance
 * @mem_base:		Base of the lowest physical continuous memory range
 *			of the memory associated with the NUMA domain
 * @mem_size		Size of the lowest physical continuous memory range
 *			of the memory associated with the NUMA domain
 * @base:		Base address of the memory region to scrub
 * @size:		Size of the memory region to scrub
 * @scrub_cycle_hrs:	Current scrub rate in hours
 * @set_scrub_cycle:	Scrub rate to set in hours
 * @min_scrub_cycle:	Minimum scrub rate supported
 * @max_scrub_cycle:	Maximum scrub rate supported
 * @od_scrub:		Status of demand scrubbing (memory region)
 * @bg_scrub:		Status of background patrol scrubbing
 * @reenable_bg_scrub:	Flag indicates restart background scrubbing after demand
 *			scrubbing is finished
 * @thread:		Demand scrub monitor kthread
 * @driver_active:	Flag indicates RAS2 memory driver is active/removed
 */
struct ras2_mem_ctx {
	struct auxiliary_device		adev;
	struct acpi_ras2_shmem __iomem	*comm_addr;
	struct device			*dev;
	void				*sspcc;
	struct mutex			*pcc_lock;
	u32				sys_comp_nid;
	u64				mem_base;
	u64				mem_size;
	u64				base;
	u64				size;
	u8				scrub_cycle_hrs;
	u8				set_scrub_cycle;
	u8				min_scrub_cycle;
	u8				max_scrub_cycle;
	bool				od_scrub;
	bool				bg_scrub;
	bool				reenable_bg_scrub;
	struct task_struct		*thread;
	bool				driver_active;
};

#ifdef CONFIG_ACPI_RAS2
void __init acpi_ras2_init(void);
int ras2_send_pcc_cmd(struct ras2_mem_ctx *ras2_ctx, u16 cmd);
#else
static inline void acpi_ras2_init(void) { }
#endif

#endif /* _ACPI_RAS2_H */
