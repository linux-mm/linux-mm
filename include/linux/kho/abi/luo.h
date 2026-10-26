/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

/**
 * DOC: Live Update Orchestrator ABI
 *
 * Live Update Orchestrator uses the stable Application Binary Interface
 * defined below to pass state from a source kernel to a target kernel.
 * The ABI is built upon the Kexec HandOver framework and registers the
 * central `struct luo_ser` via the KHO raw subtree API.
 *
 * This interface is a contract. `struct luo_ser`, and the serialized state
 * of each file handler that uses it, start with an 8-byte base feature
 * header (`struct luo_ser_hdr`) holding the combined structure `size`
 * (`__u32`) and `nr_features` (`__u32`), followed by an array of 64-bit
 * `flags` entries (`__u64 flags[]`) before the payload structure.
 * When `nr_features == 0`, `flags` has zero entries and the header is exactly
 * 8 bytes. For `1 <= nr_features <= 32`, `flags` has 1 entry and the header
 * is 16 bytes.
 *
 * Feature state encoding (2 bits per feature):
 *   `LUO_FEAT_ABSENT`   = 0b00: Feature absent in serialized payload
 *   `LUO_FEAT_OPTIONAL` = 0b01: Feature present in serialized payload (optional)
 *   `LUO_FEAT_RESERVED` = 0b10: Reserved encoding (not validated)
 *   `LUO_FEAT_REQUIRED` = 0b11: Feature present in serialized payload (required)
 *
 * Feature flags mapping:
 *   - Each 64-bit `flags` entry encodes 32 features (2 bits per feature).
 *   - Feature N is encoded in `flags[N / 32]` at bits
 *     `[2 * (N % 32) + 1 : 2 * (N % 32)]`.
 *
 * Truth table::
 *
 *   Source hdr  Target supported[i]  Action
 *   ----------  -------------------  ----------------------------------
 *   00          0 or 1               Accept: feature absent in payload
 *   01          0                    Accept: ignore the feature data
 *   01          1                    Accept: use the feature data
 *   11          0                    Reject: -EOPNOTSUPP
 *   11          1                    Accept: use the feature data
 *   10          0 or 1               Reserved: not checked by target
 *
 * Source hdr is the 2-bit state (enum luo_feat_state) of feature i in the
 * header written by the source kernel. Target supported[i] is the boolean entry
 * for feature i in the target kernel's supported feature table passed to
 * luo_ser_hdr_validate() (1 = supported, 0 = not supported; an index at or
 * beyond @nr_supported, or a NULL table, is 0).
 *
 * The relative order and offsets of fields in serialized structures must
 * remain unchanged as features are added or retired. New fields are always
 * appended at the end of the structure under a new feature flag, and
 * retired fields are kept in place so that the offsets of subsequent
 * fields never shift. If a field's type or format needs to change, the
 * existing field and feature are kept intact (and retired when no longer
 * needed) and a new feature flag with a new appended field of the new type
 * is added.
 *
 * Apart from missing required features (-EOPNOTSUPP), `luo_ser_hdr_validate()`
 * rejects a header only when the source kernel marks a feature `LUO_FEAT_REQUIRED`
 * that the target kernel does not support. A feature the source kernel did
 * not send reads as `LUO_FEAT_ABSENT`, so the target kernel reads the fields
 * of a feature only after checking it::
 *
 *   if (luo_ser_get_feat(hdr, FEAT_X) != LUO_FEAT_ABSENT)
 *           val = ser->x_field;
 *
 * The source kernel is responsible for marking a feature it relies on as
 * `LUO_FEAT_REQUIRED`. The target kernel handles each feature in its own
 * retrieve code, for example by falling back to default behavior when a
 * feature is absent; it does not guard against a source kernel that sets
 * the wrong states.
 *
 * KHO Structure Overview:
 *   The entire LUO state is encapsulated within a single KHO entry named "LUO".
 *   This entry contains a `struct luo_ser_hdr` followed by the
 *   `struct luo_ser` payload.
 *
 * Serialization Structures:
 *   - struct luo_ser:
 *     The central ABI payload structure that contains the overall state of
 *     the LUO (`LUO_CORE_FEAT_CORE`: the liveupdate-number and physical
 *     pointers to sessions and FLBs), preceded by `struct luo_ser_hdr`.
 *
 *   - struct luo_session_ser:
 *     Metadata for a single session, including its name and a physical pointer
 *     to the first `struct kho_block_header_ser` for all files in that session.
 *     Multiple blocks are linked via the `next` field in the header.
 *
 *   - struct luo_file_ser:
 *     Metadata for a single preserved file. Contains the `compatible` string to
 *     find the correct handler in the target kernel, a user-provided `token` for
 *     identification, and an opaque `data` handle for the handler to use.
 *
 *   - struct luo_flb_header_ser:
 *     Header for the FLB array. Contains the total page count of the
 *     preserved memory block and the number of `struct luo_flb_ser` entries
 *     that follow.
 *
 *   - struct luo_flb_ser:
 *     Metadata for a single preserved global object. Contains its `name`
 *     (compatible string), an opaque `data` handle, and the `count`
 *     number of files depending on it.
 */

