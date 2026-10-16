// SPDX-License-Identifier: GPL-2.0-only

#include <linux/mm.h>
#include <linux/cma.h>
#include <linux/compiler.h>
#include <linux/cpuset.h>
#include <linux/mm_inline.h>

#include <asm/page.h>
#include <asm/setup.h>

#include <linux/hugetlb.h>
#include <linux/memblock.h>
#include <linux/math.h>
#include <linux/math64.h>
#include "internal.h"
#include "hugetlb_cma.h"

struct hugetlb_cma_pool {
	struct cma *cma[MAX_NUMNODES];
	unsigned long size;
	unsigned int order;
};

struct hugetlb_cma_config {
	unsigned long size_in_node[MAX_NUMNODES];
	unsigned int percent;
	unsigned int percent_in_node[MAX_NUMNODES];
};

static struct hugetlb_cma_pool hugetlb_cma[HUGE_MAX_HSTATE] __ro_after_init;
static struct hugetlb_cma_config hugetlb_cma_config[HUGE_MAX_HSTATE] __initdata;
static unsigned int hugetlb_cma_nr __ro_after_init;
static unsigned int hugetlb_cma_default_order __ro_after_init;

static bool hugetlb_cma_only __ro_after_init;

static char hugetlb_cma_cmdline[COMMAND_LINE_SIZE] __initdata;
static unsigned int hugetlb_cma_cmdline_len __initdata;

static struct hugetlb_cma_pool *hugetlb_cma_pool(unsigned int order)
{
	unsigned int i;

	/* PMD pages use their own pool; other gigantic sizes share. */
	if (order != PMD_SHIFT - PAGE_SHIFT && order_is_gigantic(order))
		order = hugetlb_cma_default_order;

	for (i = 0; i < hugetlb_cma_nr; i++) {
		if (hugetlb_cma[i].order == order)
			return &hugetlb_cma[i];
	}
	return NULL;
}

#ifdef CONFIG_NUMA
static phys_addr_t __init memblock_node_memory_size(int nid)
{
	struct memblock_region *reg;
	phys_addr_t size = 0;

	for_each_mem_region(reg) {
		if (reg->nid == nid)
			size += reg->size;
	}
	return size;
}
#else
static phys_addr_t __init memblock_node_memory_size(int nid)
{
	return memblock_phys_mem_size();
}
#endif

void hugetlb_cma_free_frozen_folio(struct folio *folio)
{
	unsigned int i;
	int nid = folio_nid(folio);
	unsigned long nr_pages = folio_nr_pages(folio);

	for (i = 0; i < hugetlb_cma_nr; i++) {
		if (cma_release_frozen(hugetlb_cma[i].cma[nid], &folio->page, nr_pages))
			return;
	}

	WARN_ON_ONCE(1);
}

struct folio *hugetlb_cma_alloc_frozen_folio(int order, gfp_t gfp_mask,
		int nid, nodemask_t *nodemask)
{
	int node;
	struct folio *folio;
	struct page *page = NULL;
	const nodemask_t *nmask;
	unsigned int cpuset_mems_cookie;
	struct hugetlb_cma_pool *pool = hugetlb_cma_pool(order);

	if (!pool || !pool->size)
		return NULL;

retry_cpuset:
	if (!nodemask) {
		cpuset_mems_cookie = read_mems_allowed_begin();
		nmask = &cpuset_current_mems_allowed;
	} else {
		nmask = nodemask;
	}

	if (pool->cma[nid] && node_isset(nid, *nmask))
		page = cma_alloc_frozen_compound(pool->cma[nid], order);

	if (!page && !(gfp_mask & __GFP_THISNODE)) {
		for_each_node_mask(node, *nmask) {
			if (node == nid || !pool->cma[node])
				continue;

			page = cma_alloc_frozen_compound(pool->cma[node], order);
			if (page)
				break;
		}
	}

	if (!page) {
		if (!nodemask &&
		    unlikely(read_mems_allowed_retry(cpuset_mems_cookie)))
			goto retry_cpuset;
		return NULL;
	}

	folio = page_folio(page);
	folio_set_hugetlb_cma(folio);
	return folio;
}

