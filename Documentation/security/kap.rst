.. SPDX-License-Identifier: GPL-2.0

======================
Kernel Assurance Plane
======================

Kernel Assurance Plane (KAP) introduces an objective-scoped authority context as
a kernel object. Intent Lineage Groups (igroups) are that object: an externally
managed authority whose lifetime is tied to an objective rather than to a
process, a thread group or a sandbox, that a trusted controller can update or
revoke while unmodified worker tasks keep running.

This document describes the design for review. It lists the alternatives that
were considered for each decision and the questions on which maintainer feedback
is wanted. The central question is in `Authority lifecycle versus enforcement
mechanism`_: whether the novel part below should be a small authority-lifecycle
facility that composes with Landlock and existing LSM hooks for the actual
mediation, rather than a self-contained LSM that carries its own.

What this is and is not
=======================

The distinctive part of KAP is not path or syscall mediation. Landlock already
restricts a task's access to the filesystem and to a few other operations, often
more completely than the checks here, and seccomp already filters syscalls. KAP
does not try to replace or improve on either.

What Linux does not provide today is an authority context with these four
properties together:

* It is **externally managed**. A trusted controller, not the task itself,
  installs and changes it. This is unlike Landlock and seccomp, where a task
  restricts itself and cannot be restricted further from outside once running.
* Its **lifetime is tied to an objective**, not to a process, a thread group or
  a sandbox. Several objectives can run in one environment, each held to its own
  authority.
* It can be **updated or revoked in place** by the controller, and the change
  takes effect at the next operation of an already running task, without that
  task cooperating and without killing it.
* It is **inherited** across fork, exec and setns, so the authority follows the
  work rather than a particular executable or label.

Everything else in this RFC, the set of checked operations and how each is
matched, exists to make that authority context enforceable. Those checks overlap
with Landlock and with existing hooks, and the open question below is whether to
keep them in KAP or delegate them.

A motivating example
====================

Two objectives, O1 and O2, run as separate worker tasks in one environment. Both
start with authority to read ``/workspace`` and ``/package-cache`` and to reach
the package registry. They are bound to two igroups.

O1 finishes its first stage. The controller advances O1 to a new authority
generation that is narrower: no package cache, no network. It writes the new
authority and a higher epoch to O1's igroup. O2's igroup is untouched.

From its next check, an already running O1 worker is held to the new authority.
It cannot reach the cache or the network even though it could a moment ago, it
was not asked to drop anything, it was not restarted, and O2 is unaffected. If
instead the controller revokes O1, the next O1 check fails closed.

This is the behaviour that a self-restriction model cannot express. Landlock and
seccomp restrict the calling thread and its future children at the point the
thread asks; they offer no way for an outside controller to narrow or revoke the
authority of a task that is already running, per objective, while a second
objective beside it is left alone. That per-objective, externally revocable
lifecycle is what KAP adds.

Model
=====

``igroup``
  Intent Lineage Group, the unit KAP enforces. An igroup owns an authority
  snapshot, a lineage root, the current epoch, an optional expiry, a transition
  sequence, its bound tasks and an event log. A process is bound by writing its
  pid to the igroup, which binds all of its threads. Children inherit the
  binding, and it is kept across exec and setns.