#ifndef _LINUX_KHO_ABI_LUO_H
#define _LINUX_KHO_ABI_LUO_H

#include <linux/bits.h>
#include <linux/bug.h>
#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/kho/abi/block.h>
#include <linux/math.h>
#include <linux/string.h>
#include <linux/types.h>
#include <uapi/linux/liveupdate.h>

/*
 * The LUO state is registered under this KHO entry name.
 */
#define LUO_KHO_ENTRY_NAME	"LUO"

#define LUO_FEAT_PER_FLAGS	32
#define LUO_FEAT_STATE_MASK	0b11ULL

/**
 * enum luo_feat_state - 2-bit state of a feature in the serialized payload.
 * @LUO_FEAT_ABSENT:   Feature is absent in the serialized payload (`0b00`).
 * @LUO_FEAT_OPTIONAL: Feature is present and optional (`0b01`).
 * @LUO_FEAT_RESERVED: Reserved encoding (`0b10`).
 * @LUO_FEAT_REQUIRED: Feature is present and required (`0b11`).
 */
enum luo_feat_state {
	LUO_FEAT_ABSENT   = 0b00,
	LUO_FEAT_OPTIONAL = 0b01,
	LUO_FEAT_RESERVED = 0b10,
	LUO_FEAT_REQUIRED = 0b11,
};

/**
 * struct luo_ser_hdr - Base 8-byte header for serialized LUO ABI structures.
 * @size:        Total size of the serialized structure in bytes (header +
 *               `flags` + payload).
 * @nr_features: Number of features encoded in @flags.
 * @flags:       64-bit feature flags entries (`DIV_ROUND_UP(@nr_features, 32)`
 *               entries). Each entry encodes 32 features (2 bits per feature:
 *               feature `N` is in `flags[N / 32]` bits
 *               `[2*(N % 32) + 1 : 2*(N % 32)]`).
 */
struct luo_ser_hdr {
	__u32 size;
	__u32 nr_features;
	__u64 flags[];
};

static_assert(sizeof(struct luo_ser_hdr) == 8);

/*
 * Each subsystem declares a wrapper structure with DECLARE_LUO_SER_WRAP() (for
 * example `struct memfd_luo_ser_wrap` or `struct luo_ser_wrap`) holding the
 * header, any 64-bit `flags` entries and its payload, allocates it with
 * sizeof() and initializes the header with the generated wrapper initializer
 * (e.g. luo_ser_wrap_init()). The structure matches only the layout this kernel
 * writes, so incoming state is never cast to it; the target kernel uses
 * luo_ser_hdr_validate() and luo_ser_payload() instead.
 */

#define LUO_SER_NR_FLAGS(nr_features)	DIV_ROUND_UP_POW2(nr_features, LUO_FEAT_PER_FLAGS)

/**
 * DECLARE_LUO_SER_WRAP - Declare an outgoing LUO header + payload wrapper structure.
 * @name:          Name of the payload structure (`struct @name`).
 * @expected_size: Expected total size in bytes of `struct @name_wrap`.
 * @nr_features:   Number of features defined for `struct @name`.
 *
 * Declares `struct @name_wrap` and inline initializer `@name_wrap_init()`
 * for the source kernel to allocate and populate when preserving state.
 * It matches only the layout written by this kernel; incoming state from
 * another kernel must be validated with luo_ser_hdr_validate() and its
 * payload reached via luo_ser_payload().
 */
