// SPDX-License-Identifier: GPL-2.0
/*
 * Kernel Assurance Plane control plane in securityfs.
 *
 *   /sys/kernel/security/kap/
 *     kap.default
 *     igroups/                 link to the root of the internal kapfs mount
 *       <igroup>/              created with mkdir, as a cgroup is
 *         kap.install
 *         kap.procs
 *         kap.igfd
 *         kap.lineage_root
 *         kap.authority_epoch
 *         kap.authority
 *         kap.expires_at
 *         kap.transition
 *         kap.mode
 *         kap.events
 *
 * securityfs has no mkdir, so igroups live in a small internal filesystem that
 * is mounted with kern_mount() and reached through a link under securityfs.
 * AppArmor publishes its policy namespaces the same way.
 */

#define pr_fmt(fmt) "KAP: " fmt

#include <linux/capability.h>
#include <linux/ctype.h>
#include <linux/dcache.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mount.h>
#include <linux/spinlock.h>
#include <linux/namei.h>
#include <linux/security.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>

#include "kap.h"

#define KAPFS_MAGIC 0x4b415046

static struct vfsmount *kapfs_mnt;
static struct dentry *kap_sfs_dir;
static struct dentry *kap_sfs_default;
static struct kap_file_ctx kap_default_ctx = { .kind = KAP_FILE_DEFAULT };

static const char * const kap_file_names[KAP_FILE_COUNT] = {
	[KAP_FILE_PROCS] = "kap.procs",
	[KAP_FILE_IGFD] = "kap.igfd",
	[KAP_FILE_LINEAGE_ROOT] = "kap.lineage_root",
	[KAP_FILE_AUTHORITY_EPOCH] = "kap.authority_epoch",
	[KAP_FILE_AUTHORITY] = "kap.authority",
	[KAP_FILE_TRANSITION] = "kap.transition",
	[KAP_FILE_MODE] = "kap.mode",
	[KAP_FILE_EVENTS] = "kap.events",
	[KAP_FILE_EXPIRES_AT] = "kap.expires_at",
	[KAP_FILE_INSTALL] = "kap.install",
};

static const umode_t kap_file_modes[KAP_FILE_COUNT] = {
	[KAP_FILE_PROCS] = 0600,
	[KAP_FILE_IGFD] = 0400,
	[KAP_FILE_LINEAGE_ROOT] = 0600,
	[KAP_FILE_AUTHORITY_EPOCH] = 0600,
	[KAP_FILE_AUTHORITY] = 0600,
	[KAP_FILE_TRANSITION] = 0600,
	[KAP_FILE_MODE] = 0600,
	[KAP_FILE_EXPIRES_AT] = 0600,
	[KAP_FILE_INSTALL] = 0200,
	[KAP_FILE_EVENTS] = 0400,
};

static int kapfs_show(struct seq_file *m, void *v)
{
	struct kap_file_ctx *ctx = m->private;
	struct kap_igroup *igroup = ctx->igroup;
	struct kap_bound_pid *entry;

	spin_lock(&kap_lock);
	switch (ctx->kind) {
	case KAP_FILE_PROCS:
		list_for_each_entry(entry, &igroup->pids, node)
			seq_printf(m, "%d\n", entry->pid);
		break;
	case KAP_FILE_IGFD:
		seq_printf(m, "igroup=%s epoch=%llu root=%s\n",
			   igroup->name, igroup->epoch, igroup->lineage_root);
		break;
	case KAP_FILE_LINEAGE_ROOT:
		seq_printf(m, "%s\n", igroup->lineage_root);
		break;
	case KAP_FILE_AUTHORITY_EPOCH:
		seq_printf(m, "%llu\n", igroup->epoch);
		break;
	case KAP_FILE_AUTHORITY:
		seq_write(m, igroup->authority, igroup->authority_len);
		if (igroup->authority_len && igroup->authority[igroup->authority_len - 1] != '\n')
			seq_putc(m, '\n');
		break;
	case KAP_FILE_TRANSITION:
		if (igroup->has_sequence)
			seq_printf(m, "last_sequence=%llu\n", igroup->last_sequence);
		break;
	case KAP_FILE_MODE:
		seq_printf(m, "%s\n", igroup->revoked ? "revoked" : "enforce");
		break;
	case KAP_FILE_EVENTS:
		seq_write(m, igroup->events, igroup->events_len);
		break;
	case KAP_FILE_EXPIRES_AT:
		seq_printf(m, "%llu\n", igroup->expires_at);
		break;
	case KAP_FILE_DEFAULT:
		seq_printf(m, "%s\n", kap_default_closed ? "closed" : "open");
		break;
	default:
		break;
	}
	spin_unlock(&kap_lock);
	return 0;
}

