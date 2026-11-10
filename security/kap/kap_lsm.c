// SPDX-License-Identifier: GPL-2.0
/*
 * Kernel Assurance Plane LSM prototype.
 *
 * This file wires KAP into the LSM hooks. igroup state and decisions are in
 * igroup.c, and the securityfs control plane that installs authority is in
 * kapfs.c.
 */

#define pr_fmt(fmt) "KAP: " fmt

#include <linux/binfmts.h>
#include <linux/cred.h>
#include <linux/bpf.h>
#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/kap.h>
#include <linux/limits.h>
#include <linux/lsm_hooks.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/mount.h>
#include <linux/nsproxy.h>
#include <linux/sched.h>
#include <linux/security.h>
#include <linux/slab.h>
#include <linux/socket.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <net/sock.h>

#include "kap.h"

#define KAP_LSM_TARGET_LEN 256

struct lsm_blob_sizes kap_blob_sizes __ro_after_init = {
	.lbs_task = sizeof(struct kap_task_state),
};

bool kap_lsm_active;
struct io_uring_cmd;

static char *kap_lsm_file_path_alloc(struct file *file);
static int kap_lsm_path_target(const struct path *path, char *target, size_t target_len);
static bool kap_path_is_memfd(const char *path);
static bool kap_path_is_anonymous_channel(const char *path);
static bool kap_lsm_current_active(void);

struct kap_task_state *kap_task_state(struct task_struct *task)
{
	if (!READ_ONCE(kap_lsm_active) || !task || !task->security)
		return NULL;
	return (struct kap_task_state *)((char *)task->security + kap_blob_sizes.lbs_task);
}

static int kap_task_alloc(struct task_struct *task, u64 clone_flags)
{
	struct kap_task_state *old = kap_task_state(current);
	struct kap_task_state *new = kap_task_state(task);
	int rc;

	if (!new)
		return 0;

	rc = kap_check_namespace_flags(clone_flags, "clone");
	if (rc)
		return rc;

	memset(new, 0, sizeof(*new));
	/* Under kap_lock, so a bind running now is copied whole or not at all. */
	spin_lock(&kap_lock);
	if (old && old->active)
		*new = *old;
	spin_unlock(&kap_lock);
	return 0;
}

static void kap_task_free(struct task_struct *task)
{
	struct kap_task_state *state = kap_task_state(task);

	if (state)
		memset(state, 0, sizeof(*state));
}

static bool kap_lsm_current_active(void)
{
	struct kap_task_state *state = kap_task_state(current);

	return state && state->active;
}

static int kap_bprm_check_security(struct linux_binprm *bprm)
{
	char *target;

	/*
	 * An unbound task is skipped everywhere else, but under
	 * kap.default = closed an unbound exec is the one thing that must not
	 * proceed.
	 */
	if (!kap_lsm_current_active() && !kap_fail_closed_kind(KAP_HOOK_EXEC))
		return 0;
	if (bprm->file) {
		target = kap_lsm_file_path_alloc(bprm->file);
		if (!IS_ERR(target)) {
			if (kap_path_is_memfd(target)) {
				int rc = kap_check_current_target_emit(KAP_HOOK_EXECMEM, target,
								       "memfd_exec_predicate_denied",
								       "memfd_exec_allowed");
				kfree(target);
				return rc;
			}
			kfree(target);
		}
	}
	return kap_check_current_emit(KAP_HOOK_EXEC, bprm,
				      "exec_predicate_denied", "exec_allowed");
}

/*
 * Resolves a file to a PATH_MAX buffer the caller must kfree, or an ERR_PTR.
 * The buffer is allocated rather than kept on the stack: these hooks run deep
 * in syscall paths and a 4K frame does not belong on a 16K kernel stack.
 */
static char *kap_lsm_file_path_alloc(struct file *file)
{
	char *buf;
	char *path;

	if (!file)
		return ERR_PTR(-EINVAL);
	buf = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!buf)
		return ERR_PTR(-ENOMEM);
	path = d_path(&file->f_path, buf, PATH_MAX);
	if (IS_ERR(path)) {
		kfree(buf);
		return ERR_CAST(path);
	}
	/* d_path builds backwards from the end, so move the result to the front
	 * and hand back a single pointer.
	 */
	memmove(buf, path, strlen(path) + 1);
	return buf;
}

