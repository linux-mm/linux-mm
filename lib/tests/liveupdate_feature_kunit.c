// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the Live Update Orchestrator feature header helpers
 * in include/linux/kho/abi/luo.h.
 *
 * Copyright (c) 2026, Google LLC.
 * Tarun Sahu <tarunsahu@google.com>
 */

#include <kunit/test.h>
#include <linux/kho/abi/luo.h>
#include <linux/module.h>

struct luo_test_multi_ser {
	u64 val;
};

static_assert(sizeof(struct luo_test_multi_ser) == 8);
DECLARE_LUO_SER_WRAP(luo_test_multi_ser, 32, 35);

struct luo_test_one_ser {
	u64 val;
};

static_assert(sizeof(struct luo_test_one_ser) == 8);
DECLARE_LUO_SER_WRAP(luo_test_one_ser, 24, 1);

struct luo_test_zero_ser {
	u64 val;
};

static_assert(sizeof(struct luo_test_zero_ser) == 8);
DECLARE_LUO_SER_WRAP(luo_test_zero_ser, 16, 0);

static void luo_ser_hdr_set_get_test(struct kunit *test)
{
	struct luo_test_multi_ser_wrap out;
	struct luo_test_zero_ser_wrap zero;

	memset(&out, 0, sizeof(out));
	luo_test_multi_ser_wrap_init(&out);

	KUNIT_EXPECT_EQ(test, out.hdr.size, 32U);
	KUNIT_EXPECT_EQ(test, out.hdr.nr_features, 35U);
	KUNIT_EXPECT_EQ(test, LUO_SER_NR_FLAGS(out.hdr.nr_features), 2U);
	KUNIT_EXPECT_EQ(test, luo_ser_hdr_size(&out.hdr), 24UL);
	KUNIT_EXPECT_PTR_EQ(test, luo_ser_payload(&out.hdr), (void *)&out.ser);

	/* 0, 31 in flags[0]; 32, 34 in flags[1] */
	luo_ser_set_feat(&out.hdr, 0, LUO_FEAT_REQUIRED);
	luo_ser_set_feat(&out.hdr, 31, LUO_FEAT_OPTIONAL);
	luo_ser_set_feat(&out.hdr, 32, LUO_FEAT_REQUIRED);
	luo_ser_set_feat(&out.hdr, 34, LUO_FEAT_OPTIONAL);

	KUNIT_EXPECT_EQ(test, (int)luo_ser_get_feat(&out.hdr, 0), (int)LUO_FEAT_REQUIRED);
	KUNIT_EXPECT_EQ(test, (int)luo_ser_get_feat(&out.hdr, 1), (int)LUO_FEAT_ABSENT);
	KUNIT_EXPECT_EQ(test, (int)luo_ser_get_feat(&out.hdr, 31), (int)LUO_FEAT_OPTIONAL);
	KUNIT_EXPECT_EQ(test, (int)luo_ser_get_feat(&out.hdr, 32), (int)LUO_FEAT_REQUIRED);
	KUNIT_EXPECT_EQ(test, (int)luo_ser_get_feat(&out.hdr, 33), (int)LUO_FEAT_ABSENT);
	KUNIT_EXPECT_EQ(test, (int)luo_ser_get_feat(&out.hdr, 34), (int)LUO_FEAT_OPTIONAL);

	KUNIT_EXPECT_EQ(test, (int)luo_ser_get_feat(&out.hdr, 35), (int)LUO_FEAT_ABSENT);
	KUNIT_EXPECT_EQ(test, (int)luo_ser_get_feat(&out.hdr, 100), (int)LUO_FEAT_ABSENT);

	memset(&zero, 0, sizeof(zero));
	luo_test_zero_ser_wrap_init(&zero);
	KUNIT_EXPECT_EQ(test, sizeof(struct luo_ser_hdr), 8UL);
	KUNIT_EXPECT_EQ(test, luo_ser_hdr_size(&zero.hdr), 8UL);
	KUNIT_EXPECT_PTR_EQ(test, luo_ser_payload(&zero.hdr), (void *)&zero.ser);
	KUNIT_EXPECT_EQ(test, luo_ser_hdr_validate(&zero.hdr, NULL, 0), 0);
}

static void luo_ser_hdr_truth_table_test(struct kunit *test)
{
	struct luo_test_one_ser_wrap out;
	bool supp_yes[1] = { true };
	bool supp_no[1] = { false };

	memset(&out, 0, sizeof(out));
	luo_test_one_ser_wrap_init(&out);
	luo_ser_set_feat(&out.hdr, 0, LUO_FEAT_ABSENT);
	KUNIT_EXPECT_EQ(test, luo_ser_hdr_validate(&out.hdr, supp_no, 1), 0);

	luo_ser_set_feat(&out.hdr, 0, LUO_FEAT_OPTIONAL);
	KUNIT_EXPECT_EQ(test, luo_ser_hdr_validate(&out.hdr, supp_no, 1), 0);
	KUNIT_EXPECT_EQ(test, luo_ser_hdr_validate(&out.hdr, supp_yes, 1), 0);

	luo_ser_set_feat(&out.hdr, 0, LUO_FEAT_REQUIRED);
	KUNIT_EXPECT_EQ(test, luo_ser_hdr_validate(&out.hdr, supp_no, 1),
			-EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, luo_ser_hdr_validate(&out.hdr, supp_yes, 1), 0);

	out.flags[0] = (u64)LUO_FEAT_RESERVED;
	KUNIT_EXPECT_EQ(test, luo_ser_hdr_validate(&out.hdr, supp_no, 1), 0);
	KUNIT_EXPECT_EQ(test, luo_ser_hdr_validate(&out.hdr, supp_yes, 1), 0);

	luo_ser_set_feat(&out.hdr, 0, LUO_FEAT_REQUIRED);
	KUNIT_EXPECT_EQ(test, luo_ser_hdr_validate(&out.hdr, NULL, 0),
			-EOPNOTSUPP);
	luo_ser_set_feat(&out.hdr, 0, LUO_FEAT_OPTIONAL);
	KUNIT_EXPECT_EQ(test, luo_ser_hdr_validate(&out.hdr, NULL, 0), 0);
}

static struct kunit_case luo_feature_test_cases[] = {
	KUNIT_CASE(luo_ser_hdr_set_get_test),
	KUNIT_CASE(luo_ser_hdr_truth_table_test),
	{}
};

static struct kunit_suite luo_feature_test_suite = {
	.name = "liveupdate_feature",
	.test_cases = luo_feature_test_cases,
};

kunit_test_suite(luo_feature_test_suite);

MODULE_DESCRIPTION("KUnit tests for the Live Update feature header helpers");
MODULE_LICENSE("GPL");
