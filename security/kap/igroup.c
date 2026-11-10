// SPDX-License-Identifier: GPL-2.0
/*
 * Kernel Assurance Plane: igroups and the authority decision.
 *
 * An igroup holds the authority compiled for one objective. Privileged
 * userspace validates policy before installing it; the kernel stores only the
 * compiled state and decides each hooked operation against the current task's
 * igroup, epoch and mode. The control plane that creates igroups and installs
 * authority is in kapfs.c.
 */

#define pr_fmt(fmt) "KAP: " fmt

#include <linux/binfmts.h>
#include <linux/errno.h>
#include <linux/in.h>
#include <linux/in6.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/spinlock.h>
#include <linux/pid.h>
#include <linux/pid_namespace.h>
#include <linux/printk.h>
#include <linux/ratelimit.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timekeeping.h>

#include "kap.h"

LIST_HEAD(kap_igroups);
/*
 * Guards kap_igroups and the state of every igroup on it. A spinlock, because
 * some hooks run under other spinlocks; nothing sleeps while it is held.
 */
DEFINE_SPINLOCK(kap_lock);
/* Set once the control plane exists; until then no task can be bound. */
bool kap_ready;
/* kap.default: false is "open", the default. */
bool kap_default_closed;

static bool kap_current_epoch_stale_locked(struct kap_igroup *igroup);

static const char *kap_hook_name(enum kap_hook_kind kind)
{
	switch (kind) {
	case KAP_HOOK_EXEC:
		return "exec";
	case KAP_HOOK_FILE:
		return "file";
	case KAP_HOOK_SOCKET_CONNECT:
		return "net";
	case KAP_HOOK_CRED:
		return "cred";
	case KAP_HOOK_IPC:
		return "ipc";
	case KAP_HOOK_NAMESPACE:
		return "namespace";
	case KAP_HOOK_DESCRIPTOR:
		return "descriptor";
	case KAP_HOOK_BPF:
		return "bpf";
	case KAP_HOOK_PTRACE:
		return "ptrace";
	case KAP_HOOK_PROCFS:
		return "procfs";
	case KAP_HOOK_EXECMEM:
		return "execmem";
	case KAP_HOOK_IO_URING:
		return "io_uring";
	default:
		return "unknown";
	}
}

static int kap_file_target(const struct file *file, char *target,
			   size_t target_len)
{
	char *path_buf;
	char *path;
	int rc = 0;

	path_buf = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!path_buf)
		return -ENOMEM;
	path = d_path(&file->f_path, path_buf, PATH_MAX);
	if (IS_ERR(path))
		rc = PTR_ERR(path);
	else
		strscpy(target, path, target_len);
	kfree(path_buf);
	return rc;
}

static int kap_object_target(enum kap_hook_kind kind, const void *object,
			     char *target, size_t target_len)
{
	if (!object)
		return -EINVAL;

	switch (kind) {
	case KAP_HOOK_EXEC: {
		const struct linux_binprm *bprm = object;

		if (!bprm->file)
			return -EINVAL;
		return kap_file_target(bprm->file, target, target_len);
	}
	case KAP_HOOK_FILE:
		return kap_file_target(object, target, target_len);
	case KAP_HOOK_PROCFS:
		return kap_file_target(object, target, target_len);
	case KAP_HOOK_SOCKET_CONNECT: {
		const struct sockaddr *addr = object;

		if (addr->sa_family == AF_INET) {
			const struct sockaddr_in *in = object;

			scnprintf(target, target_len, "%pI4:%u",
				  &in->sin_addr, ntohs(in->sin_port));
			return 0;
		}
		if (addr->sa_family == AF_INET6) {
			const struct sockaddr_in6 *in6 = object;

			scnprintf(target, target_len, "%pI6c:%u",
				  &in6->sin6_addr, ntohs(in6->sin6_port));
			return 0;
		}
		/* Other families, vsock among them, are named so a grant can allow them. */
		scnprintf(target, target_len, "family:%u", addr->sa_family);
		return 0;
	}
	case KAP_HOOK_CRED:
		strscpy(target, "cred", target_len);
		return 0;
	case KAP_HOOK_IPC:
		strscpy(target, "ipc", target_len);
		return 0;
	case KAP_HOOK_NAMESPACE:
		strscpy(target, "namespace", target_len);
		return 0;
	case KAP_HOOK_DESCRIPTOR:
		return kap_file_target(object, target, target_len);
	case KAP_HOOK_BPF:
		strscpy(target, "bpf", target_len);
		return 0;
	case KAP_HOOK_PTRACE:
		strscpy(target, "ptrace", target_len);
		return 0;
	case KAP_HOOK_EXECMEM:
		return kap_file_target(object, target, target_len);
	default:
		return -EINVAL;
	}
}

