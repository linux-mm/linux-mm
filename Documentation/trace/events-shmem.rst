.. SPDX-License-Identifier: GPL-2.0

Tmpfs and shmem events
=====================

The events in ``events/shmem/`` attribute swapin and allocation activity to
``dev`` (the filesystem device) and ``ino`` (the inode number). They also
cover anonymous shared memory and memfd files. ``index`` is the page offset
requested by the caller, before any alignment for a large folio.

Swapin
------

``shmem_swapin_folio`` records each return from ``shmem_swapin_folio()``.
``error`` is the return value: zero means a folio was installed in the file
page cache; ``-EEXIST`` normally asks the caller to retry after a race.
Entry validation failures and poison markers also produce events.

``entry`` contains the original XArray entry's type and offset. For a large
entry, this is its base swap slot even when ``index`` lies in its middle.
For ``path=poison``, it encodes a marker, not a real swap slot.

``requested_order`` is the confirmed XArray entry order, or -1 when unknown.
``actual_order`` is the order of the folio obtained, or -1 if none was
obtained. A nonnegative actual order does not imply successful insertion:
always check ``error``. One successful event may cover multiple pages.

``path`` identifies the branch taken: ``lookup`` for entry validation
failure, ``cache`` for the initial swap cache hit, ``sync`` for synchronous
swapin, ``readahead`` for the readahead helper, or ``poison`` for a marker.
The helpers may themselves find a cached folio, so path does not prove I/O
occurred. Event counts are not disk-I/O or major-fault counts.

``fallback`` is a bitmask recording explicit downgrades from a higher order
to order 0 in the synchronous helper. An initial order-0 request does not
set policy reason bits, even when the policy conditions hold. Set reasons
are printed separated by ``|``:

* ``none``: no explicit downgrade in the synchronous helper.
* ``userfaultfd``: the VMA has userfaultfd armed.
* ``zswap``: ``zswap_never_enabled()`` is false, restricting the request
  to order 0.
* ``sync_fail``: the initial higher-order synchronous attempt failed.

The zswap restriction follows a static key, not the current enable setting.
It persists after zswap is disabled and also applies with
``CONFIG_ZSWAP_DEFAULT_ON=y`` even when booting with ``zswap.enabled=0``.
The policy reasons can occur together, so both ``userfaultfd`` and ``zswap``
are recorded when both restrictions apply to a higher-order request.
``fallback_error`` preserves the initial synchronous error, including
range conflicts such as ``-EBUSY``; it is zero for policy downgrades and
when fallback is ``none``. ``-ENOMEM`` can indicate allocation or memcg
charging failure.

These fields describe a decision to request order 0, not a guarantee about
the returned folio: a racing swap-cache insertion may provide a different
order. Swap-cache and readahead paths can also return a smaller folio
without setting ``fallback``.

For example, after mounting tracefs at ``/sys/kernel/tracing``, use a private
instance to capture a workload's swapins::

  mkdir /sys/kernel/tracing/instances/shmem
  cd /sys/kernel/tracing/instances/shmem
  echo 1 > events/shmem/shmem_swapin_folio/enable
  # Run the workload, then stop recording before reading the buffer.
  echo 0 > tracing_on
  cat trace

Filter or aggregate by both ``dev`` and ``ino``; inode numbers alone are
not unique across mounts. Trace filters use the numeric values shown by
the event's ``format`` file. Check ``per_cpu/cpu*/stats`` for dropped events
before interpreting counts.