static int kap_lsm_path_target(const struct path *path, char *target, size_t target_len)
{
	char *path_buf;
	char *rendered;
	int rc = 0;

	if (!path)
		return -EINVAL;
	path_buf = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!path_buf)
		return -ENOMEM;
	rendered = d_path(path, path_buf, PATH_MAX);
	if (IS_ERR(rendered))
		rc = PTR_ERR(rendered);
	else
		strscpy(target, rendered, target_len);
	kfree(path_buf);
	return rc;
}

static bool kap_path_is_procfs(const char *path)
{
	return !strcmp(path, "/proc") || !strncmp(path, "/proc/", strlen("/proc/"));
}

/*
 * Rewrites /proc/<pid>/... to /proc/self/... in place. target is a PATH_MAX
 * buffer from kap_lsm_file_path_alloc. Done in place rather than through a
 * second PATH_MAX buffer, which would not fit on the stack.
 */
static void kap_normalize_procfs_self(char *target)
{
	static const char self[] = "/proc/self";
	const size_t self_len = sizeof(self) - 1;
	char prefix[32];
	size_t prefix_len;
	size_t tail_len;

	prefix_len = scnprintf(prefix, sizeof(prefix), "/proc/%d", task_pid_nr(current));
	if (strncmp(target, prefix, prefix_len))
		return;
	if (target[prefix_len] != '\0' && target[prefix_len] != '/')
		return;
	tail_len = strlen(target + prefix_len);
	if (self_len + tail_len >= PATH_MAX)
		return;
	memmove(target + self_len, target + prefix_len, tail_len + 1);
	memcpy(target, self, self_len);
}

static bool kap_path_is_memfd(const char *path)
{
	return strstr(path, "/memfd:") || strstr(path, "memfd:");
}

static bool kap_path_is_anonymous_channel(const char *path)
{
	return !strncmp(path, "pipe:", strlen("pipe:")) ||
	       !strncmp(path, "socket:", strlen("socket:")) ||
	       !strncmp(path, "anon_inode:", strlen("anon_inode:"));
}

static int kap_file_open(struct file *file)
{
	char *target;

	if (!kap_lsm_current_active())
		return 0;
	target = kap_lsm_file_path_alloc(file);
	if (!IS_ERR(target)) {
		if (kap_path_is_procfs(target)) {
			int rc;

			kap_normalize_procfs_self(target);
			rc = kap_check_current_target_emit(KAP_HOOK_PROCFS, target,
							   "procfs_predicate_denied",
							   "procfs_allowed");
			kfree(target);
			return rc;
		}
		kfree(target);
	}
	return kap_check_current_emit(KAP_HOOK_FILE, file,
				      "file_predicate_denied", "file_allowed");
}

static int kap_file_permission(struct file *file, int mask)
{
	const char *why = NULL;
	char *target;
	int rc;

	if (!kap_lsm_current_active())
		return 0;
	if (!(mask & (MAY_READ | MAY_WRITE | MAY_APPEND | MAY_EXEC)))
		return 0;
	target = kap_lsm_file_path_alloc(file);
	if (IS_ERR(target))
		return kap_check_current_emit(KAP_HOOK_FILE, file,
					      "file_use_predicate_denied",
					      "file_use_allowed");
	if (kap_path_is_anonymous_channel(target)) {
		kfree(target);
		return 0;
	}
	if (kap_path_is_procfs(target)) {
		kap_normalize_procfs_self(target);
		rc = kap_check_current_target_why(KAP_HOOK_PROCFS, target, &why);
		if (rc)
			kap_emit_current_event_target(KAP_HOOK_PROCFS, 0,
						      why ?: "procfs_use_predicate_denied", target);
	} else {
		rc = kap_check_current_target_why(KAP_HOOK_FILE, target, &why);
		if (rc)
			kap_emit_current_event_target(KAP_HOOK_FILE, 0,
						      why ?: "file_use_predicate_denied", target);
	}
	kfree(target);
	return rc;
}

static int kap_mmap_file(struct file *file, unsigned long reqprot,
			 unsigned long prot, unsigned long flags)
{
	char *target;
	int rc = 0;

	if (!kap_lsm_current_active())
		return 0;
	if (!(reqprot & PROT_EXEC) && !(prot & PROT_EXEC))
		return 0;
	if (!file)
		return kap_check_current_target_emit(KAP_HOOK_EXECMEM, "anonymous",
						     "execmem_predicate_denied",
						     "execmem_allowed");
	target = kap_lsm_file_path_alloc(file);
	if (IS_ERR(target))
		return 0;
	if (kap_path_is_memfd(target))
		rc = kap_check_current_target_emit(KAP_HOOK_EXECMEM, target,
						   "execmem_predicate_denied",
						   "execmem_allowed");
	kfree(target);
	return rc;
}