static bool kap_file_prefix_match(const char *target, const char *candidate)
{
	size_t len = strlen(candidate);
	size_t target_len = strlen(target);

	if (!strcmp(target, candidate))
		return true;
	if (!len || target_len <= len)
		return false;
	if (candidate[len - 1] == '/')
		return !strncmp(target, candidate, len);
	return target[len] == '/' && !strncmp(target, candidate, len);
}

static bool kap_string_match(const char *target, const char *candidate)
{
	size_t len = strlen(candidate);

	if (!strcmp(candidate, "*"))
		return true;
	if (!strcmp(target, candidate))
		return true;
	if (len && candidate[len - 1] == '*')
		return !strncmp(target, candidate, len - 1);
	return false;
}

static bool kap_target_match(enum kap_hook_kind kind, const char *target,
			     const char *candidate)
{
	if (!strcmp(candidate, "*"))
		return true;
	if (kind == KAP_HOOK_FILE || kind == KAP_HOOK_DESCRIPTOR ||
	    kind == KAP_HOOK_PROCFS)
		return kap_file_prefix_match(target, candidate);
	return kap_string_match(target, candidate);
}

static const char *kap_next_predicate(const char *cursor, const char *end,
				      const char *kind_name)
{
	char needle[64];
	const char *found;

	scnprintf(needle, sizeof(needle), "\"kind\":\"%s\"", kind_name);
	found = strstr(cursor, needle);
	if (!found || found >= end)
		return NULL;
	return found;
}

/*
 * Append one pattern to the pool. The caller compiles a whole set before moving
 * on, so entries land contiguously and can be walked by length.
 */
static int kap_intern_pattern(struct kap_compiled_authority *compiled,
			      struct kap_pattern_set *set,
			      const char *value, size_t len)
{
	if ((size_t)compiled->pool_used + len + 1 > sizeof(compiled->pool))
		return -E2BIG;
	memcpy(compiled->pool + compiled->pool_used, value, len);
	compiled->pool[compiled->pool_used + len] = '\0';
	compiled->pool_used += len + 1;
	set->count++;
	return 0;
}

/* Walk one "allow" or "deny" array of a predicate block and intern its entries. */
static int kap_compile_array(struct kap_compiled_authority *compiled,
			     struct kap_pattern_set *set,
			     const char *start, const char *end,
			     const char *array_name)
{
	char needle[32];
	const char *array;
	const char *cursor;

	scnprintf(needle, sizeof(needle), "\"%s\":[", array_name);
	array = strstr(start, needle);
	if (!array || array >= end)
		return 0;
	cursor = array + strlen(needle);
	while (cursor < end && *cursor && *cursor != ']') {
		const char *value_start;
		const char *value_end;
		int rc;

		while (cursor < end && *cursor && *cursor != '"' && *cursor != ']')
			cursor++;
		if (cursor >= end || *cursor == ']')
			break;
		value_start = ++cursor;
		value_end = value_start;
		while (value_end < end && *value_end && *value_end != '"')
			value_end++;
		if (value_end >= end || !*value_end)
			break;
		rc = kap_intern_pattern(compiled, set, value_start,
					value_end - value_start);
		if (rc)
			return rc;
		cursor = value_end + 1;
	}
	return 0;
}

/* Collect one array across every predicate of a kind, keeping the set contiguous. */
static int kap_compile_kind_array(struct kap_compiled_authority *compiled,
				  struct kap_pattern_set *set,
				  const char *auth, const char *end,
				  const char *kind_name, const char *array_name)
{
	const char *predicate = auth;

	set->offset = compiled->pool_used;
	set->count = 0;
	while ((predicate = kap_next_predicate(predicate, end, kind_name))) {
		const char *next = strstr(predicate + 1, "\"kind\":");
		const char *predicate_start = predicate;
		const char *predicate_end = next && next < end ? next : end;
		int rc;

		while (predicate_start > auth && *predicate_start != '{')
			predicate_start--;
		rc = kap_compile_array(compiled, set, predicate_start,
				       predicate_end, array_name);
		if (rc)
			return rc;
		predicate = predicate_end;
	}
	return 0;
}

