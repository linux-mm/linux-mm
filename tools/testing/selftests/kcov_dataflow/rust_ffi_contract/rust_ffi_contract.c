// SPDX-License-Identifier: GPL-2.0
/*
 * rust_ffi_contract.c - Demonstrates kcov_dataflow detecting an FFI
 * contract violation at a function boundary.
 *
 * The pattern: caller passes a struct pointer to callee. Callee's
 * contract says "returns 0 implies out->buffer is valid". A bug in
 * the async path returns 0 but leaves buffer=NULL.
 *
 * kcov_dataflow captures:
 *   [ENTRY] ffi_alloc_buf(alloc={.buffer=NULL, .data_size=0}, 256, 16, 1)
 *   [RET]   ffi_alloc_buf() = 0
 *   [ENTRY] ffi_check_result(alloc={.buffer=NULL, .data_size=0x110, ...})
 *                             ^ proves contract violated
 *   [RET]   ffi_check_result() = -EFAULT
 *
 * Write to /sys/kernel/debug/kcov_dataflow_test/rust_ffi_trigger to run.
 */
#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("FFI contract violation detection via kcov_dataflow");

struct ffi_alloc {
	void *buffer;
	u64 data_size;
	u32 free_async;
	u32 flags;
};

/* Prototypes */
int ffi_alloc_buf(struct ffi_alloc *alloc, u64 data_size,
		  u64 offsets_size, int is_async);
int ffi_check_result(struct ffi_alloc *alloc);

/*
 * Callee with contract: returns 0 implies alloc->buffer is valid.
 * BUG: async path with free_async==0 returns 0 but buffer stays NULL.
 */
noinline int ffi_alloc_buf(struct ffi_alloc *alloc, u64 data_size,
			   u64 offsets_size, int is_async)
{
	/*
	 * data_size + offsets_size is used on every path so that the compiler
	 * keeps offsets_size alive (an unused parameter is dropped at -O2 and
	 * callers then pass poison, leaving nothing to trace).
	 */
	if (!is_async) {
		alloc->buffer = kmalloc(data_size + offsets_size, GFP_KERNEL);
		if (!alloc->buffer)
			return -ENOMEM;
		return 0;
	}
	/* BUG: returns success but buffer is NULL when pool empty */
	if (alloc->free_async == 0) {
		alloc->buffer = NULL;
		alloc->data_size = data_size + offsets_size;
		return 0; /* contract violation */
	}
	alloc->buffer = kmalloc(data_size + offsets_size, GFP_KERNEL);
	alloc->free_async--;
	return 0;
}
EXPORT_SYMBOL(ffi_alloc_buf);

/* Caller that trusts the contract */
noinline int ffi_check_result(struct ffi_alloc *alloc)
{
	if (!alloc->buffer) {
		pr_err("ffi_contract: VIOLATION detected - buffer is NULL after success\n");
		return -EFAULT;
	}
	kfree(alloc->buffer);
	return 0;
}
EXPORT_SYMBOL(ffi_check_result);

static struct dentry *test_dir;

static ssize_t rust_ffi_trigger_write(struct file *f, const char __user *buf,
				 size_t count, loff_t *ppos)
{
	struct ffi_alloc alloc = { .buffer = NULL, .data_size = 0,
				   .free_async = 0, .flags = 0 };
	int ret;

	/*
	 * Keep the initializer: the callee provably writes alloc->buffer before
	 * reading it, so without the barrier the compiler drops the NULL store
	 * and the ENTRY record would show stack garbage instead of NULL.
	 */
	barrier_data(&alloc);

	/* Trigger the bug: is_async=1, free_async=0 */
	ret = ffi_alloc_buf(&alloc, 256, 16, 1);
	pr_info("ffi_contract: ffi_alloc_buf returned %d, buffer=%p\n",
		ret, alloc.buffer);

	if (ret == 0)
		ffi_check_result(&alloc);

	return count;
}

static const struct file_operations rust_ffi_trigger_fops = {
	.write = rust_ffi_trigger_write,
};

static int __init ffi_contract_init(void)
{
	test_dir = debugfs_create_dir("kcov_dataflow_test", NULL);
	debugfs_create_file("rust_ffi_trigger", 0200, test_dir, NULL,
			    &rust_ffi_trigger_fops);
	return 0;
}

static void __exit ffi_contract_exit(void)
{
	debugfs_remove_recursive(test_dir);
}

module_init(ffi_contract_init);
module_exit(ffi_contract_exit);