static int kapfs_open(struct inode *inode, struct file *file)
{
	return single_open(file, kapfs_show, inode->i_private);
}

static ssize_t kapfs_write(struct file *file, const char __user *buf, size_t len, loff_t *ppos)
{
	struct seq_file *seq = file->private_data;
	struct kap_file_ctx *ctx = seq->private;
	struct kap_igroup *igroup = ctx->igroup;
	struct kap_compiled_authority *compiled = NULL;
	struct kap_bound_pid *entry = NULL;
	struct kap_install_request req;
	u64 value = 0;
	pid_t pid = 0;
	char *tmp;
	int rc = 0;

	/* The opener's credentials, so a handed-over descriptor gains nothing. */
	if (!file_ns_capable(file, &init_user_ns, CAP_SYS_ADMIN))
		return -EPERM;
	if (len == 0)
		return 0;
	if (len > (ctx->kind == KAP_FILE_INSTALL ? KAP_MAX_INSTALL_BYTES
						 : KAP_MAX_PREDICATE_BYTES))
		return -EINVAL;
	if (ctx->kind == KAP_FILE_EVENTS || ctx->kind == KAP_FILE_IGFD)
		return -EINVAL;

	tmp = memdup_user_nul(buf, len);
	if (IS_ERR(tmp))
		return PTR_ERR(tmp);
	kap_trim_newline(tmp);
	if (!tmp[0]) {
		kfree(tmp);
		return len;
	}

	/* Parse, allocate and compile first: kap_lock is a spinlock. */
	switch (ctx->kind) {
	case KAP_FILE_PROCS:
		rc = kstrtoint(tmp, 10, &pid);
		if (!rc) {
			entry = kzalloc_obj(*entry, GFP_KERNEL);
			if (!entry)
				rc = -ENOMEM;
		}
		break;
	case KAP_FILE_LINEAGE_ROOT:
		if (strlen(tmp) > KAP_MAX_ROOT_LEN)
			rc = -EINVAL;
		break;
	case KAP_FILE_INSTALL:
		rc = kap_install_parse(tmp, &req);
		if (!rc)
			rc = kap_authority_prepare(req.authority, &compiled);
		break;
	case KAP_FILE_AUTHORITY:
		rc = kap_authority_prepare(tmp, &compiled);
		break;
	case KAP_FILE_AUTHORITY_EPOCH:
	case KAP_FILE_EXPIRES_AT:
		rc = kstrtou64(tmp, 10, &value);
		break;
	case KAP_FILE_TRANSITION:
		rc = kap_parse_transition_sequence(tmp, &value);
		break;
	case KAP_FILE_MODE:
		if (!sysfs_streq(tmp, "revoked") && !sysfs_streq(tmp, "enforce"))
			rc = -EINVAL;
		break;
	case KAP_FILE_DEFAULT:
		if (!sysfs_streq(tmp, "closed") && !sysfs_streq(tmp, "open"))
			rc = -EINVAL;
		break;
	default:
		rc = -EINVAL;
		break;
	}
	if (rc)
		goto out;

	spin_lock(&kap_lock);
	switch (ctx->kind) {
	case KAP_FILE_PROCS:
		rc = kap_bind_pid_locked(igroup, pid, &entry);
		break;
	case KAP_FILE_LINEAGE_ROOT:
		strscpy(igroup->lineage_root, tmp, sizeof(igroup->lineage_root));
		break;
	case KAP_FILE_INSTALL:
		rc = kap_install_locked(igroup, &req, &compiled);
		break;
	case KAP_FILE_AUTHORITY_EPOCH:
		/* Writing an epoch also clears revoked mode. */
		if (value < igroup->epoch) {
			rc = -ESTALE;
		} else {
			igroup->epoch = value;
			igroup->revoked = false;
		}
		break;
	case KAP_FILE_AUTHORITY: {
		struct kap_compiled_authority *old = igroup->compiled;

		strscpy(igroup->authority, tmp, sizeof(igroup->authority));
		igroup->authority_len = strlen(igroup->authority);
		igroup->compiled = compiled;
		compiled = old;
		break;
	}
	case KAP_FILE_TRANSITION:
		if (igroup->has_sequence && value <= igroup->last_sequence) {
			rc = -EALREADY;
		} else {
			igroup->last_sequence = value;
			igroup->has_sequence = true;
		}
		break;
	case KAP_FILE_EXPIRES_AT:
		/* An expiry already in the past is refused rather than installed. */
		if (value && (u64)ktime_get_real_seconds() >= value)
			rc = -ESTALE;
		else
			igroup->expires_at = value;
		break;
	case KAP_FILE_MODE:
		igroup->revoked = sysfs_streq(tmp, "revoked");
		break;
	case KAP_FILE_DEFAULT:
		WRITE_ONCE(kap_default_closed, sysfs_streq(tmp, "closed"));
		break;
	default:
		break;
	}
	spin_unlock(&kap_lock);
out:
	/* Whatever was not consumed: a refused document, or the replaced one. */
	kvfree(compiled);
	kfree(entry);
	kfree(tmp);
	return rc ? rc : len;
}