static int kap_file_mprotect(struct vm_area_struct *vma,
			     unsigned long reqprot, unsigned long prot)
{
	char *target;
	struct file *file;
	int rc = 0;

	if (!kap_lsm_current_active())
		return 0;
	if (!(reqprot & PROT_EXEC) && !(prot & PROT_EXEC))
		return 0;
	file = vma ? vma->vm_file : NULL;
	if (!file)
		return kap_check_current_target_emit(KAP_HOOK_EXECMEM, "anonymous",
						     "execmem_predicate_denied",
						     "execmem_allowed");
	target = kap_lsm_file_path_alloc(file);
	if (IS_ERR(target))
		return 0;
	if (kap_path_is_memfd(target))
		rc = kap_check_current_target_emit(KAP_HOOK_EXECMEM, target,
						   "execmem_predicate_denied",
						   "execmem_allowed");
	kfree(target);
	return rc;
}

#ifdef CONFIG_SECURITY_NETWORK
static int kap_socket_connect(struct socket *sock, struct sockaddr *address, int addrlen)
{
	/*
	 * AF_UNSPEC dissolves an association rather than making one; resolvers
	 * use it between candidate addresses. AF_UNIX is checked as ipc by the
	 * unix hooks.
	 */
	if (addrlen < offsetofend(struct sockaddr, sa_family) ||
	    address->sa_family == AF_UNSPEC || address->sa_family == AF_UNIX)
		return 0;
	return kap_check_current_emit(KAP_HOOK_SOCKET_CONNECT, address,
				      "net_predicate_denied", "net_allowed");
}
#endif

static int kap_check_static_target(enum kap_hook_kind kind, const char *target,
				   const char *deny_reason, const char *allow_reason)
{
	return kap_check_current_target_emit(kind, target, deny_reason, allow_reason);
}

static int kap_check_namespace_target(const char *ns, const char *operation)
{
	char target[64];

	scnprintf(target, sizeof(target), "%s:%s", ns, operation);
	return kap_check_static_target(KAP_HOOK_NAMESPACE, target,
				       "namespace_predicate_denied", "namespace_allowed");
}

static const struct {
	unsigned long flag;
	const char *name;
} kap_namespaces[] = {
	{ CLONE_NEWNS, "mount" },
	{ CLONE_NEWPID, "pid" },
	{ CLONE_NEWNET, "net" },
	{ CLONE_NEWUSER, "user" },
	{ CLONE_NEWUTS, "uts" },
	{ CLONE_NEWIPC, "ipc" },
	{ CLONE_NEWCGROUP, "cgroup" },
	{ CLONE_NEWTIME, "time" },
};

int kap_check_namespace_flags(unsigned long flags, const char *operation)
{
	int rc;
	int i;

	if (!operation || !*operation)
		operation = "transition";
	for (i = 0; i < ARRAY_SIZE(kap_namespaces); i++) {
		if (!(flags & kap_namespaces[i].flag))
			continue;
		/* Creating a user namespace is checked by userns_create. */
		if (kap_namespaces[i].flag == CLONE_NEWUSER &&
		    strcmp(operation, "setns"))
			continue;
		rc = kap_check_namespace_target(kap_namespaces[i].name, operation);
		if (rc)
			return rc;
	}
	return 0;
}

/* A path we cannot render is a path we cannot judge. Emitting it as a normal
 * target lets a broad allow such as "*" match the placeholder and pass, so
 * refuse outright and record why.
 */
static int kap_lsm_refuse_unresolved(const char *why)
{
	kap_emit_current_event_target(KAP_HOOK_FILE, 0, why, NULL);
	return -EACCES;
}

/*
 * The path family (rename, link, unlink, mkdir, mknod, symlink) is checked
 * like open, so a bound task cannot stage a file where it may write and move
 * it into a denied directory. Render the entry as its parent directory plus
 * its name and run it through the same file predicate as open, so one place
 * decides and one reason string comes out.
 *
 * Rendering happens into a PATH_MAX buffer rather than KAP_LSM_TARGET_LEN. A
 * truncated path stops matching a deny prefix that extends past the cut, which
 * would turn a long path into a silent allow. Truncation and resolution
 * failure are both refused instead.
 */