``authority snapshot``
  The predicate set for one igroup epoch. A deny wins over an allow, and an
  operation that no predicate allows is refused. The snapshot is compiled once
  when it is installed.

  The snapshot is the model's authority representation; its on-the-wire form is
  not. In this RFC that form is a document in a strict subset of JSON, used only
  because it is easy to produce from the prototype runtime::

    {"predicates":[{"kind":"file","allow":["/srv/work"],"deny":["/srv/work/keys"]},
                   {"kind":"exec","allow":["/usr/bin/*"],"deny":[]}]}

  The JSON subset is a prototype representation, not part of the model. Any
  userspace policy format is translated to the snapshot in userspace, before
  install. The kernel-facing questions are the representation's bounds, its
  validation, and atomic install and update, not accommodating a configuration
  language. A bounded, versioned binary form, or reuse of an existing ruleset
  format such as Landlock's, is the intended direction and is question 4 under
  `Questions for maintainers`_.

  The predicate kinds, and the limits of each as currently implemented:

  * ``exec``, ``file``: a path, matched against the path and everything below
    it. File predicates also govern rename, link, unlink, mkdir, rmdir, mknod,
    symlink and truncate, checked as the parent directory plus the name, so a
    file cannot be moved into a denied path. A program is allowed by the path it
    resolves to and refused by a deny on that path or on the name it was started
    with. This is ordinary path mediation and overlaps Landlock.
  * ``net``: connect() only, matched as ``address:port`` such as
    ``192.0.2.1:443``, or ``family:N`` for a family other than IPv4 and IPv6. A
    connect with ``AF_UNSPEC`` is not checked; ``AF_UNIX`` is checked as
    ``ipc``. bind(), listen() and already connected or accepted sockets are not
    covered. This is not a service-side network policy.
  * ``ipc``: Unix domain checks are coarse: ``unix:stream_connect`` and
    ``unix:may_send`` do not yet identify the endpoint. For a pathname socket
    the resolved endpoint is the right target and is not yet used; an abstract
    socket needs its own semantics. ``sysv:flag=N`` is only the flag passed to
    ipc_permission() and does not identify the object, so this is not SysV IPC
    confinement.
  * ``descriptor``: a file_receive check on the resolved path of a file received
    over a Unix socket. It is not a descriptor lifecycle model; using a
    descriptor already held, and accessing a mapping already established, are
    separate and are not covered by this kind.
  * ``procfs``: path-based matching under ``/proc`` with the caller's own pid
    normalised to ``/proc/self``. It is not a general procfs security model.
  * ``execmem``: the path of the mapped file, or ``anonymous``.
  * ``cred``: ``uid=U`` and the other id fields, ``groups`` or ``capset``.
  * ``namespace``: ``TYPE:OPERATION`` for clone, unshare and setns. See
    question 5 under `Questions for maintainers`_ on using existing hooks for
    these instead.
  * ``ptrace``: ``same-igroup``, ``igroup=NAME`` or ``unconfined``.
  * ``bpf``: ``cmd=N``, with ``map_type=N`` or ``prog_type=N``.
  * ``io_uring``: ``override_creds``, ``sqpoll`` and ``uring_cmd`` only. These
    three checks are not a blanket disable of io_uring and do not carry the
    submitting task's authority onto every asynchronous operation.

``epoch``
  Monotonic authority generation. Installing a higher epoch fails every task
  still bound to an older one closed from its next check.

``lineage root``
  An opaque reference supplied by userspace that ties the installed authority to
  a committed execution lineage. The kernel stores it and reports it in events;
  it does not interpret or verify it in this version. Whether a refined snapshot
  is a valid narrowing of its predecessor is a trusted-userspace responsibility,
  not a kernel check.

Deployment and privilege model
==============================

KAP is meant for a supervised agent runtime, not for an unprivileged application
that installs its own sandbox. The task running under an igroup is not the
authority for what that igroup may do; the fact that a user started the work is
not itself a grant.

A trusted, privileged runtime combines the task, its workspace, the tool
contracts, credential bindings, administrator policy and any approvals into the
compiled snapshot, installs it, binds the worker, and then runs the worker
unprivileged. Writes to the control plane are authorised against the initial
user namespace. The worker cannot widen its own authority; it can only have it
narrowed or revoked from outside.

Control plane
=============

KAP state lives under ``/sys/kernel/security/kap``::

  /sys/kernel/security/kap/
    kap.default
    igroups/            created with mkdir, as cgroups are
      <igroup>/
        kap.install
        kap.procs
        kap.lineage_root
        kap.authority_epoch
        kap.authority
        kap.expires_at
        kap.transition
        kap.mode
        kap.events
        kap.igfd

``kap.default``
  ``open`` (the default) or ``closed``. When closed, a task outside the initial
  PID namespace that is bound to no igroup cannot start a program, so a bind
  that failed cannot leave a workload running without authority.

``kap.install``
  Write the lineage root, epoch, optional expiry and transition, and the
  authority as one ``key=value`` document in a single write. They commit
  together or not at all.

``kap.procs``
  Write a pid to bind that process, with all of its threads, to the igroup.
  Reads return the pids written.

``kap.lineage_root``, ``kap.authority``
  Read or write the lineage root or the authority document. A write to
  ``kap.authority`` is compiled before it replaces the current authority; a
  document that does not compile leaves it unchanged.

``kap.authority_epoch``, ``kap.expires_at``
  Read or write the epoch, or the expiry in seconds since the Unix epoch (0 for
  none). A lower epoch, or an expiry already in the past, is refused. Writing
  an epoch, or installing authority, also clears revoked mode.

``kap.transition``
  Write a transition record whose sequence number is higher than the last one.
  Reads return the last sequence number.

``kap.mode``
  ``enforce`` or ``revoked``. A revoked igroup refuses every operation.

``kap.events``
  Read recent decisions as JSON lines with the fields ``ts_ns``, ``igroup``,
  ``decision``, ``operation``, ``target``, ``reason``, ``epoch``, ``root``
  (the lineage root) and ``pid``.

``kap.igfd``
  Read the igroup's name, epoch and lineage root on one line. The name is
  reserved for future igroup file descriptors.