void * __init hugetlb_cma_alloc_bootmem(struct hstate *h, int nid, bool node_exact)
{
	struct hugetlb_cma_pool *pool = hugetlb_cma_pool(huge_page_order(h));
	struct cma *cma;
	void *m;
	int node;

	cma = pool->cma[nid];
	m = cma_reserve_early(cma, huge_page_size(h));
	if (m || node_exact)
		return m;

	for_each_node_mask(node, hugetlb_bootmem_nodes) {
		cma = pool->cma[node];
		if (!cma || node == nid)
			continue;
		m = cma_reserve_early(cma, huge_page_size(h));
		if (m)
			return m;
	}

	return NULL;
}

static int __init hugetlb_cma_parse(char *p)
{
	struct hugetlb_cma_pool *pool;
	struct hugetlb_cma_config *config;
	unsigned int order = hugetlb_cma_default_order;
	unsigned int i;
	int nid, count = 0;
	unsigned long tmp;
	char *s = p;
	char *suffix = strchr(p, '@');

	if (suffix) {
		unsigned long long page_size;
		char *end;

		page_size = memparse(suffix + 1, &end);
		if (suffix == p || !page_size || *end) {
			pr_warn("hugetlb_cma: invalid parameter %s\n", p);
			return -EINVAL;
		}
		if ((page_size != PMD_SIZE &&
		     page_size != (PAGE_SIZE << order)) ||
		    !arch_hugetlb_valid_size(page_size)) {
			pr_warn("hugetlb_cma: unsupported page size in %s\n", p);
			return -EINVAL;
		}
		order = ilog2(page_size) - PAGE_SHIFT;
	}

	if (!order) {
		pr_warn("hugetlb_cma: the option isn't supported by current arch\n");
		return -EINVAL;
	}

	for (i = 0; i < hugetlb_cma_nr; i++) {
		if (hugetlb_cma[i].order == order)
			break;
	}

	if (i == ARRAY_SIZE(hugetlb_cma))
		return -EINVAL;

	pool = &hugetlb_cma[i];
	if (i == hugetlb_cma_nr) {
		hugetlb_cma_nr++;
		pool->order = order;
	}
	config = &hugetlb_cma_config[i];

	while (*s) {
		if (sscanf(s, "%lu%n", &tmp, &count) != 1)
			break;

		if (s[count] == ':') {
			char *next;

			if (tmp >= MAX_NUMNODES)
				break;
			nid = array_index_nospec(tmp, MAX_NUMNODES);

			pool->size = 0;
			config->percent = 0;

			s += count + 1;
			tmp = memparse(s, &next);
			if (*next == '%') {
				if (tmp > 100) {
					pr_warn("hugetlb_cma: invalid percentage %lu for node %d\n",
						tmp, nid);
					break;
				}
				config->percent_in_node[nid] = tmp;
				config->size_in_node[nid] = 0;
				s = next + 1;
			} else {
				config->size_in_node[nid] = tmp;
				config->percent_in_node[nid] = 0;
				s = next;
			}

			/*
			 * Skip the separator if have one, otherwise
			 * break the parsing.
			 */
			if (*s == ',')
				s++;
			else
				break;
		} else {
			char *next;

			tmp = memparse(p, &next);
			if (*next == '%') {
				if (tmp > 100) {
					pr_warn("hugetlb_cma: invalid percentage %lu\n", tmp);
				} else {
					config->percent = tmp;
					pool->size = 0;
					for (nid = 0; nid < MAX_NUMNODES; nid++) {
						config->size_in_node[nid] = 0;
						config->percent_in_node[nid] = 0;
					}
				}
			} else {
				pool->size = tmp;
				config->percent = 0;
				for (nid = 0; nid < MAX_NUMNODES; nid++) {
					config->size_in_node[nid] = 0;
					config->percent_in_node[nid] = 0;
				}
			}
			break;
		}
	}

	return 0;
}

