// SPDX-License-Identifier: GPL-2.0

#include <linux/mm.h>
#include "internal.h"

static DEFINE_SPINLOCK(reserved_thp_lock);

static unsigned long reserved_thp_cmdline_size __initdata = HPAGE_PMD_SIZE;
static bool reserved_thp_cmdline_size_valid __initdata = true;
static unsigned long reserved_thp_requested __initdata;
static unsigned long reserved_thp_total;
static unsigned long reserved_thp_used;

static int __init setup_reserved_thp_size(char *str)
{
	unsigned long size;
	size = memparse(str, NULL);
	if (size != HPAGE_PMD_SIZE) {
		pr_warn("unsupported thp_reserved_size=%s, only %lu is supported\n",
			str, HPAGE_PMD_SIZE);
		reserved_thp_cmdline_size_valid = false;
		return -EINVAL;
	}
	reserved_thp_cmdline_size = size;
	reserved_thp_cmdline_size_valid = true;
	return 0;
}
early_param("thp_reserved_size", setup_reserved_thp_size);
static int __init setup_reserved_thp_nr(char *str)
{
	int count;
	if (sscanf(str, "%lu%n", &reserved_thp_requested, &count) != 1 ||
	    str[count]) {
		pr_warn("invalid thp_reserved_nr=%s\n", str);
		reserved_thp_requested = 0;
		return -EINVAL;
	}
	return 0;
}
early_param("thp_reserved_nr", setup_reserved_thp_nr);

unsigned long reserved_thp_hpage_nr(unsigned long start, unsigned long end)
{
	return (end - start) >> HPAGE_PMD_SHIFT;
}

int reserved_thp_charge(unsigned long nr_hpages)
{
	int ret = 0;

	if (!nr_hpages)
		return 0;

	spin_lock(&reserved_thp_lock);
	if (nr_hpages > reserved_thp_total - reserved_thp_used)
		ret = -ENOMEM;
	else
		reserved_thp_used += nr_hpages;
	spin_unlock(&reserved_thp_lock);

	return ret;
}

void reserved_thp_uncharge(unsigned long nr_hpages)
{
	if (!nr_hpages)
		return;

	spin_lock(&reserved_thp_lock);
	if (WARN_ON_ONCE(nr_hpages > reserved_thp_used))
		reserved_thp_used = 0;
	else
		reserved_thp_used -= nr_hpages;
	spin_unlock(&reserved_thp_lock);
}

static ssize_t total_hpages_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%lu\n", READ_ONCE(reserved_thp_total));
}
static ssize_t free_hpages_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	unsigned long free_hpages;

	spin_lock(&reserved_thp_lock);
	free_hpages = reserved_thp_total - reserved_thp_used;
	spin_unlock(&reserved_thp_lock);

	return sysfs_emit(buf, "%lu\n", free_hpages);
}
static ssize_t used_hpages_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%lu\n", READ_ONCE(reserved_thp_used));
}

static struct kobj_attribute total_hpages_attr = __ATTR_RO(total_hpages);
static struct kobj_attribute free_hpages_attr = __ATTR_RO(free_hpages);
static struct kobj_attribute used_hpages_attr = __ATTR_RO(used_hpages);

static struct attribute *reserved_thp_attrs[] = {
	&total_hpages_attr.attr,
	&free_hpages_attr.attr,
	&used_hpages_attr.attr,
	NULL,
};

static const struct attribute_group reserved_thp_attr_group = {
	.attrs = reserved_thp_attrs,
};

static int __init reserved_thp_init(void)
{
	struct kobject *kobj;
	int ret;

	if (reserved_thp_requested && reserved_thp_cmdline_size_valid) {
		reserved_thp_total = reserved_thp_pageblocks(reserved_thp_requested);
		pr_info("reserved %lu/%lu PMD THP pageblocks (%lu bytes each)\n",
			reserved_thp_total, reserved_thp_requested,
			reserved_thp_cmdline_size);
	}
	kobj = kobject_create_and_add("reserved_thp", mm_kobj);
	if (!kobj)
		return -ENOMEM;
	ret = sysfs_create_group(kobj, &reserved_thp_attr_group);
	if (ret)
		kobject_put(kobj);
	return ret;
}
subsys_initcall(reserved_thp_init);