static const struct file_operations kapfs_fops = {
	.owner = THIS_MODULE,
	.open = kapfs_open,
	.read = seq_read,
	.write = kapfs_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static struct inode *kapfs_make_inode(struct super_block *sb, umode_t mode)
{
	struct inode *inode = new_inode(sb);

	if (!inode)
		return NULL;
	inode->i_ino = get_next_ino();
	inode->i_mode = mode;
	simple_inode_init_ts(inode);
	return inode;
}

static void kapfs_instantiate(struct dentry *dentry, struct inode *inode)
{
	d_make_persistent(dentry, inode);
}

/* Names end up in kap.events and the kernel log, so keep them plain. */
static bool kapfs_valid_name(const struct qstr *name)
{
	unsigned int i;

	if (name->len == 0 || name->len > KAP_MAX_NAME_LEN || name->name[0] == '.')
		return false;
	for (i = 0; i < name->len; i++) {
		unsigned char c = name->name[i];

		if (!isalnum(c) && c != '-' && c != '_' && c != '.')
			return false;
	}
	return true;
}

/* Returns NULL on success: the passed dentry is the one instantiated. */
static struct dentry *kapfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
				  struct dentry *dentry, umode_t mode)
{
	struct dentry *files[KAP_FILE_COUNT] = {};
	struct inode *inodes[KAP_FILE_COUNT] = {};
	struct kap_igroup *igroup;
	struct inode *inode;
	int rc = 0;
	int i;

	if (!capable(CAP_SYS_ADMIN))
		return ERR_PTR(-EPERM);
	if (!kapfs_valid_name(&dentry->d_name))
		return ERR_PTR(-EINVAL);

	/* Allocate everything first, so a failure leaves nothing behind. */
	igroup = kvzalloc_obj(*igroup, GFP_KERNEL);
	inode = kapfs_make_inode(dir->i_sb, S_IFDIR | 0700);
	if (!igroup || !inode) {
		rc = -ENOMEM;
		goto err;
	}
	memcpy(igroup->name, dentry->d_name.name, dentry->d_name.len);
	INIT_LIST_HEAD(&igroup->pids);
	INIT_LIST_HEAD(&igroup->node);
	for (i = 0; i < KAP_FILE_COUNT; i++) {
		files[i] = d_alloc_name(dentry, kap_file_names[i]);
		inodes[i] = kapfs_make_inode(dir->i_sb, S_IFREG | kap_file_modes[i]);
		if (!files[i] || !inodes[i]) {
			rc = -ENOMEM;
			goto err;
		}
		igroup->files[i].kind = i;
		igroup->files[i].igroup = igroup;
		inodes[i]->i_fop = &kapfs_fops;
		inodes[i]->i_private = &igroup->files[i];
	}

	spin_lock(&kap_lock);
	if (kap_find_igroup_locked(igroup->name))
		rc = -EEXIST;
	else
		list_add_tail(&igroup->node, &kap_igroups);
	spin_unlock(&kap_lock);
	if (rc)
		goto err;

	inode->i_op = &simple_dir_inode_operations;
	inode->i_fop = &simple_dir_operations;
	inc_nlink(inode);
	inc_nlink(dir);
	/* Lookups in the new directory wait until its files are all there. */
	inode_lock(inode);
	kapfs_instantiate(dentry, inode);
	igroup->dentry = dentry;
	for (i = 0; i < KAP_FILE_COUNT; i++)
		kapfs_instantiate(files[i], inodes[i]);
	inode_unlock(inode);
	return NULL;

err:
	for (i = 0; i < KAP_FILE_COUNT; i++) {
		if (inodes[i])
			iput(inodes[i]);
		if (files[i])
			dput(files[i]);
	}
	if (inode)
		iput(inode);
	kvfree(igroup);
	return ERR_PTR(rc);
}