static int __init cmdline_parse_hugetlb_cma(char *p)
{
	size_t len;

	if (!p)
		return -EINVAL;
	len = strlen(p) + 1;
	if (len > sizeof(hugetlb_cma_cmdline) - hugetlb_cma_cmdline_len)
		return -EINVAL;
	memcpy(hugetlb_cma_cmdline + hugetlb_cma_cmdline_len, p, len);
	hugetlb_cma_cmdline_len += len;

	return 0;
}
early_param("hugetlb_cma", cmdline_parse_hugetlb_cma);

static int __init cmdline_parse_hugetlb_cma_only(char *p)
{
	return kstrtobool(p, &hugetlb_cma_only);
}

early_param("hugetlb_cma_only", cmdline_parse_hugetlb_cma_only);

unsigned int __weak arch_hugetlb_cma_order(void)
{
	return 0;
}

static void __init hugetlb_cma_reserve_pool(struct hugetlb_cma_pool *pool,
					    struct hugetlb_cma_config *config)
{
	unsigned long size, reserved, per_node, hugepage_size;
	unsigned int order = pool->order;
	bool node_specific_cma_alloc = false;
	bool has_node_specific_param = false;
	int nid;

	for (nid = 0; nid < MAX_NUMNODES; nid++) {
		if (config->size_in_node[nid] || config->percent_in_node[nid]) {
			has_node_specific_param = true;
			break;
		}
	}

	if (has_node_specific_param) {
		pool->size = 0;
		for (nid = 0; nid < MAX_NUMNODES; nid++) {
			if (config->percent_in_node[nid]) {
				phys_addr_t node_gfp_mem = memblock_node_memory_size(nid);
				u64 s;

				s = mul_u64_u32_div((u64)node_gfp_mem,
						    config->percent_in_node[nid],
						    100);

				config->size_in_node[nid] = s;
			}
			pool->size += config->size_in_node[nid];
		}
	} else if (config->percent) {
		pool->size = mul_u64_u32_div((u64)memblock_phys_mem_size(),
						   config->percent, 100);
	}

	if (!pool->size)
		return;

	hugepage_size = max(PAGE_SIZE << order, CMA_MIN_ALIGNMENT_BYTES);

	if (config->percent) {
		unsigned long orig_size = pool->size;

		pool->size = ALIGN_DOWN(pool->size, hugepage_size);
		if (orig_size && !pool->size)
			pr_warn("hugetlb_cma: reservation size rounded down to 0 from %lu MiB (%u%%)\n",
				orig_size / SZ_1M, config->percent);
	} else if (has_node_specific_param) {
		pool->size = 0;
		for (nid = 0; nid < MAX_NUMNODES; nid++) {
			if (config->percent_in_node[nid]) {
				unsigned long orig_size = config->size_in_node[nid];

				config->size_in_node[nid] =
					ALIGN_DOWN(config->size_in_node[nid],
						   hugepage_size);
				if (orig_size && !config->size_in_node[nid])
					pr_warn("hugetlb_cma: reservation size rounded down to 0 from %lu MiB (%u%%) on node %d\n",
						orig_size / SZ_1M,
						config->percent_in_node[nid],
						nid);
			}
			pool->size += config->size_in_node[nid];
		}
	}

	hugetlb_bootmem_set_nodes();

	for (nid = 0; nid < MAX_NUMNODES; nid++) {
		size = config->size_in_node[nid];
		if (size == 0)
			continue;

		if (!node_isset(nid, hugetlb_bootmem_nodes)) {
			pr_warn("hugetlb_cma: invalid node %d specified\n", nid);
		} else if (!IS_ALIGNED(size, hugepage_size)) {
			pr_warn("hugetlb_cma: cma area of node %d must be a multiple of %lu MiB\n",
				nid, hugepage_size / SZ_1M);
		} else {
			node_specific_cma_alloc = true;
			continue;
		}

		pool->size -= size;
		config->size_in_node[nid] = 0;
	}

	/* Validate the CMA size again in case some invalid nodes specified. */
	if (!pool->size)
		return;

	if (!IS_ALIGNED(pool->size, hugepage_size)) {
		pr_warn("hugetlb_cma: cma area must be a multiple of %lu MiB\n",
			hugepage_size / SZ_1M);
		pool->size = 0;
		return;
	}