/*
 * Compile a document into per-kind pattern sets. Allow and deny are collected in
 * separate passes per kind, because a document may contain several predicates
 * of one kind and interleaving them would break the contiguous layout the
 * matcher relies on.
 */
static int kap_compile_authority(struct kap_compiled_authority *compiled,
				 const char *auth, size_t auth_len)
{
	const char *end = auth + auth_len;
	int kind;
	int rc;

	memset(compiled, 0, sizeof(*compiled));
	for (kind = 0; kind <= KAP_HOOK_LAST; kind++) {
		const char *kind_name = kap_hook_name(kind);

		rc = kap_compile_kind_array(compiled, &compiled->allow[kind],
					    auth, end, kind_name, "allow");
		if (rc)
			return rc;
		rc = kap_compile_kind_array(compiled, &compiled->deny[kind],
					    auth, end, kind_name, "deny");
		if (rc)
			return rc;
	}
	return 0;
}

static bool kap_authority_match(struct kap_igroup *igroup, enum kap_hook_kind kind,
				const char *target, bool deny_array)
{
	const struct kap_compiled_authority *compiled = igroup->compiled;
	const struct kap_pattern_set *set;
	const char *pattern;
	u32 i;

	if (!compiled || kind > KAP_HOOK_LAST)
		return false;
	set = deny_array ? &compiled->deny[kind] : &compiled->allow[kind];
	pattern = compiled->pool + set->offset;
	for (i = 0; i < set->count; i++) {
		if (kap_target_match(kind, target, pattern))
			return true;
		pattern += strlen(pattern) + 1;
	}
	return false;
}

static bool kap_is_known_kind_name(const char *value, size_t len)
{
	int kind;

	for (kind = KAP_HOOK_EXEC; kind <= KAP_HOOK_LAST; kind++) {
		const char *known = kap_hook_name(kind);

		if (strlen(known) == len && !strncmp(value, known, len))
			return true;
	}
	return false;
}

static bool kap_range_contains(const char *start, const char *end, const char *needle)
{
	const char *found = strstr(start, needle);

	return found && found < end;
}

static int kap_validate_authority_payload(const char *payload)
{
	const char *cursor = payload;
	const char *end = payload + strlen(payload);
	bool saw_kind = false;

	if (payload[0] != '{' || !strstr(payload, "\"predicates\":["))
		return -EINVAL;

	while ((cursor = strstr(cursor, "\"kind\":\""))) {
		const char *kind_start = cursor + strlen("\"kind\":\"");
		const char *kind_end = strchr(kind_start, '"');
		const char *next_kind = strstr(kind_start, "\"kind\":\"");
		const char *predicate_start = cursor;
		const char *predicate_end = next_kind ? next_kind : end;

		if (!kind_end)
			return -EINVAL;
		if (!kap_is_known_kind_name(kind_start, kind_end - kind_start))
			return -EINVAL;
		while (predicate_start > payload && *predicate_start != '{')
			predicate_start--;
		if (!kap_range_contains(predicate_start, predicate_end, "\"allow\":["))
			return -EINVAL;
		if (!kap_range_contains(predicate_start, predicate_end, "\"deny\":["))
			return -EINVAL;
		saw_kind = true;
		cursor = kind_end + 1;
	}
	return saw_kind ? 0 : -EINVAL;
}

/*
 * With kap.default = closed, an unbound task outside the initial PID namespace
 * cannot start a program, so a workload whose bind failed does not run without
 * authority.
 *
 * Only exec is refused. Setup work that runs in the new PID namespace before
 * the bind, such as mounts, keeps working. Tasks in the initial PID namespace
 * are never refused.
 */
static bool kap_unbound_refused(enum kap_hook_kind kind)
{
	if (!READ_ONCE(kap_default_closed))
		return false;
	if (kind != KAP_HOOK_EXEC && kind != KAP_HOOK_EXECMEM)
		return false;
	return task_active_pid_ns(current) != &init_pid_ns;
}

