/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 *
 * Copyright (C) 2025 Amazon.com Inc. or its affiliates.
 * Pratyush Yadav <ptyadav@amazon.de>
 */

#ifndef _LINUX_KHO_ABI_MEMFD_H
#define _LINUX_KHO_ABI_MEMFD_H

#include <linux/build_bug.h>
#include <linux/types.h>
#include <linux/kho/abi/kexec_handover.h>
#include <linux/kho/abi/luo.h>

/**
 * DOC: memfd Live Update ABI
 *
 * memfd uses the ABI defined below for preserving its state across a kexec
 * reboot using the LUO.
 *
 * The state is serialized into `struct memfd_luo_ser`
 * which is handed over to the target kernel via the KHO mechanism.
 *
 * This interface is a contract. The serialized state starts with an
 * 8-byte `struct luo_ser_hdr` followed by `struct memfd_luo_ser`.
 * The relative order and offsets of fields must remain unchanged as
 * features are added or retired: new fields are appended at the end of
 * `struct memfd_luo_ser` under a new feature flag, and retired fields are
 * kept in place so subsequent offsets never shift. In the target kernel,
 * the fields of a feature are read only after `luo_ser_get_feat()` shows
 * the feature present.
 *
 * Any modification to the structure layout beyond feature extensions
 * constitutes a breaking change. Such changes require incrementing the
 * version number in the MEMFD_LUO_FH_COMPATIBLE string.
 */

/**
 * MEMFD_LUO_FOLIO_DIRTY - The folio is dirty.
 *
 * This flag indicates the folio contains data from user. A non-dirty folio is
 * one that was allocated (say using fallocate(2)) but not written to.
 */
#define MEMFD_LUO_FOLIO_DIRTY		BIT(0)

/**
 * MEMFD_LUO_FOLIO_UPTODATE - The folio is up-to-date.
 *
 * An up-to-date folio has been zeroed out. shmem zeroes out folios on first
 * use. This flag tracks which folios need zeroing.
 */
#define MEMFD_LUO_FOLIO_UPTODATE	BIT(1)

/**
 * struct memfd_luo_folio_ser - Serialized state of a single folio.
 * @pfn:       The page frame number of the folio.
 * @flags:     Flags to describe the state of the folio.
 * @index:     The page offset (pgoff_t) of the folio within the original file.
 */
struct memfd_luo_folio_ser {
	u64 pfn:52;
	u64 flags:12;
	u64 index;
};

static_assert(sizeof(struct memfd_luo_folio_ser) == 16);

/*
 * The set of seals this version supports preserving. If support for any new
 * seals is needed, add it here and bump version.
 */
#define MEMFD_LUO_ALL_SEALS (F_SEAL_SEAL | \
			     F_SEAL_SHRINK | \
			     F_SEAL_GROW | \
			     F_SEAL_WRITE | \
			     F_SEAL_FUTURE_WRITE | \
			     F_SEAL_EXEC)

/**
 * enum memfd_luo_feature - Feature indices for &struct memfd_luo_ser.
 * @MEMFD_LUO_FEAT_CORE: Core feature covering `pos`, `size`, `seals`, `flags`,
 *                       `nr_folios`, `folios`, and %MEMFD_LUO_ALL_SEALS.
 * @MEMFD_LUO_FEAT_MAX:  Number of defined &enum memfd_luo_feature features.
 */
enum memfd_luo_feature {
	MEMFD_LUO_FEAT_CORE = 0,
	MEMFD_LUO_FEAT_MAX,
};

/**
 * struct memfd_luo_ser - Main serialization structure for a memfd.
 * @pos:       The file's current position (f_pos).
 * @size:      The total size of the file in bytes (i_size).
 * @seals:     The seals present on the memfd. The seals are uABI so it is safe
 *             to directly use them in the ABI.
 * @flags:     Flags for the file. Unused flag bits must be set to 0.
 * @nr_folios: Number of folios in the folios array.
 * @folios:    KHO vmalloc descriptor pointing to the array of
 *             struct memfd_luo_folio_ser.
 *
 * Preceded by &struct luo_ser_hdr.
 */
struct memfd_luo_ser {
	u64 pos;
	u64 size;
	u32 seals;
	u32 flags;
	u64 nr_folios;
	struct kho_vmalloc folios;
};

static_assert(sizeof(struct memfd_luo_ser) == 48);

/* memfd state written by this kernel. Never cast incoming state to it. */
DECLARE_LUO_SER_WRAP(memfd_luo_ser, 64, MEMFD_LUO_FEAT_MAX);

/* The compatibility string for memfd file handler */
#define MEMFD_LUO_FH_COMPATIBLE	"memfd-v3"

#endif /* _LINUX_KHO_ABI_MEMFD_H */