static const struct inode_operations kapfs_root_inode_ops = {
	.lookup = simple_lookup,
	.mkdir = kapfs_mkdir,
};

static int kapfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct inode *inode;

	sb->s_magic = KAPFS_MAGIC;
	inode = kapfs_make_inode(sb, S_IFDIR | 0700);
	if (!inode)
		return -ENOMEM;
	inode->i_op = &kapfs_root_inode_ops;
	inode->i_fop = &simple_dir_operations;
	inc_nlink(inode);
	sb->s_root = d_make_root(inode);
	if (!sb->s_root)
		return -ENOMEM;
	return 0;
}

static int kapfs_get_tree(struct fs_context *fc)
{
	return get_tree_single(fc, kapfs_fill_super);
}

static const struct fs_context_operations kapfs_context_ops = {
	.get_tree = kapfs_get_tree,
};

static int kapfs_init_fs_context(struct fs_context *fc)
{
	fc->ops = &kapfs_context_ops;
	return 0;
}

/* Never registered, so it cannot be mounted from userspace. */
static struct file_system_type kapfs_type = {
	.owner = THIS_MODULE,
	.name = "kapfs",
	.init_fs_context = kapfs_init_fs_context,
	.kill_sb = kill_anon_super,
};

static const char *kap_igroups_get_link(struct dentry *dentry,
					struct inode *inode,
					struct delayed_call *done)
{
	struct path path;
	int rc;

	if (!dentry)
		return ERR_PTR(-ECHILD);
	path.mnt = mntget(kapfs_mnt);
	path.dentry = dget(kapfs_mnt->mnt_root);
	rc = nd_jump_link(&path);
	return ERR_PTR(rc);
}

static int kap_igroups_readlink(struct dentry *dentry, char __user *buffer,
				int buflen)
{
	char name[32];
	int rc;

	rc = snprintf(name, sizeof(name), "kapfs:[%llu]",
		      (unsigned long long)d_inode(kapfs_mnt->mnt_root)->i_ino);
	if (rc <= 0 || rc >= sizeof(name))
		return -ENOENT;
	return readlink_copy(buffer, buflen, name, rc);
}

static const struct inode_operations kap_igroups_link_iops = {
	.readlink = kap_igroups_readlink,
	.get_link = kap_igroups_get_link,
};

int __init kap_securityfs_init(void)
{
	struct dentry *dent;
	int rc;

	if (!READ_ONCE(kap_lsm_active))
		return 0;

	kapfs_mnt = kern_mount(&kapfs_type);
	if (IS_ERR(kapfs_mnt)) {
		rc = PTR_ERR(kapfs_mnt);
		kapfs_mnt = NULL;
		goto err;
	}
	kapfs_mnt->mnt_sb->s_flags &= ~SB_NOUSER;

	dent = securityfs_create_dir("kap", NULL);
	if (IS_ERR(dent)) {
		rc = PTR_ERR(dent);
		goto err_unmount;
	}
	kap_sfs_dir = dent;

	dent = securityfs_create_file("kap.default", 0600, kap_sfs_dir,
				      &kap_default_ctx, &kapfs_fops);
	if (IS_ERR(dent)) {
		rc = PTR_ERR(dent);
		goto err_remove;
	}
	kap_sfs_default = dent;

	dent = securityfs_create_symlink("igroups", kap_sfs_dir, NULL,
					 &kap_igroups_link_iops);
	if (IS_ERR(dent)) {
		rc = PTR_ERR(dent);
		goto err_remove;
	}

	WRITE_ONCE(kap_ready, true);
	pr_info("control plane at /sys/kernel/security/kap\n");
	return 0;

err_remove:
	securityfs_remove(kap_sfs_default);
	securityfs_remove(kap_sfs_dir);
	kap_sfs_default = NULL;
	kap_sfs_dir = NULL;
err_unmount:
	kern_unmount(kapfs_mnt);
	kapfs_mnt = NULL;
err:
	pr_err("could not create the securityfs control plane: %d\n", rc);
	return rc;
}