static int kap_lsm_check_dir_entry(const struct path *dir, struct dentry *dentry)
{
	char *buf;
	char *rendered;
	char *target;
	int rc;
	int len;

	/* kap_path_link builds old_path by hand, so either field can be NULL.
	 * d_path would dereference them.
	 */
	if (!dir || !dir->mnt || !dir->dentry || !dentry)
		return kap_lsm_refuse_unresolved("file_path_unresolved");
	buf = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	target = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!target) {
		kfree(buf);
		return -ENOMEM;
	}
	rendered = d_path(dir, buf, PATH_MAX);
	if (IS_ERR(rendered)) {
		/* No resolvable path means no way to know whether it is denied.
		 * Refuse rather than let an unresolved target meet a broad
		 * allow and pass.
		 */
		kfree(target);
		kfree(buf);
		return kap_lsm_refuse_unresolved("file_path_unresolved");
	}
	/* d_path renders the root as "/", so joining blindly gives "//name". */
	if (!strcmp(rendered, "/"))
		len = snprintf(target, PATH_MAX, "/%pd", dentry);
	else
		len = snprintf(target, PATH_MAX, "%s/%pd", rendered, dentry);
	if (len < 0 || len >= PATH_MAX) {
		kfree(target);
		kfree(buf);
		return kap_lsm_refuse_unresolved("file_path_truncated");
	}
	rc = kap_check_current_target_emit(KAP_HOOK_FILE, target,
					   "file_predicate_denied",
					   "file_allowed");
	kfree(target);
	kfree(buf);
	return rc;
}

static int kap_path_rename(const struct path *old_dir, struct dentry *old_dentry,
			   const struct path *new_dir, struct dentry *new_dentry,
			   unsigned int flags)
{
	int rc;

	if (!kap_lsm_current_active())
		return 0;
	/* Both ends matter. The destination is the bypass being closed, and the
	 * source stops a bound task moving a file out of a path it was never
	 * allowed to read.
	 */
	rc = kap_lsm_check_dir_entry(old_dir, old_dentry);
	if (rc)
		return rc;
	return kap_lsm_check_dir_entry(new_dir, new_dentry);
}

static int kap_path_link(struct dentry *old_dentry, const struct path *new_dir,
			 struct dentry *new_dentry)
{
	struct path old_path;
	int rc;

	if (!kap_lsm_current_active())
		return 0;
	/* Checking only the destination would let a denied file be linked into
	 * an allowed directory and then read through the alias, since the later
	 * open sees the allowed name. A hard link cannot cross a mount, so the
	 * source shares new_dir's vfsmount.
	 */
	old_path.mnt = new_dir ? new_dir->mnt : NULL;
	old_path.dentry = old_dentry ? old_dentry->d_parent : NULL;
	rc = kap_lsm_check_dir_entry(&old_path, old_dentry);
	if (rc)
		return rc;
	return kap_lsm_check_dir_entry(new_dir, new_dentry);
}

static int kap_path_symlink(const struct path *dir, struct dentry *dentry,
			    const char *old_name)
{
	if (!kap_lsm_current_active())
		return 0;
	return kap_lsm_check_dir_entry(dir, dentry);
}

/*
 * Creating or removing an entry under a denied directory never reaches open or
 * rename, so without these the deny still leaks for those mutations.
 */
static int kap_path_mknod(const struct path *dir, struct dentry *dentry,
			  umode_t mode, unsigned int dev)
{
	if (!kap_lsm_current_active())
		return 0;
	return kap_lsm_check_dir_entry(dir, dentry);
}

static int kap_path_mkdir(const struct path *dir, struct dentry *dentry,
			  umode_t mode)
{
	if (!kap_lsm_current_active())
		return 0;
	return kap_lsm_check_dir_entry(dir, dentry);
}

static int kap_path_unlink(const struct path *dir, struct dentry *dentry)
{
	if (!kap_lsm_current_active())
		return 0;
	return kap_lsm_check_dir_entry(dir, dentry);
}

static int kap_path_rmdir(const struct path *dir, struct dentry *dentry)
{
	if (!kap_lsm_current_active())
		return 0;
	return kap_lsm_check_dir_entry(dir, dentry);
}