/*
 * No igroup means nowhere to append an event, so the kernel log is the only
 * record of an unbound refusal.
 */
static int kap_refuse_unbound(enum kap_hook_kind kind, const char *target)
{
	pr_warn_ratelimited("deny igroup=<unbound> op=%s target=%s reason=unbound_task pid=%d\n",
			    kap_hook_name(kind), target ? target : "",
			    task_pid_nr(current));
	return -EACCES;
}

/*
 * Exported so the LSM layer can tell whether to keep going for a task it would
 * otherwise skip. Its hooks return early for any unbound task, which is exactly
 * the task fail-closed exists to stop, so that shortcut has to consult this.
 */
bool kap_fail_closed_kind(enum kap_hook_kind kind)
{
	return kap_unbound_refused(kind);
}

/*
 * Refuse and name the gate that did it. Only the authority-state gates set a
 * reason: a predicate match leaves *why NULL so the caller's own allow/deny
 * strings stay in use for the common path.
 */
static int kap_refuse(const char **why, const char *reason)
{
	if (why)
		*why = reason;
	return -EACCES;
}

static int kap_authority_decision_locked(struct kap_igroup *igroup,
					 enum kap_hook_kind kind,
					 const char *target,
					 const char **why)
{
	if (why)
		*why = NULL;
	if (kap_current_epoch_stale_locked(igroup))
		return kap_refuse(why, "authority_epoch_stale");
	/*
	 * Checked on every decision, not once at install. It bounds how long a
	 * running authority lives; it does not protect against a clock that
	 * has been moved back.
	 */
	if (igroup->expires_at &&
	    (u64)ktime_get_real_seconds() >= igroup->expires_at)
		return kap_refuse(why, "authority_expired");
	if (igroup->revoked)
		return kap_refuse(why, "authority_revoked");
	if (!igroup->authority_len)
		return kap_refuse(why, "authority_absent");
	if (!target || !*target)
		return kap_refuse(why, "target_unresolved");
	if (kap_authority_match(igroup, kind, target, true))
		return -EACCES;
	if (!kap_authority_match(igroup, kind, target, false))
		return -EACCES;
	return 0;
}

struct kap_igroup *kap_find_igroup_locked(const char *name)
{
	struct kap_igroup *igroup;

	list_for_each_entry(igroup, &kap_igroups, node) {
		if (!strcmp(igroup->name, name))
			return igroup;
	}
	return NULL;
}

static bool kap_pid_in_igroup_locked(struct kap_igroup *igroup, pid_t pid)
{
	struct kap_bound_pid *entry;

	list_for_each_entry(entry, &igroup->pids, node) {
		if (entry->pid == pid)
			return true;
	}
	return false;
}

static struct kap_igroup *kap_find_current_igroup_locked(void)
{
	struct kap_task_state *state = kap_task_state(current);

	if (!state || !state->active)
		return NULL;
	return kap_find_igroup_locked(state->igroup_name);
}

static bool kap_current_epoch_stale_locked(struct kap_igroup *igroup)
{
	struct kap_task_state *state = kap_task_state(current);

	return state && state->active && !strcmp(state->igroup_name, igroup->name) &&
	       state->epoch != igroup->epoch;
}

static bool kap_current_task_active(void)
{
	struct kap_task_state *state = kap_task_state(current);

	return state && state->active;
}

void kap_trim_newline(char *value)
{
	size_t len = strlen(value);

	while (len && (value[len - 1] == '\n' || value[len - 1] == '\r')) {
		value[len - 1] = '\0';
		len--;
	}
}

