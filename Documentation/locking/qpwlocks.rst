.. SPDX-License-Identifier: GPL-2.0

=========
QPW locks
=========

Some places in the kernel implement a parallel programming strategy
consisting on local_locks() for most of the work, and some rare remote
operations are scheduled on target cpu. This keeps cache bouncing low since
cacheline tends to be mostly local, and avoids the cost of locks in non-RT
kernels, even though the very few remote operations will be expensive due
to scheduling overhead.

On the other hand, for RT workloads this can represent a problem:
scheduling work on remote cpu that are executing low latency tasks
is undesired and can introduce unexpected deadline misses.

QPW locks help to convert sites that use local_locks (for cpu local operations)
and queue_work_on (for queueing work remotely, to be executed
locally on the owner cpu of the lock) to QPW locks.

The lock is declared qpw_lock_t type.
The lock is initialized with qpw_lock_init.
The lock is locked with qpw_lock (takes a lock and cpu as a parameter).
The lock is unlocked with qpw_unlock (takes a lock and cpu as a parameter).

The qpw_lock_irqsave function disables interrupts and saves current interrupt state,
cpu as a parameter.

For trylock variant, there is the qpw_trylock_t type, initialized with
qpw_trylock_init. Then the corresponding qpw_trylock and
qpw_trylock_irqsave.

work_struct should be replaced by qpw_struct, which contains a cpu parameter
(owner cpu of the lock), initialized by INIT_QPW.

The queue work related functions (analogous to queue_work_on and flush_work) are:
queue_percpu_work_on and flush_percpu_work.

The behaviour of the QPW functions is as follows:

* !CONFIG_QPW (or CONFIG_QPW and qpw=off kernel boot parameter):
        - qpw_lock:                     local_lock
        - qpw_lock_irqsave:             local_lock_irqsave
        - qpw_trylock:                  local_trylock
        - qpw_trylock_irqsave:          local_trylock_irqsave
        - qpw_unlock:                   local_unlock
        - queue_percpu_work_on:         queue_work_on
        - flush_percpu_work:            flush_work

* CONFIG_QPW (and CONFIG_QPW_DEFAULT=y or qpw=on kernel boot parameter),
        - qpw_lock:                     spin_lock
        - qpw_lock_irqsave:             spin_lock_irqsave
        - qpw_trylock:                  spin_trylock
        - qpw_trylock_irqsave:          spin_trylock_irqsave
        - qpw_unlock:                   spin_unlock
        - queue_percpu_work_on:         executes work function on caller cpu
        - flush_percpu_work:            empty

qpw_get_cpu(work_struct), to be called from within qpw work function,
returns the target cpu.

In addition to the locking functions above, there are the local locking
functions (local_qpw_lock, local_qpw_trylock and local_qpw_unlock).
These must only be used to access per-CPU data from the CPU that owns
that data, and not remotely. They disable preemption or migration
and don't require a cpu parameter.

These should only be used when accessing per-CPU data of the local CPU.