static int kap_sb_mount(const char *dev_name, const struct path *path,
			const char *type, unsigned long flags, void *data)
{
	char path_target[KAP_LSM_TARGET_LEN];
	char target[KAP_LSM_TARGET_LEN];

	if (!kap_lsm_current_active())
		return 0;

	if (kap_lsm_path_target(path, path_target, sizeof(path_target)))
		strscpy(path_target, "unknown", sizeof(path_target));
	scnprintf(target, sizeof(target), "mount:mount:%s:%s",
		  type ?: "unknown", path_target);
	return kap_check_static_target(KAP_HOOK_NAMESPACE, target,
				       "mount_predicate_denied", "mount_allowed");
}

static int kap_sb_umount(struct vfsmount *mnt, int flags)
{
	struct path path = { .mnt = mnt, .dentry = mnt ? mnt->mnt_root : NULL };
	char path_target[KAP_LSM_TARGET_LEN];
	char target[KAP_LSM_TARGET_LEN];

	if (!kap_lsm_current_active())
		return 0;

	if (!mnt || kap_lsm_path_target(&path, path_target, sizeof(path_target)))
		strscpy(path_target, "unknown", sizeof(path_target));
	scnprintf(target, sizeof(target), "mount:umount:%s", path_target);
	return kap_check_static_target(KAP_HOOK_NAMESPACE, target,
				       "mount_predicate_denied", "mount_allowed");
}

static int kap_sb_pivotroot(const struct path *old_path, const struct path *new_path)
{
	char old_target[KAP_LSM_TARGET_LEN];
	char new_target[KAP_LSM_TARGET_LEN];
	char target[KAP_LSM_TARGET_LEN];

	if (!kap_lsm_current_active())
		return 0;

	if (kap_lsm_path_target(old_path, old_target, sizeof(old_target)))
		strscpy(old_target, "unknown", sizeof(old_target));
	if (kap_lsm_path_target(new_path, new_target, sizeof(new_target)))
		strscpy(new_target, "unknown", sizeof(new_target));
	scnprintf(target, sizeof(target), "mount:pivotroot:%s:%s",
		  old_target, new_target);
	return kap_check_static_target(KAP_HOOK_NAMESPACE, target,
				       "mount_predicate_denied", "mount_allowed");
}

static int kap_move_mount(const struct path *from_path, const struct path *to_path)
{
	char from_target[KAP_LSM_TARGET_LEN];
	char to_target[KAP_LSM_TARGET_LEN];
	char target[KAP_LSM_TARGET_LEN];

	if (!kap_lsm_current_active())
		return 0;

	if (kap_lsm_path_target(from_path, from_target, sizeof(from_target)))
		strscpy(from_target, "unknown", sizeof(from_target));
	if (kap_lsm_path_target(to_path, to_target, sizeof(to_target)))
		strscpy(to_target, "unknown", sizeof(to_target));
	scnprintf(target, sizeof(target), "mount:move:%s:%s",
		  from_target, to_target);
	return kap_check_static_target(KAP_HOOK_NAMESPACE, target,
				       "mount_predicate_denied", "mount_allowed");
}

#ifdef CONFIG_SECURITY_NETWORK
static int kap_unix_stream_connect(struct sock *sock, struct sock *other, struct sock *newsk)
{
	return kap_check_static_target(KAP_HOOK_IPC, "unix:stream_connect",
				       "ipc_predicate_denied", "ipc_allowed");
}

static int kap_unix_may_send(struct socket *sock, struct socket *other)
{
	return kap_check_static_target(KAP_HOOK_IPC, "unix:may_send",
				       "ipc_predicate_denied", "ipc_allowed");
}
#endif

static int kap_file_receive(struct file *file)
{
	return kap_check_current_emit(KAP_HOOK_DESCRIPTOR, file,
				      "descriptor_predicate_denied", "descriptor_allowed");
}

static int kap_task_fix_setuid(struct cred *new, const struct cred *old, int flags)
{
	char target[128];

	if (!kap_lsm_current_active())
		return 0;

	scnprintf(target, sizeof(target), "uid=%u,euid=%u,suid=%u,fsuid=%u",
		  __kuid_val(new->uid), __kuid_val(new->euid),
		  __kuid_val(new->suid), __kuid_val(new->fsuid));
	return kap_check_current_target_emit(KAP_HOOK_CRED, target,
					     "cred_predicate_denied", "cred_allowed");
}