#define DECLARE_LUO_SER_WRAP(name, expected_size, nr_features)		\
	struct name##_wrap {						\
		union {							\
			struct luo_ser_hdr hdr;				\
			struct {					\
				__u32 __size;				\
				__u32 __nr_features;			\
				__u64 flags[LUO_SER_NR_FLAGS(nr_features)];\
				struct name ser;			\
			};						\
		};							\
	};								\
	static_assert(sizeof(struct name##_wrap) == (expected_size),	\
		      "LUO struct " #name " size mismatch!");		\
	static inline void name##_wrap_init(struct name##_wrap *wrap)	\
	{								\
		luo_ser_hdr_init(&wrap->hdr, sizeof(*wrap), (nr_features));\
	}

/**
 * luo_ser_hdr_size - Header size of a serialized header.
 * @hdr: Pointer to the serialized &struct luo_ser_hdr.
 *
 * Return: Size of @hdr in bytes, including its 64-bit `flags` entries.
 */
static inline size_t luo_ser_hdr_size(const struct luo_ser_hdr *hdr)
{
	return struct_size(hdr, flags, LUO_SER_NR_FLAGS(hdr->nr_features));
}

/**
 * luo_ser_hdr_init - Initialize a feature header.
 * @hdr:         Pointer to the &struct luo_ser_hdr to initialize.
 * @size:        Total size of the serialized structure (header, `flags` entries
 *               and payload), normally sizeof() of the wrapper structure.
 * @nr_features: Number of features to accommodate.
 *
 * Sets @hdr->size and @hdr->nr_features, and clears the 64-bit `flags` entries.
 * Touches only the header.
 */
static inline void
luo_ser_hdr_init(struct luo_ser_hdr *hdr, size_t size, u32 nr_features)
{
	hdr->size = size;
	hdr->nr_features = nr_features;
	memset(hdr->flags, 0, LUO_SER_NR_FLAGS(nr_features) * sizeof(hdr->flags[0]));
}

/*
 * These helpers operate on any well-formed header: one initialized on the
 * outgoing path or validated by luo_ser_hdr_validate() on the incoming path.
 */

/**
 * luo_ser_payload - Get pointer to the payload immediately following @hdr.
 * @hdr: Pointer to the serialized &struct luo_ser_hdr.
 *
 * @hdr must be initialized or validated.
 *
 * Return: Pointer to the payload structure.
 */
static inline void *luo_ser_payload(const struct luo_ser_hdr *hdr)
{
	return (void *)((const char *)hdr + luo_ser_hdr_size(hdr));
}

/**
 * luo_ser_set_feat - Set the 2-bit state of feature @feat in @hdr.
 * @hdr:   Pointer to &struct luo_ser_hdr.
 * @feat:  Feature index.
 * @state: Feature state (&enum luo_feat_state).
 *
 * @hdr must be initialized or validated.
 */
static inline void
luo_ser_set_feat(struct luo_ser_hdr *hdr, u32 feat, enum luo_feat_state state)
{
	u32 idx, slot;

	if (WARN_ON_ONCE(feat >= hdr->nr_features))
		return;

	idx = feat / LUO_FEAT_PER_FLAGS;
	slot = feat % LUO_FEAT_PER_FLAGS;
	hdr->flags[idx] &= ~(LUO_FEAT_STATE_MASK << (2 * slot));
	hdr->flags[idx] |= (u64)(state & LUO_FEAT_STATE_MASK) << (2 * slot);
}

/**
 * luo_ser_get_feat - Query the 2-bit state of feature @feat in @hdr.
 * @hdr:  Pointer to &struct luo_ser_hdr.
 * @feat: Feature index.
 *
 * @hdr must be initialized or validated.
 *
 * Return: &enum luo_feat_state (returns %LUO_FEAT_ABSENT if @feat is beyond
 *         @hdr->nr_features).
 */
static inline enum luo_feat_state
luo_ser_get_feat(const struct luo_ser_hdr *hdr, u32 feat)
{
	u32 idx, slot;

	if (feat >= hdr->nr_features)
		return LUO_FEAT_ABSENT;

	idx = feat / LUO_FEAT_PER_FLAGS;
	slot = feat % LUO_FEAT_PER_FLAGS;

	return (hdr->flags[idx] >> (2 * slot)) & LUO_FEAT_STATE_MASK;
}

/**
 * luo_ser_hdr_validate - Validate source kernel's feature header.
 * @hdr:          Pointer to the feature header from the source kernel.
 * @supported:    Target kernel's supported features table, indexed by feature.
 * @nr_supported: Number of entries in @supported.
 *
 * Checks each source feature state in @hdr against @supported[i] using the
 * truth table in the DOC section above.
 *
 * Return: 0 if compatible, or -EOPNOTSUPP on required feature mismatch.
 */
static inline int
luo_ser_hdr_validate(const struct luo_ser_hdr *hdr,
		     const bool *supported, u32 nr_supported)
{
	u32 feat;

	for (feat = 0; feat < hdr->nr_features; feat++)
		if (luo_ser_get_feat(hdr, feat) == LUO_FEAT_REQUIRED &&
		    (feat >= nr_supported || !supported[feat]))
			return -EOPNOTSUPP;

	return 0;
}

/**
 * enum luo_core_feature - Feature indices for &struct luo_ser.
 * @LUO_CORE_FEAT_CORE: Core feature covering `liveupdate_num`, `sessions_pa`
 *                      and `flbs_pa`.
 * @LUO_CORE_FEAT_MAX:  Number of defined &enum luo_core_feature features.
 */
enum luo_core_feature {
	LUO_CORE_FEAT_CORE = 0,
	LUO_CORE_FEAT_MAX,
};

/**
 * struct luo_ser - Centralized LUO ABI payload.
 * @liveupdate_num: A counter tracking the number of successful live updates.
 * @sessions_pa:    Physical address of the first session block header.
 * @flbs_pa:        Physical address of the FLB header.
 *
 * This structure is the root payload of all preserved LUO state, preceded by
 * &struct luo_ser_hdr.
 */
struct luo_ser {
	u64 liveupdate_num;
	u64 sessions_pa;
	u64 flbs_pa;
};

static_assert(sizeof(struct luo_ser) == 24);

/* LUO state written by this kernel. Never cast incoming state to it. */
DECLARE_LUO_SER_WRAP(luo_ser, 40, LUO_CORE_FEAT_MAX);

#define LIVEUPDATE_HNDL_COMPAT_LENGTH	48

/**
 * struct luo_file_ser - Represents the serialized preserves files.
 * @compatible:  File handler compatible string.
 * @data:        Private data
 * @token:       User provided token for this file
 *
 * Layout changes must be gated by a feature in &enum luo_core_feature.
 */
struct luo_file_ser {
	char compatible[LIVEUPDATE_HNDL_COMPAT_LENGTH];
	u64 data;
	u64 token;
};

static_assert(sizeof(struct luo_file_ser) == 64);

/**
 * struct luo_file_set_ser - Represents the serialized metadata for file set
 * @files:   The physical address of the first `struct kho_block_header_ser`.
 *           This structure is the header for a block of memory containing
 *           an array of `struct luo_file_ser` entries. Multiple blocks are
 *           linked via the `next` field in the header.
 * @count:   The total number of files that were part of this session during
 *           serialization. Used for iteration and validation during
 *           restoration.
 */
struct luo_file_set_ser {
	u64 files;
	u64 count;
};

static_assert(sizeof(struct luo_file_set_ser) == 16);

/**
 * struct luo_session_ser - Represents the serialized metadata for a LUO session.
 * @name:         The unique name of the session, provided by the userspace at
 *                the time of session creation.
 * @file_set_ser: Serialized files belonging to this session,
 *
 * This structure is used to package session-specific metadata for transfer
 * between kernels via Kexec Handover. An array of these structures (one per
 * session) is created and passed to the target kernel, allowing it to
 * reconstruct the session context.
 *
 * Layout changes must be gated by a feature in &enum luo_core_feature.
 */
struct luo_session_ser {
	char name[LIVEUPDATE_SESSION_NAME_LENGTH];
	struct luo_file_set_ser file_set_ser;
};

static_assert(sizeof(struct luo_session_ser) == 80);

/* The max size is set so it can be reliably used during in serialization */
#define LIVEUPDATE_FLB_COMPAT_LENGTH	48

/**
 * struct luo_flb_header_ser - Header for the serialized FLB data block.
 * @pgcnt: The total number of pages occupied by the entire preserved memory
 *         region, including this header and the subsequent array of
 *         &struct luo_flb_ser entries.
 * @count: The number of &struct luo_flb_ser entries that follow this header
 *         in the memory block.
 *
 * This structure is located at the physical address specified by the
 * flbs_pa in luo_ser.
 *
 * Layout changes must be gated by a feature in &enum luo_core_feature.
 */
struct luo_flb_header_ser {
	u64 pgcnt;
	u64 count;
};

static_assert(sizeof(struct luo_flb_header_ser) == 16);

/**
 * struct luo_flb_ser - Represents the serialized state of a single FLB object.
 * @name:    The unique compatibility string of the FLB object, used to find the
 *           corresponding &struct liveupdate_flb in the target kernel.
 * @data:    The opaque u64 handle returned by the FLB's .preserve() operation
 *           in the source kernel. This handle encapsulates the entire state
 *           needed for restoration.
 * @count:   The reference count at the time of serialization; i.e., the number
 *           of preserved files that depended on this FLB. This is used by the
 *           target kernel to correctly manage the FLB's lifecycle.
 *
 * An array of these structures is created in a preserved memory region and
 * passed to the target kernel. Each entry allows the LUO core to restore
 * one global, shared object.
 *
 * Layout changes must be gated by a feature in &enum luo_core_feature.
 */
struct luo_flb_ser {
	char name[LIVEUPDATE_FLB_COMPAT_LENGTH];
	u64 data;
	u64 count;
};

static_assert(sizeof(struct luo_flb_ser) == 64);

/* Kernel Live Update Test ABI */
#ifdef CONFIG_LIVEUPDATE_TEST
#define LIVEUPDATE_TEST_FLB_COMPATIBLE(i)	"liveupdate-test-flb-v" #i
#endif

#endif /* _LINUX_KHO_ABI_LUO_H */
