.. SPDX-License-Identifier: GPL-2.0+ OR MIT

========================================
The Hierarchical Per-CPU Counters (HPCC)
========================================

:Author: Mathieu Desnoyers

Introduction
============

Counters come in many varieties, each with their own trade offs:

 * A global atomic counter provides a fast read access to the current
   sum, at the expense of cache-line bouncing on updates. This leads to
   poor performance of frequent updates from various cores on large SMP
   systems.

 * A per-cpu split counter provides fast updates to per-cpu counters,
   at the expense of a slower aggregation (sum). The sum operation needs
   to iterate over all per-cpu counters to calculate the current total.

The hierarchical per-cpu counters attempt to provide the best of both
worlds (fast updates, and fast sum) by relaxing requirements on the sum
accuracy. It allows quickly querying an approximated sum value, along
with the possible min/max ranges of the associated precise sum. The
exact precise sum can still be calculated with an iteration on all
per-cpu counters, but the availability of an approximated sum value with
possible precise sum min/max ranges allows eliminating candidates which
are certainly outside of a known target range without the overhead of
precise sums.

Overview
========

The hierarchical per-cpu counters are organized as a tree with the
per-cpu counters as its first level and the tree root as its last
level.

The intermediate tree levels contain carry propagation counters. When
reaching a threshold (batch size), the carry is propagated up the
tree.

Each counter below the root, from the per-cpu counters up, keeps the
residual it has not propagated yet, and only propagates once this
residual reaches its batch size in either direction. This hysteresis
prevents updates going back and forth around a batch size boundary from
propagating further up the tree each time.

This allows reading an approximated value at the root, which has a
bounded accuracy determined by the tree topology and the batch size:
the precise sum is within the same distance of the approximation in
both directions. This bound grows as O(nr_cpus * log(nr_cpus)).

The per-cpu counters are bytes, which limits the batch size to 128. The
tree positions are cache line aligned to prevent false sharing between
CPUs updating different branches of the tree. A tree position holds the
items of all the counter trees initialized together by
percpu_counter_tree_init_many(), so a set of counters updated together,
such as the RSS counters of an mm, shares cache lines.
percpu_counter_tree_init_many() allocates the per-cpu counters, one per
possible CPU, and the tree items, whose number depends on nr_cpu_ids, so
struct percpu_counter_tree has a fixed size.

The fan-out of each tree level is selected at boot from nr_cpu_ids, the
highest possible CPU number plus one. It can be overridden with the
percpu_counter_tree.arity= boot parameter for performance comparison.

The bounded accuracy is a property of the settled tree. While updates are
in flight the transient error can exceed it, bounded by the number of
updaters and their increment size.

Likewise, the precise sum is exact when no update runs concurrently. It
takes no lock: an update which carries from the per-cpu counter to the
upper levels can be counted in part, the residual it left below read and
its carry missed on the way up.

Lazy Tree Activation
====================

Counters initialized with percpu_counter_tree_init_many_lazy() start
without their per-cpu counters and tree items, for counters which may
never need the scalability of the tree. Until
percpu_counter_tree_activate_many() allocates and publishes the tree,
the counters are exact:

 * percpu_counter_tree_add_owner() adds to the bias of the counter,
   written only by its owner, a single context chosen by the user of the
   counters, such as a task, without atomic instruction.

 * percpu_counter_tree_add() adds to the atomic bias of the counter, for
   any other updater.

The precise and approximate sums read both, in O(1), and count each
update whole. The activation does not move these values: the sums keep
reading them along with the tree, so updates racing with the activation
are not lost, whichever side of it they land on. The bias only has the
owner as writer, and the atomic bias only receives atomic additions,
before and after the activation. After activation, both update functions
add to the tree, and the approximation has the accuracy of the tree.

The activations of the same counters must be serialized by the caller:
a concurrent activation would publish a second tree and lose the
updates made to the first.

The RSS counters of an mm use this: the task owning the mm updates the
plain counter, and other tasks, such as reclaim, update the atomic
counter. The tree is activated when a second thread shares the mm,
under mm->rss_stat_mutex, which serializes the activations.
A single-threaded process therefore has exact RSS counters, cheap
precise sums, and neither per-cpu memory nor tree items for them.

Use Cases
=========

Use cases HPCC is meant to handle involve tracking resources which are
used across many CPUs to quickly sum as feedback for decision making to
apply throttling, quota limits, sort tasks, and perform memory or task
migration decisions. When considering approximated sums within the
accuracy range of the decision threshold, the user can either:

 * Be conservative and fast: Consider that the sum has reached the
   limit as soon as the given limit is within the approximation range.

 * Be aggressive and fast: Consider that the sum is over the
   limit only when the approximation range is over the given limit.

 * Be precise and slow: Do a precise comparison with the limit, which
   requires a precise sum when the limit is within the approximated
   range.

One use-case for these hierarchical counters is to implement a two-pass
algorithm to speed up picking a maximum/minimum sum value from
a set. A first pass compares the approximated values, and then a second
pass only needs the precise sum for counter trees which are within the
possible precise sum range of the counter tree chosen by the first pass.

Because the comparison is on the wrapped difference, it is not transitive:
ordering is undefined over a set of counters spanning more than LONG_MAX.

Functions and structures
========================

.. kernel-doc:: include/linux/percpu_counter_tree.h
.. kernel-doc:: lib/percpu_counter_tree.c