Design decisions
================

Authority lifecycle versus enforcement mechanism
------------------------------------------------

This is the decision we most want feedback on, because it shapes the rest.

The current series is a self-contained LSM: it holds the igroup authority
lifecycle and also performs the operation mediation at existing LSM hooks. Most
of that mediation, path access in particular, overlaps with Landlock and could
be expressed with it.

The alternative is to keep only the novel part in KAP, the externally managed,
per-objective, revocable authority lifecycle with binding and inheritance, and
to compose it with Landlock and existing hooks for the actual enforcement, so
that an igroup's current generation drives a Landlock ruleset and other existing
checks rather than a KAP-private predicate engine. That is a smaller and more
upstreamable facility, but a larger change from this code.

We implemented the self-contained form first because it is what runs today and
makes the model concrete. We would rather present the authority-lifecycle
facility that composes with existing enforcement if the subsystem agrees that is
the right shape. Question 1 under `Questions for maintainers`_ asks exactly this.

Where the control plane lives
-----------------------------

Igroups are created and destroyed by userspace, so the control plane needs
directories that userspace can create. securityfs does not allow userspace
mkdir.

Considered:

* A dedicated filesystem mounted at ``/sys/fs/kap``, the prototype's first
  interface. It works, but puts security state outside securityfs, where the
  other LSMs keep theirs.
* securityfs with control files, for example writing a name to a ``create`` file
  to make an igroup. This keeps everything in securityfs but replaces mkdir and
  rmdir with a private protocol that every tool has to learn.

Chosen: a small internal filesystem that the kernel mounts itself and links
under securityfs, the approach AppArmor uses for policy namespaces
(``apparmorfs``, linked as ``/sys/kernel/security/apparmor/policy``). Users only
see paths under securityfs, and an igroup is created with mkdir, the way a
cgroup is.

Igroups rather than a cgroup controller
---------------------------------------

Considered: making KAP a cgroup controller, since igroups are created like
cgroups and follow the process tree.

Chosen: a separate hierarchy. Linux can of course group tasks, including across
setns; the point is not that it cannot. It is that we want the authority
lifecycle to be independent of the resource hierarchy. A container usually
places all of its tasks in one cgroup, while several objectives can run inside
it, each needing its own authority and its own independent update and
revocation. Tying authority to resource partitioning would prevent that.

An authority-lifecycle object rather than BPF LSM programs
----------------------------------------------------------

Considered: carrying the model as BPF LSM programs.

A BPF LSM can enforce at security hooks, and a BPF implementation could carry
parts of this, the per-operation checks in particular. So the question is not
which mechanism enforces, but which authority-lifecycle semantics should be a
shared, in-tree contract: how an authority object is created, attached to a
task, inherited across fork, exec and setns, updated to a new generation, and
revoked, with those transitions taking effect for already running tasks. Those
semantics are kernel object state that we think belongs in one auditable place
rather than being re-implemented per deployment. Which mechanism then performs
the mediation is the subject of the lifecycle-versus-enforcement decision above.

The kernel enforces the snapshot, not a policy language
-------------------------------------------------------

Considered: parsing a policy language in the kernel.

Chosen: the kernel accepts only a compiled predicate snapshot. From the
kernel's point of view that installed snapshot is the policy. Objective
interpretation, authorization decisions and validation of the upstream
artifacts stay in privileged userspace; the kernel enforces the installed
snapshot and the execution binding, and compiles the snapshot into per-kind
pattern sets once at install time.

Questions for maintainers
=========================

1. Should this be a minimal authority-lifecycle facility (bind and inheritance,
   atomic generation update, revocation, with lifecycle kept separate from any
   sandbox implementation) that composes with Landlock and existing hooks for
   enforcement, rather than a self-contained LSM with its own mediation? See
   `Authority lifecycle versus enforcement mechanism`_.
2. Is an internal filesystem linked under securityfs, as AppArmor does,
   acceptable for a control plane that needs userspace mkdir?
3. Should igroup removal follow cgroups, with rmdir allowed once no task is
   bound?
4. The predicate snapshot's wire form is a JSON subset and experimental. Is a
   bounded, versioned binary format preferred, or reuse of an existing ruleset
   format such as Landlock's?
5. For namespace transitions through unshare, clone and setns, this series calls
   KAP directly. If the target tree has, or would accept, LSM hooks for those
   transitions, we would use them instead of calling KAP directly. Which
   existing or proposed hooks should we build on?
6. The LSM ID is provisional and should be confirmed.

RFC status
==========

This interface is experimental. The RFC is intended to make the authority and
lineage model reviewable, not to freeze the userspace ABI.
