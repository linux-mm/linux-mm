// SPDX-License-Identifier: GPL-2.0
//! Test kcov_df_remote_start/stop from kworker context.
//!
//! A composite struct holds three RBTrees (simulating RBTree/XArray/maple_tree
//! workloads). Three work phases run on system_wq:
//!   Phase 1 (populate): fill all three trees
//!   Phase 2 (update): insert new values, read existing, overwrite
//!   Phase 3 (drain): remove all entries
//!
//! User space publishes a buffer with KCOV_DF_REMOTE_ENABLE, writes to
//! /sys/kernel/debug/kcov_dataflow_test/trigger_kworker_remote, then reads
//! the captured records.

#![allow(missing_docs)]

use kernel::prelude::*;
use kernel::sync::{Arc, Completion};
use kernel::workqueue::{self, impl_has_work, new_work, Work, WorkItem};
use kernel::rbtree::RBTree;
use kernel::c_str;

module! {
    type: RustKworkerRemote,
    name: "rust_kworker_remote",
    authors: ["kcov-dataflow"],
    description: "Test kcov_df_remote capturing from kworker (RBTree composite)",
    license: "GPL",
}

// Extern bindings for kcov_dataflow remote API (kernel/kcov_dataflow.c)
unsafe extern "C" {
    fn kcov_df_remote_start(handle: u64);
    fn kcov_df_remote_stop();
}

/// Composite data structure: three trees with different key ranges.
/// Simulates a real driver managing multiple lookup tables.
struct CompositeStore {
    /// Primary index (keys 0..N)
    primary: RBTree<u64, u64>,
    /// Secondary/auxiliary index (keys 100..N)
    aux: RBTree<u64, u64>,
    /// Scratch/temp space (keys 200..N)
    scratch: RBTree<u64, u64>,
}

impl CompositeStore {
    fn new() -> Self {
        Self {
            primary: RBTree::new(),
            aux: RBTree::new(),
            scratch: RBTree::new(),
        }
    }

    /// Phase 1: populate all three trees with initial data.
    #[inline(never)]
    fn populate(&mut self) -> Result {
        for i in 0u64..8 {
            self.primary.try_create_and_insert(i, i * 0x1111, GFP_KERNEL)?;
        }
        for i in 100u64..108 {
            self.aux.try_create_and_insert(i, i * 0x2222, GFP_KERNEL)?;
        }
        for i in 200u64..208 {
            self.scratch.try_create_and_insert(i, i * 0x3333, GFP_KERNEL)?;
        }
        Ok(())
    }

    /// Phase 2: insert more, read existing, overwrite some.
    #[inline(never)]
    fn update(&mut self) -> Result {
        // Insert new entries into primary
        for i in 8u64..12 {
            self.primary.try_create_and_insert(i, i * 0x4444, GFP_KERNEL)?;
        }
        // Read from aux (get passes &K which is a struct arg)
        for i in 100u64..108 {
            let _ = self.aux.get(&i);
        }
        // Overwrite scratch entries
        for i in 200u64..204 {
            self.scratch.remove(&i);
            self.scratch.try_create_and_insert(i, i * 0x5555, GFP_KERNEL)?;
        }
        Ok(())
    }

    /// Phase 3: drain all trees.
    #[inline(never)]
    fn drain(&mut self) {
        while let Some(c) = self.primary.cursor_front_mut() {
            c.remove_current();
        }
        while let Some(c) = self.aux.cursor_front_mut() {
            c.remove_current();
        }
        while let Some(c) = self.scratch.cursor_front_mut() {
            c.remove_current();
        }
    }
}

/// Work item that runs three phases in kworker context with remote capture.
#[pin_data]
struct RemoteWork {
    #[pin]
    work: Work<RemoteWork>,
    #[pin]
    done: Completion,
}

impl_has_work! {
    impl HasWork<Self> for RemoteWork { self.work }
}

impl RemoteWork {
    fn new() -> Result<Arc<Self>> {
        Arc::pin_init(pin_init!(RemoteWork {
            work <- new_work!("RemoteWork::work"),
            done <- Completion::new(),
        }), GFP_KERNEL)
    }
}

impl WorkItem for RemoteWork {
    type Pointer = Arc<RemoteWork>;

    fn run(this: Arc<RemoteWork>) {
        // Enable remote kcov_dataflow capture for this kworker task.
        // SAFETY: FFI call to exported kernel symbol; no-op if no buffer published.
        // Handle 1 matches what trigger-view.py passes via KCOV_DF_REMOTE_ENABLE.
        unsafe { kcov_df_remote_start(1) };

        let mut store = CompositeStore::new();
        let _ = store.populate();
        let _ = store.update();
        store.drain();

        // SAFETY: FFI call to exported kernel symbol; disables capture.
        unsafe { kcov_df_remote_stop() };

        this.done.complete_all();
    }
}

// --- Debugfs trigger (same raw pattern as eight_struct_args_rust) ---

unsafe extern "C" fn write_handler(
    _file: *mut kernel::bindings::file,
    _buf: *const core::ffi::c_char,
    count: usize,
    _ppos: *mut kernel::bindings::loff_t,
) -> kernel::ffi::c_long {
    let work = match RemoteWork::new() {
        Ok(w) => w,
        Err(_) => return -(kernel::bindings::ENOMEM as kernel::ffi::c_long),
    };
    let waiter = work.clone();
    let _ = workqueue::system().enqueue(work);
    waiter.done.wait_for_completion();
    count as kernel::ffi::c_long
}

#[repr(transparent)]
struct SyncFops(kernel::bindings::file_operations);
unsafe impl Sync for SyncFops {}

static FOPS: SyncFops = SyncFops(kernel::bindings::file_operations {
    write: Some(unsafe { core::mem::transmute(write_handler as *const ()) }),
    ..unsafe { core::mem::zeroed() }
});

struct RustKworkerRemote {
    dir: *mut kernel::bindings::dentry,
}

impl kernel::Module for RustKworkerRemote {
    fn init(_module: &'static ThisModule) -> Result<Self> {
        let dir = unsafe {
            kernel::bindings::debugfs_create_dir(
                c_str!("kcov_dataflow_test").as_char_ptr(),
                core::ptr::null_mut(),
            )
        };
        unsafe {
            kernel::bindings::debugfs_create_file_unsafe(
                c_str!("trigger_kworker_remote").as_char_ptr(),
                0o222,
                dir,
                core::ptr::null_mut(),
                &FOPS.0,
            )
        };
        Ok(Self { dir })
    }
}

impl Drop for RustKworkerRemote {
    fn drop(&mut self) {
        unsafe { kernel::bindings::debugfs_remove(self.dir) };
    }
}

unsafe impl Send for RustKworkerRemote {}
unsafe impl Sync for RustKworkerRemote {}