static int kap_task_fix_setgid(struct cred *new, const struct cred *old, int flags)
{
	char target[128];

	if (!kap_lsm_current_active())
		return 0;

	scnprintf(target, sizeof(target), "gid=%u,egid=%u,sgid=%u,fsgid=%u",
		  __kgid_val(new->gid), __kgid_val(new->egid),
		  __kgid_val(new->sgid), __kgid_val(new->fsgid));
	return kap_check_current_target_emit(KAP_HOOK_CRED, target,
					     "cred_predicate_denied", "cred_allowed");
}

static int kap_task_fix_setgroups(struct cred *new, const struct cred *old)
{
	return kap_check_static_target(KAP_HOOK_CRED, "groups",
				       "cred_predicate_denied", "cred_allowed");
}

static int kap_capset(struct cred *new, const struct cred *old,
		      const kernel_cap_t *effective,
		      const kernel_cap_t *inheritable,
		      const kernel_cap_t *permitted)
{
	return kap_check_static_target(KAP_HOOK_CRED, "capset",
				       "cred_predicate_denied", "cred_allowed");
}

static int kap_userns_create(const struct cred *cred)
{
	return kap_check_static_target(KAP_HOOK_NAMESPACE, "user",
				       "namespace_predicate_denied", "namespace_allowed");
}

static int kap_ipc_permission(struct kern_ipc_perm *ipcp, short flag)
{
	char target[64];

	if (!kap_lsm_current_active())
		return 0;

	scnprintf(target, sizeof(target), "sysv:flag=%hd", flag);
	return kap_check_current_target_emit(KAP_HOOK_IPC, target,
					     "ipc_predicate_denied", "ipc_allowed");
}

static void kap_ptrace_target(struct task_struct *child, char *target, size_t target_len)
{
	struct kap_task_state *current_state = kap_task_state(current);
	struct kap_task_state *child_state = kap_task_state(child);

	if (current_state && current_state->active &&
	    child_state && child_state->active &&
	    !strcmp(current_state->igroup_name, child_state->igroup_name) &&
	    current_state->epoch == child_state->epoch) {
		strscpy(target, "same-igroup", target_len);
		return;
	}
	if (child_state && child_state->active) {
		scnprintf(target, target_len, "igroup=%s", child_state->igroup_name);
		return;
	}
	strscpy(target, "unconfined", target_len);
}

static int kap_ptrace_access_check(struct task_struct *child, unsigned int mode)
{
	char target[128];

	if (!kap_lsm_current_active())
		return 0;

	kap_ptrace_target(child, target, sizeof(target));
	return kap_check_current_target_emit(KAP_HOOK_PTRACE, target,
					     "ptrace_predicate_denied", "ptrace_allowed");
}

static int kap_ptrace_traceme(struct task_struct *parent)
{
	char target[128];

	if (!kap_lsm_current_active())
		return 0;

	kap_ptrace_target(parent, target, sizeof(target));
	return kap_check_current_target_emit(KAP_HOOK_PTRACE, target,
					     "ptrace_predicate_denied", "ptrace_allowed");
}

#ifdef CONFIG_BPF_SYSCALL
static int kap_bpf(int cmd, union bpf_attr *attr, unsigned int size, bool kernel)
{
	char target[128];

	switch (cmd) {
	case BPF_MAP_CREATE:
		scnprintf(target, sizeof(target), "cmd=%d,map_type=%u",
			  cmd, attr ? attr->map_type : 0);
		break;
	case BPF_PROG_LOAD:
		scnprintf(target, sizeof(target), "cmd=%d,prog_type=%u",
			  cmd, attr ? attr->prog_type : 0);
		break;
	default:
		scnprintf(target, sizeof(target), "cmd=%d", cmd);
		break;
	}
	return kap_check_current_target_emit(KAP_HOOK_BPF, target,
					     "bpf_predicate_denied", "bpf_allowed");
}
#endif

#ifdef CONFIG_IO_URING
static int kap_uring_override_creds(const struct cred *new)
{
	return kap_check_static_target(KAP_HOOK_IO_URING, "override_creds",
				       "io_uring_predicate_denied", "io_uring_allowed");
}

static int kap_uring_sqpoll(void)
{
	return kap_check_static_target(KAP_HOOK_IO_URING, "sqpoll",
				       "io_uring_predicate_denied", "io_uring_allowed");
}

static int kap_uring_cmd(struct io_uring_cmd *ioucmd)
{
	return kap_check_static_target(KAP_HOOK_IO_URING, "uring_cmd",
				       "io_uring_predicate_denied", "io_uring_allowed");
}
#endif