static void kap_append_event_locked(struct kap_igroup *igroup,
				    enum kap_hook_kind kind,
				    bool allowed,
				    const char *reason,
				    const char *target)
{
	char line[512];
	char escaped_target[192];
	u64 now = ktime_get_real_ns();
	int len;
	size_t out = 0;
	size_t i;

	escaped_target[0] = '\0';
	if (target) {
		for (i = 0; target[i] && out + 2 < sizeof(escaped_target); i++) {
			if (target[i] == '"' || target[i] == '\\') {
				if (out + 3 >= sizeof(escaped_target))
					break;
				escaped_target[out++] = '\\';
				escaped_target[out++] = target[i];
			} else if (target[i] == '\n' || target[i] == '\r' || target[i] == '\t') {
				escaped_target[out++] = ' ';
			} else if ((unsigned char)target[i] < 0x20) {
				escaped_target[out++] = '?';
			} else {
				escaped_target[out++] = target[i];
			}
		}
		escaped_target[out] = '\0';
	}

	/*
	 * kap.events halves itself when full, so allowed traffic can evict a
	 * denial. Mirror denials to the kernel log, before the ring write so
	 * the early returns below cannot drop the record.
	 */
	if (!allowed)
		pr_warn_ratelimited("deny igroup=%s op=%s target=%s reason=%s epoch=%llu pid=%d\n",
				    igroup->name, kap_hook_name(kind),
				    escaped_target, reason, igroup->epoch,
				    task_pid_nr(current));

	len = scnprintf(line, sizeof(line),
			"{\"ts_ns\":%llu,\"igroup\":\"%s\",\"decision\":\"%s\",\"operation\":\"%s\",\"target\":\"%s\",\"reason\":\"%s\",\"epoch\":%llu,\"root\":\"%s\",\"pid\":%d}\n",
			now, igroup->name, allowed ? "allow" : "deny",
			kap_hook_name(kind), escaped_target, reason, igroup->epoch,
			igroup->lineage_root, task_pid_nr(current));
	if (len <= 0)
		return;
	if (len >= KAP_MAX_EVENTS_BYTES) {
		igroup->events_len = 0;
		return;
	}
	if (igroup->events_len + len >= KAP_MAX_EVENTS_BYTES) {
		size_t keep = KAP_MAX_EVENTS_BYTES / 2;

		memmove(igroup->events, igroup->events + igroup->events_len - keep, keep);
		igroup->events_len = keep;
	}
	memcpy(igroup->events + igroup->events_len, line, len);
	igroup->events_len += len;
}

/* The common case: an unbound task is never checked and takes no lock. */
static bool kap_skip_current(enum kap_hook_kind kind)
{
	if (!READ_ONCE(kap_ready))
		return true;
	return !kap_current_task_active() && !kap_unbound_refused(kind);
}

/*
 * A program is allowed by the path it resolves to, and refused by a deny on
 * either that path or the name it was started with. "..", relative names and
 * symlinks can therefore neither step around a deny nor borrow an allow.
 */
static int kap_exec_decision_locked(struct kap_igroup *igroup,
				    const char *resolved, const char *name,
				    const char **why)
{
	int rc;

	rc = kap_authority_decision_locked(igroup, KAP_HOOK_EXEC, resolved, why);
	if (*why || !name || !*name || !strcmp(name, resolved))
		return rc;
	if (kap_authority_match(igroup, KAP_HOOK_EXEC, resolved, true) ||
	    kap_authority_match(igroup, KAP_HOOK_EXEC, name, true))
		return -EACCES;
	return rc;
}

static int kap_decide_locked(struct kap_igroup *igroup, enum kap_hook_kind kind,
			     const void *object, const char *target,
			     const char **why)
{
	if (kind == KAP_HOOK_EXEC) {
		const struct linux_binprm *bprm = object;

		return kap_exec_decision_locked(igroup, target, bprm->filename, why);
	}
	return kap_authority_decision_locked(igroup, kind, target, why);
}

int kap_check_current_target_why(enum kap_hook_kind kind, const char *target,
				 const char **why)
{
	const char *gate = NULL;
	struct kap_igroup *igroup;
	int rc = 0;

	if (why)
		*why = NULL;
	if (kap_skip_current(kind))
		return 0;

	spin_lock(&kap_lock);
	igroup = kap_find_current_igroup_locked();
	if (!igroup) {
		if (kap_unbound_refused(kind))
			rc = kap_refuse_unbound(kind, target);
		goto out;
	}
	rc = kap_authority_decision_locked(igroup, kind, target, &gate);
	if (why)
		*why = gate;
out:
	spin_unlock(&kap_lock);
	return rc;
}

