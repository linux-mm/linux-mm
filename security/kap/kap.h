/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SECURITY_KAP_H
#define _SECURITY_KAP_H

#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#define KAP_TASK_IGROUP_NAME_LEN 64

struct dentry;
struct task_struct;

enum kap_hook_kind {
	KAP_HOOK_EXEC,
	KAP_HOOK_FILE,
	KAP_HOOK_SOCKET_CONNECT,
	KAP_HOOK_CRED,
	KAP_HOOK_IPC,
	KAP_HOOK_NAMESPACE,
	KAP_HOOK_DESCRIPTOR,
	KAP_HOOK_BPF,
	KAP_HOOK_PTRACE,
	KAP_HOOK_PROCFS,
	KAP_HOOK_EXECMEM,
	KAP_HOOK_IO_URING,
	KAP_HOOK_LAST = KAP_HOOK_IO_URING,
};

struct kap_task_state {
	bool active;
	u64 epoch;
	char igroup_name[KAP_TASK_IGROUP_NAME_LEN];
};

struct kap_task_state *kap_task_state(struct task_struct *task);
bool kap_fail_closed_kind(enum kap_hook_kind kind);
int kap_check_current_target_why(enum kap_hook_kind kind, const char *target,
				 const char **why);
int kap_check_current_emit(enum kap_hook_kind kind, const void *object,
			   const char *deny_reason, const char *allow_reason);
int kap_check_current_target_emit(enum kap_hook_kind kind, const char *target,
				  const char *deny_reason, const char *allow_reason);
void kap_emit_current_event_target(enum kap_hook_kind kind, int decision,
				   const char *reason, const char *target);

/* Shared between the policy engine (igroup.c) and the control plane (kapfs.c). */
#define KAP_MAX_NAME_LEN 63
#define KAP_MAX_ROOT_LEN 64
#define KAP_MAX_PREDICATE_BYTES 65536
#define KAP_MAX_EVENTS_BYTES 65536
/* The install document is the authority plus a few short header lines. */
#define KAP_MAX_INSTALL_BYTES (KAP_MAX_PREDICATE_BYTES + 1024)

enum kap_file_kind {
	KAP_FILE_PROCS,
	KAP_FILE_IGFD,
	KAP_FILE_LINEAGE_ROOT,
	KAP_FILE_AUTHORITY_EPOCH,
	KAP_FILE_AUTHORITY,
	KAP_FILE_TRANSITION,
	KAP_FILE_MODE,
	KAP_FILE_EVENTS,
	KAP_FILE_EXPIRES_AT,
	KAP_FILE_INSTALL,
	KAP_FILE_COUNT,
	/* Filesystem-wide, not created per igroup, so it sits past the count. */
	KAP_FILE_DEFAULT = KAP_FILE_COUNT,
};

struct kap_igroup;

struct kap_file_ctx {
	enum kap_file_kind kind;
	struct kap_igroup *igroup;
};

struct kap_bound_pid {
	pid_t pid;
	struct list_head node;
};

/*
 * Predicates are compiled once at install, so a decision only walks an array.
 * Patterns are interned into one pool and referenced by offset, so the whole
 * thing is a flat allocation and a decision allocates nothing.
 *
 * The pool is sized to the authority document itself, so any document that is
 * accepted can be compiled: a pattern costs strlen + 1 here and at least
 * strlen + 2 in the document because of its surrounding quotes. Each set's
 * patterns are laid out contiguously and walked by length, so there is no
 * per-set entry limit either.
 */
struct kap_pattern_set {
	u32 offset;
	u32 count;
};

struct kap_compiled_authority {
	char pool[KAP_MAX_PREDICATE_BYTES];
	u32 pool_used;
	struct kap_pattern_set allow[KAP_HOOK_LAST + 1];
	struct kap_pattern_set deny[KAP_HOOK_LAST + 1];
};

struct kap_igroup {
	char name[KAP_MAX_NAME_LEN + 1];
	u64 epoch;
	bool revoked;
	bool has_sequence;
	u64 last_sequence;
	char lineage_root[KAP_MAX_ROOT_LEN + 1];
	char authority[KAP_MAX_PREDICATE_BYTES];
	size_t authority_len;
	/* Expiry in Unix seconds, 0 for none. */
	u64 expires_at;
	/* Swapped wholesale on a successful install; NULL until the first one. */
	struct kap_compiled_authority *compiled;
	char events[KAP_MAX_EVENTS_BYTES];
	size_t events_len;
	struct kap_file_ctx files[KAP_FILE_COUNT];
	struct list_head pids;
	struct list_head node;
	struct dentry *dentry;
};

struct kap_install_request {
	char lineage_root[KAP_MAX_ROOT_LEN + 1];
	u64 epoch;
	u64 expires_at;
	u64 sequence;
	/* Points into the caller's buffer, which outlives this request. */
	const char *authority;
	bool have_root;
	bool have_epoch;
	bool have_expires_at;
	bool have_transition;
	bool have_authority;
};

extern struct list_head kap_igroups;
extern spinlock_t kap_lock;
extern bool kap_lsm_active;
extern bool kap_ready;
extern bool kap_default_closed;

struct kap_igroup *kap_find_igroup_locked(const char *name);
int kap_bind_pid_locked(struct kap_igroup *igroup, pid_t pid,
			struct kap_bound_pid **entry);
int kap_parse_transition_sequence(const char *payload, u64 *sequence);
int kap_install_parse(char *payload, struct kap_install_request *req);
int kap_authority_prepare(const char *doc, struct kap_compiled_authority **out);
int kap_install_locked(struct kap_igroup *igroup,
		       const struct kap_install_request *req,
		       struct kap_compiled_authority **compiled);
void kap_trim_newline(char *value);
int kap_securityfs_init(void);

#endif /* _SECURITY_KAP_H */