	if (!node_specific_cma_alloc) {
		/*
		 * If 3 GB area is requested on a machine with 4 numa nodes,
		 * let's allocate 1 GB on first three nodes and ignore the last one.
		 */
		per_node = DIV_ROUND_UP(pool->size,
					nodes_weight(hugetlb_bootmem_nodes));
		per_node = round_up(per_node, hugepage_size);
		if (config->percent)
			pr_info("hugetlb_cma: reserve %lu MiB (%u%%), up to %lu MiB per node\n",
				pool->size / SZ_1M, config->percent,
				per_node / SZ_1M);
		else
			pr_info("hugetlb_cma: reserve %lu MiB, up to %lu MiB per node\n",
				pool->size / SZ_1M, per_node / SZ_1M);
	}

	reserved = 0;
	for_each_node_mask(nid, hugetlb_bootmem_nodes) {
		int res;
		char name[CMA_MAX_NAME];

		if (node_specific_cma_alloc) {
			if (config->size_in_node[nid] == 0)
				continue;

			size = config->size_in_node[nid];
		} else {
			size = min(per_node, pool->size - reserved);
		}

		if (order == hugetlb_cma_default_order)
			snprintf(name, sizeof(name), "hugetlb%d", nid);
		else
			snprintf(name, sizeof(name), "hugetlb-%lukB-%d",
				 (PAGE_SIZE << order) / SZ_1K, nid);

		/*
		 * Note that 'order per bit' is based on smallest size that
		 * may be returned to CMA allocator in the case of
		 * huge page demotion.
		 */
		res = cma_declare_contiguous_multi(size, hugepage_size,
					min_t(unsigned int, order, HUGETLB_PAGE_ORDER), name,
					&pool->cma[nid], nid);
		if (res || !cma_validate_zones(pool->cma[nid])) {
			pr_warn("hugetlb_cma: %s: err %d, node %d\n",
				res ? "reservation failed" : "reserved area spans zones",
				res, nid);
			pool->cma[nid] = NULL;
			continue;
		}

		reserved += size;
		if (config->percent_in_node[nid])
			pr_info("hugetlb_cma: reserved %lu MiB (%u%%) on node %d\n",
				size / SZ_1M, config->percent_in_node[nid], nid);
		else
			pr_info("hugetlb_cma: reserved %lu MiB on node %d\n",
				size / SZ_1M, nid);

		if (reserved >= pool->size)
			break;
	}

	/* Only successfully reserved areas enable CMA allocation for this size. */
	pool->size = reserved;
}

void __init hugetlb_cma_reserve(void)
{
	unsigned int i;
	char *p;

	hugetlb_cma_default_order = arch_hugetlb_cma_order();

	for (p = hugetlb_cma_cmdline;
	     p < hugetlb_cma_cmdline + hugetlb_cma_cmdline_len;
	     p += strlen(p) + 1)
		hugetlb_cma_parse(p);

	for (i = 0; i < hugetlb_cma_nr; i++)
		hugetlb_cma_reserve_pool(&hugetlb_cma[i], &hugetlb_cma_config[i]);
}

bool hugetlb_cma_exclusive_alloc(unsigned int order)
{
	struct hugetlb_cma_pool *pool = hugetlb_cma_pool(order);

	return hugetlb_cma_only && pool && pool->size;
}

unsigned long __init hugetlb_cma_size(struct hstate *h)
{
	struct hugetlb_cma_pool *pool = hugetlb_cma_pool(huge_page_order(h));

	return pool ? pool->size : 0;
}

unsigned long __init hugetlb_cma_total_size(void)
{
	unsigned long size = 0;
	unsigned int i;

	for (i = 0; i < hugetlb_cma_nr; i++)
		size += hugetlb_cma[i].size;

	return size;
}

bool __init hugetlb_early_cma(struct hstate *h)
{
	if (arch_has_huge_bootmem_alloc())
		return false;

	return hstate_is_gigantic(h) && hugetlb_cma_exclusive_alloc(huge_page_order(h));
}