int kap_check_current_emit(enum kap_hook_kind kind, const void *object,
			   const char *deny_reason, const char *allow_reason)
{
	char *target;
	struct kap_igroup *igroup;
	const char *why = NULL;
	int rc = 0;

	if (kap_skip_current(kind))
		return 0;

	target = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!target)
		return -ENOMEM;
	if (kap_object_target(kind, object, target, PATH_MAX)) {
		kfree(target);
		return -EACCES;
	}

	spin_lock(&kap_lock);
	igroup = kap_find_current_igroup_locked();
	if (!igroup) {
		if (kap_unbound_refused(kind))
			rc = kap_refuse_unbound(kind, target);
		goto out;
	}
	rc = kap_decide_locked(igroup, kind, object, target, &why);
	kap_append_event_locked(igroup, kind, rc == 0,
				rc ? (why ? why : deny_reason) : allow_reason, target);
out:
	spin_unlock(&kap_lock);
	kfree(target);
	return rc;
}

int kap_check_current_target_emit(enum kap_hook_kind kind, const char *target,
				  const char *deny_reason, const char *allow_reason)
{
	struct kap_igroup *igroup;
	const char *why = NULL;
	int rc = 0;

	if (kap_skip_current(kind))
		return 0;

	spin_lock(&kap_lock);
	igroup = kap_find_current_igroup_locked();
	if (!igroup) {
		if (kap_unbound_refused(kind))
			rc = kap_refuse_unbound(kind, target);
		goto out;
	}
	rc = kap_authority_decision_locked(igroup, kind, target, &why);
	kap_append_event_locked(igroup, kind, rc == 0,
				rc ? (why ? why : deny_reason) : allow_reason, target);
out:
	spin_unlock(&kap_lock);
	return rc;
}

void kap_emit_current_event_target(enum kap_hook_kind kind, int decision,
				   const char *reason, const char *target)
{
	struct kap_igroup *igroup;

	if (!READ_ONCE(kap_ready) || !kap_current_task_active())
		return;

	spin_lock(&kap_lock);
	igroup = kap_find_current_igroup_locked();
	if (igroup)
		kap_append_event_locked(igroup, kind, decision, reason, target);
	spin_unlock(&kap_lock);
}

int kap_parse_transition_sequence(const char *payload, u64 *sequence)
{
	const char *cursor = strstr(payload, "\"sequence\"");
	u64 value = 0;
	bool seen_digit = false;

	if (!cursor)
		cursor = strstr(payload, "sequence");
	if (!cursor)
		return -EINVAL;
	cursor = strchr(cursor, ':');
	if (!cursor)
		return -EINVAL;
	cursor++;
	while (*cursor == ' ' || *cursor == '\t')
		cursor++;
	while (*cursor >= '0' && *cursor <= '9') {
		seen_digit = true;
		value = value * 10 + (*cursor - '0');
		cursor++;
	}
	if (!seen_digit)
		return -EINVAL;
	*sequence = value;
	return 0;
}

/*
 * kap.install takes the whole document in one write so the lineage root, epoch,
 * expiry and predicates commit together or not at all. Writing them one file at
 * a time cannot be made safe: kap.authority_epoch refuses a lower value with
 * -ESTALE, deliberately, so once the epoch has advanced there is no way back and
 * a later refusal leaves every bound task stale against an epoch no authority
 * was ever installed for.
 *
 * Line oriented rather than JSON, because the authority value is itself JSON and
 * nesting it would mean parsing JSON with strstr twice. Every key may appear at
 * most once and an unrecognised key is refused rather than ignored, so a typo
 * fails the install instead of silently dropping a field.
 *
 * The whole document has to arrive in one write(). Each write is parsed on its
 * own, so a caller that splits the document sees the first chunk refused for
 * missing keys rather than a half-applied install, which is the usual contract
 * for a command file.
 */