/* truncate(2) changes the contents of an existing file without opening it, so
 * none of the open or entry hooks see it. Without this a denied file can still
 * be emptied.
 */
static int kap_path_truncate(const struct path *path)
{
	char *target;
	int rc;

	if (!kap_lsm_current_active())
		return 0;
	if (!path || !path->mnt || !path->dentry)
		return kap_lsm_refuse_unresolved("file_path_unresolved");
	target = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!target)
		return -ENOMEM;
	rc = kap_lsm_path_target(path, target, PATH_MAX);
	if (rc) {
		kfree(target);
		return kap_lsm_refuse_unresolved("file_path_unresolved");
	}
	rc = kap_check_current_target_emit(KAP_HOOK_FILE, target,
					   "file_predicate_denied", "file_allowed");
	kfree(target);
	return rc;
}

static struct security_hook_list kap_hooks[] __ro_after_init = {
	LSM_HOOK_INIT(task_alloc, kap_task_alloc),
	LSM_HOOK_INIT(task_free, kap_task_free),
	LSM_HOOK_INIT(bprm_check_security, kap_bprm_check_security),
	LSM_HOOK_INIT(sb_mount, kap_sb_mount),
	LSM_HOOK_INIT(sb_umount, kap_sb_umount),
	LSM_HOOK_INIT(sb_pivotroot, kap_sb_pivotroot),
	LSM_HOOK_INIT(move_mount, kap_move_mount),
	LSM_HOOK_INIT(file_open, kap_file_open),
	LSM_HOOK_INIT(file_permission, kap_file_permission),
	LSM_HOOK_INIT(mmap_file, kap_mmap_file),
	LSM_HOOK_INIT(file_mprotect, kap_file_mprotect),
	LSM_HOOK_INIT(file_receive, kap_file_receive),
	LSM_HOOK_INIT(path_rename, kap_path_rename),
	LSM_HOOK_INIT(path_link, kap_path_link),
	LSM_HOOK_INIT(path_symlink, kap_path_symlink),
	LSM_HOOK_INIT(path_mknod, kap_path_mknod),
	LSM_HOOK_INIT(path_mkdir, kap_path_mkdir),
	LSM_HOOK_INIT(path_unlink, kap_path_unlink),
	LSM_HOOK_INIT(path_rmdir, kap_path_rmdir),
	LSM_HOOK_INIT(path_truncate, kap_path_truncate),
#ifdef CONFIG_SECURITY_NETWORK
	LSM_HOOK_INIT(socket_connect, kap_socket_connect),
	LSM_HOOK_INIT(unix_stream_connect, kap_unix_stream_connect),
	LSM_HOOK_INIT(unix_may_send, kap_unix_may_send),
#endif
	LSM_HOOK_INIT(task_fix_setuid, kap_task_fix_setuid),
	LSM_HOOK_INIT(task_fix_setgid, kap_task_fix_setgid),
	LSM_HOOK_INIT(task_fix_setgroups, kap_task_fix_setgroups),
	LSM_HOOK_INIT(capset, kap_capset),
	LSM_HOOK_INIT(userns_create, kap_userns_create),
	LSM_HOOK_INIT(ipc_permission, kap_ipc_permission),
	LSM_HOOK_INIT(ptrace_access_check, kap_ptrace_access_check),
	LSM_HOOK_INIT(ptrace_traceme, kap_ptrace_traceme),
#ifdef CONFIG_BPF_SYSCALL
	LSM_HOOK_INIT(bpf, kap_bpf),
#endif
#ifdef CONFIG_IO_URING
	LSM_HOOK_INIT(uring_override_creds, kap_uring_override_creds),
	LSM_HOOK_INIT(uring_sqpoll, kap_uring_sqpoll),
	LSM_HOOK_INIT(uring_cmd, kap_uring_cmd),
#endif
};

static const struct lsm_id kap_lsmid = {
	.name = "kap",
	.id = LSM_ID_KAP,
};

static int __init kap_lsm_init(void)
{
	WRITE_ONCE(kap_lsm_active, true);
	security_add_hooks(kap_hooks, ARRAY_SIZE(kap_hooks), &kap_lsmid);
	pr_info("LSM hooks registered\n");
	return 0;
}

DEFINE_LSM(kap) = {
	.id = &kap_lsmid,
	.blobs = &kap_blob_sizes,
	.init = kap_lsm_init,
};