int kap_install_parse(char *payload, struct kap_install_request *req)
{
	char *cursor = payload;
	char *line;

	memset(req, 0, sizeof(*req));

	while ((line = strsep(&cursor, "\n"))) {
		char *value;

		if (!*line)
			continue;
		value = strchr(line, '=');
		if (!value)
			return -EINVAL;
		*value++ = '\0';

		if (!strcmp(line, "lineage_root")) {
			if (req->have_root || strlen(value) > KAP_MAX_ROOT_LEN)
				return -EINVAL;
			strscpy(req->lineage_root, value,
				sizeof(req->lineage_root));
			req->have_root = true;
		} else if (!strcmp(line, "authority_epoch")) {
			if (req->have_epoch || kstrtou64(value, 10, &req->epoch))
				return -EINVAL;
			req->have_epoch = true;
		} else if (!strcmp(line, "expires_at")) {
			if (req->have_expires_at ||
			    kstrtou64(value, 10, &req->expires_at))
				return -EINVAL;
			req->have_expires_at = true;
		} else if (!strcmp(line, "transition")) {
			if (req->have_transition ||
			    kap_parse_transition_sequence(value, &req->sequence))
				return -EINVAL;
			req->have_transition = true;
		} else if (!strcmp(line, "authority")) {
			if (req->have_authority)
				return -EINVAL;
			req->authority = value;
			req->have_authority = true;
		} else {
			return -EINVAL;
		}
	}

	if (!req->have_root || !req->have_epoch || !req->have_authority)
		return -EINVAL;
	return 0;
}

/*
 * Validate and compile an authority document. Runs before kap_lock is taken,
 * because compiling allocates.
 */
int kap_authority_prepare(const char *doc, struct kap_compiled_authority **out)
{
	struct kap_compiled_authority *next;
	size_t len;
	int rc;

	*out = NULL;
	rc = kap_validate_authority_payload(doc);
	if (rc)
		return rc;
	len = strlen(doc);
	/* Stored whole in the igroup, so it has to fit with its terminator. */
	if (len >= KAP_MAX_PREDICATE_BYTES)
		return -E2BIG;

	next = kvzalloc_obj(*next, GFP_KERNEL);
	if (!next)
		return -ENOMEM;
	rc = kap_compile_authority(next, doc, len);
	if (rc) {
		kvfree(next);
		return rc;
	}
	*out = next;
	return 0;
}

/*
 * Commit a prepared install. On success *compiled is swapped with the
 * igroup's previous authority, which the caller frees after dropping the lock.
 */
int kap_install_locked(struct kap_igroup *igroup,
		       const struct kap_install_request *req,
		       struct kap_compiled_authority **compiled)
{
	struct kap_compiled_authority *old;

	/*
	 * Everything that can be refused is refused here, before the igroup is
	 * touched at all. The individual files apply the same rules; keep them
	 * in step.
	 */
	if (req->epoch < igroup->epoch)
		return -ESTALE;
	if (req->expires_at &&
	    (u64)ktime_get_real_seconds() >= req->expires_at)
		return -ESTALE;
	if (req->have_transition && igroup->has_sequence &&
	    req->sequence <= igroup->last_sequence)
		return -EALREADY;

	/* Nothing below can fail, which is what makes this a transaction. */
	strscpy(igroup->lineage_root, req->lineage_root,
		sizeof(igroup->lineage_root));
	igroup->epoch = req->epoch;
	igroup->revoked = false;
	igroup->expires_at = req->expires_at;
	if (req->have_transition) {
		igroup->last_sequence = req->sequence;
		igroup->has_sequence = true;
	}
	strscpy(igroup->authority, req->authority, sizeof(igroup->authority));
	igroup->authority_len = strlen(igroup->authority);
	old = igroup->compiled;
	igroup->compiled = *compiled;
	*compiled = old;
	return 0;
}

/*
 * Bind every thread of the process with this pid. *entry is allocated by the
 * caller before taking kap_lock and is consumed when the pid is recorded.
 */
int kap_bind_pid_locked(struct kap_igroup *igroup, pid_t pid,
			struct kap_bound_pid **entry)
{
	struct task_struct *task, *t;

	if (pid <= 0)
		return -EINVAL;
	rcu_read_lock();
	task = pid_task(find_vpid(pid), PIDTYPE_PID);
	if (!task) {
		rcu_read_unlock();
		return -ESRCH;
	}
	for_each_thread(task, t) {
		struct kap_task_state *state = kap_task_state(t);

		if (!state)
			continue;
		state->active = true;
		state->epoch = igroup->epoch;
		strscpy(state->igroup_name, igroup->name, sizeof(state->igroup_name));
	}
	rcu_read_unlock();

	if (kap_pid_in_igroup_locked(igroup, pid))
		return 0;
	(*entry)->pid = pid;
	list_add_tail(&(*entry)->node, &igroup->pids);
	*entry = NULL;
	return 0;
}
