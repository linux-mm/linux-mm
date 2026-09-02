// SPDX-License-Identifier: GPL-2.0
//! Verify kcov_dataflow captures struct pointer arguments with automatic
//! field expansion for Rust #[repr(C)] structs.
//!
//! Rust equivalent of eight_struct_args_c. Two families are exercised:
//!   - Flat structs S1..S8 (1-8 u64 members) via rsf_N.
//!   - Recursively (value) nested structs St1..St8, where StN embeds every
//!     smaller struct by value:
//!         St1 = { field0 }
//!         St2 = { field0, field1: St1 }              // { v, {v} }
//!         StN = { field0, field1: St1, ..., field(N-1): St(N-1) }
//!     so St8 is eight levels deep along its St7 chain. Each rstf_N reads its
//!     own field0 and forwards each nested member's address into rstf_k.
//!   - Pointer-linked nested structs Stp1..Stp8, where every member is a raw
//!     pointer to a separately allocated object:
//!         Stp1 = { field0: *const u64 }
//!         StpN = { field0: *const u64, field1: *const Stp1, ... }
//!     The heap (KBox) test builds this tower and follows it via rstpf_N.
//!
//! Write to /sys/kernel/debug/kcov_dataflow_test/trigger_struct_rust to invoke.

#![allow(missing_docs)]

use kernel::prelude::*;
use kernel::alloc::KBox;
use kernel::c_str;

module !{
	type:EightStructArgsRust,
	name: "eight_struct_args_rust",
	authors: ["kcov-dataflow"],
	description: "Struct field expansion test for kcov_dataflow (Rust)",
	license: "GPL",
}
#[repr(C)]
pub struct S1 {
	pub a : u64
}
#[repr(C)]
pub struct S2 {
	pub a : u64, pub b : u64
}
#[repr(C)]
pub struct S3 {
	pub a : u64, pub b : u64, pub c : u64
}
#[repr(C)]
pub struct S4 {
	pub a : u64, pub b : u64, pub c : u64, pub d : u64
}
#[repr(C)]
pub struct S5 {
	pub a : u64, pub b : u64, pub c : u64, pub d : u64, pub e : u64
}
#[repr(C)]
pub struct S6 {
	pub a : u64, pub b : u64, pub c : u64, pub d : u64, pub e : u64,
		pub f : u64
}
#[repr(C)]
pub struct S7 {
	pub a : u64, pub b : u64, pub c : u64, pub d : u64, pub e : u64,
		pub f : u64, pub g : u64
}
#[repr(C)]
pub struct S8 {
	pub a : u64, pub b : u64, pub c : u64, pub d : u64, pub e : u64,
		pub f : u64, pub g : u64, pub h : u64
}
// Recursively nested: StN = { field0, field1: St1, ..., field(N-1): St(N-1) }.
// Copy so a smaller value can be embedded into every larger one.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct St1 {
	pub field0 : u64
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct St2 {
	pub field0 : u64, pub field1 : St1
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct St3 {
	pub field0 : u64, pub field1 : St1, pub field2 : St2
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct St4 {
	pub field0 : u64, pub field1 : St1, pub field2 : St2, pub field3 : St3
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct St5 {
	pub field0 : u64, pub field1 : St1, pub field2 : St2, pub field3 : St3,
		pub field4 : St4
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct St6 {
	pub field0 : u64, pub field1 : St1, pub field2 : St2, pub field3 : St3,
		pub field4 : St4, pub field5 : St5
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct St7 {
	pub field0 : u64, pub field1 : St1, pub field2 : St2, pub field3 : St3,
		pub field4 : St4, pub field5 : St5, pub field6 : St6
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct St8 {
	pub field0 : u64, pub field1 : St1, pub field2 : St2, pub field3 : St3,
		pub field4 : St4, pub field5 : St5, pub field6 : St6,
		pub field7 : St7
}
// Pointer-linked nested: every member is a raw pointer to a separately
// allocated object. StpN = { field0: *const u64, field1: *const Stp1, ... }.
#[repr(C)]
pub struct Stp1 {
	pub field0 : *const u64
}
#[repr(C)]
pub struct Stp2 {
	pub field0 : *const u64, pub field1 : *const Stp1
}
#[repr(C)]
pub struct Stp3 {
	pub field0 : *const u64, pub field1 : *const Stp1,
		pub field2 : *const Stp2
}
#[repr(C)]
pub struct Stp4 {
	pub field0 : *const u64, pub field1 : *const Stp1,
		pub field2 : *const Stp2, pub field3 : *const Stp3
}
#[repr(C)]
pub struct Stp5 {
	pub field0 : *const u64, pub field1 : *const Stp1,
		pub field2 : *const Stp2, pub field3 : *const Stp3,
		pub field4 : *const Stp4
}
#[repr(C)]
pub struct Stp6 {
	pub field0 : *const u64, pub field1 : *const Stp1,
		pub field2 : *const Stp2, pub field3 : *const Stp3,
		pub field4 : *const Stp4, pub field5 : *const Stp5
}
#[repr(C)]
pub struct Stp7 {
	pub field0 : *const u64, pub field1 : *const Stp1,
		pub field2 : *const Stp2, pub field3 : *const Stp3,
		pub field4 : *const Stp4, pub field5 : *const Stp5,
		pub field6 : *const Stp6
}
#[repr(C)]
pub struct Stp8 {
	pub field0 : *const u64, pub field1 : *const Stp1,
		pub field2 : *const Stp2, pub field3 : *const Stp3,
		pub field4 : *const Stp4, pub field5 : *const Stp5,
		pub field6 : *const Stp6, pub field7 : *const Stp7
}

#[no_mangle]
#[inline(never)]
pub extern "C" fn rsf_1(a : *const S1) -> u64
{
	unsafe
	{
		(*a).a
	}
}

#[no_mangle]
#[inline(never)]
pub extern "C" fn rsf_2(a : *const S1, b : *const S2) -> u64
{
	unsafe
	{
		(*a).a + (*b).b
	}
}

#[no_mangle]
#[inline(never)]
pub extern "C" fn rsf_4(a : *const S1, b : *const S2, c : *const S3,
			d : *const S4) -> u64
{
	unsafe
	{
		(*a).a + (*b).b + (*c).c + (*d).d
	}
}

#[no_mangle]
#[inline(never)]
pub extern "C" fn rsf_8(a : *const S1, b : *const S2, c : *const S3,
			d : *const S4, e : *const S5, f : *const S6,
			g : *const S7, h : *const S8) -> u64
{
	unsafe
	{
		(*a).a + (*b).b + (*c).c + (*d).d + (*e).e + (*f).f + (*g).g +
			(*h).h
	}
}

// Recursively nested: each reads its own field0 and forwards every nested
// member's address into the matching rstf_k, walking the whole tower.
#[no_mangle]
#[inline(never)]
pub extern "C" fn rstf_1(p : *const St1) -> u64
{
	unsafe
	{
		(*p).field0
	}
}

#[no_mangle]
#[inline(never)]
pub extern "C" fn rstf_2(p : *const St2) -> u64
{
	unsafe
	{
		(*p).field0 + rstf_1(&(*p).field1)
	}
}

#[no_mangle]
#[inline(never)]
pub extern "C" fn rstf_3(p : *const St3) -> u64
{
	unsafe
	{
		(*p).field0 + rstf_1(&(*p).field1) + rstf_2(&(*p).field2)
	}
}

#[no_mangle]
#[inline(never)]
pub extern "C" fn rstf_4(p : *const St4) -> u64
{
	unsafe
	{
		(*p).field0 + rstf_1(&(*p).field1) + rstf_2(&(*p).field2) +
			rstf_3(&(*p).field3)
	}
}

#[no_mangle]
#[inline(never)]
pub extern "C" fn rstf_5(p : *const St5) -> u64
{
	unsafe
	{
		(*p).field0 + rstf_1(&(*p).field1) + rstf_2(&(*p).field2) +
			rstf_3(&(*p).field3) + rstf_4(&(*p).field4)
	}
}

#[no_mangle]
#[inline(never)]
pub extern "C" fn rstf_6(p : *const St6) -> u64
{
	unsafe
	{
		(*p).field0 + rstf_1(&(*p).field1) + rstf_2(&(*p).field2) +
			rstf_3(&(*p).field3) + rstf_4(&(*p).field4) +
			rstf_5(&(*p).field5)
	}
}

#[no_mangle]
#[inline(never)]
pub extern "C" fn rstf_7(p : *const St7) -> u64
{
	unsafe
	{
		(*p).field0 + rstf_1(&(*p).field1) + rstf_2(&(*p).field2) +
			rstf_3(&(*p).field3) + rstf_4(&(*p).field4) +
			rstf_5(&(*p).field5) + rstf_6(&(*p).field6)
	}
}

#[no_mangle]
#[inline(never)]
pub extern "C" fn rstf_8(p : *const St8) -> u64
{
	unsafe
	{
		(*p).field0 + rstf_1(&(*p).field1) + rstf_2(&(*p).field2) +
			rstf_3(&(*p).field3) + rstf_4(&(*p).field4) +
			rstf_5(&(*p).field5) + rstf_6(&(*p).field6) +
			rstf_7(&(*p).field7)
	}
}

// Pointer-linked: each dereferences its own *field0 and forwards each
// (already pointer-typed) nested member into the matching rstpf_k.
#[no_mangle]
#[inline(never)]
pub extern "C" fn rstpf_1(p : *const Stp1) -> u64
{
	unsafe
	{
		*(*p).field0
	}
}

#[no_mangle]
#[inline(never)]
pub extern "C" fn rstpf_2(p : *const Stp2) -> u64
{
	unsafe
	{
		*(*p).field0 + rstpf_1((*p).field1)
	}
}

#[no_mangle]
#[inline(never)]
pub extern "C" fn rstpf_3(p : *const Stp3) -> u64
{
	unsafe
	{
		*(*p).field0 + rstpf_1((*p).field1) + rstpf_2((*p).field2)
	}
}

#[no_mangle]
#[inline(never)]
pub extern "C" fn rstpf_4(p : *const Stp4) -> u64
{
	unsafe
	{
		*(*p).field0 + rstpf_1((*p).field1) + rstpf_2((*p).field2) +
			rstpf_3((*p).field3)
	}
}

#[no_mangle]
#[inline(never)]
pub extern "C" fn rstpf_5(p : *const Stp5) -> u64
{
	unsafe
	{
		*(*p).field0 + rstpf_1((*p).field1) + rstpf_2((*p).field2) +
			rstpf_3((*p).field3) + rstpf_4((*p).field4)
	}
}

#[no_mangle]
#[inline(never)]
pub extern "C" fn rstpf_6(p : *const Stp6) -> u64
{
	unsafe
	{
		*(*p).field0 + rstpf_1((*p).field1) + rstpf_2((*p).field2) +
			rstpf_3((*p).field3) + rstpf_4((*p).field4) +
			rstpf_5((*p).field5)
	}
}

#[no_mangle]
#[inline(never)]
pub extern "C" fn rstpf_7(p : *const Stp7) -> u64
{
	unsafe
	{
		*(*p).field0 + rstpf_1((*p).field1) + rstpf_2((*p).field2) +
			rstpf_3((*p).field3) + rstpf_4((*p).field4) +
			rstpf_5((*p).field5) + rstpf_6((*p).field6)
	}
}

#[no_mangle]
#[inline(never)]
pub extern "C" fn rstpf_8(p : *const Stp8) -> u64
{
	unsafe
	{
		*(*p).field0 + rstpf_1((*p).field1) + rstpf_2((*p).field2) +
			rstpf_3((*p).field3) + rstpf_4((*p).field4) +
			rstpf_5((*p).field5) + rstpf_6((*p).field6) +
			rstpf_7((*p).field7)
	}
}

// Build the pointer-linked Stp8 tower with KBox (each node its own allocation),
// run rstpf_8 over it, and return the sum. The KBoxes own the storage and hold
// raw pointers into their siblings; everything is freed when they drop at the
// end of this function. `?` frees any already-allocated KBoxes on OOM.
fn build_and_run_stp8() -> Result<u64>
{
	let l1 = KBox::new (0x11u64, kernel::alloc::flags::GFP_KERNEL) ? ;
	let l2 = KBox::new (0x22u64, kernel::alloc::flags::GFP_KERNEL) ? ;
	let l3 = KBox::new (0x33u64, kernel::alloc::flags::GFP_KERNEL) ? ;
	let l4 = KBox::new (0x44u64, kernel::alloc::flags::GFP_KERNEL) ? ;
	let l5 = KBox::new (0x55u64, kernel::alloc::flags::GFP_KERNEL) ? ;
	let l6 = KBox::new (0x66u64, kernel::alloc::flags::GFP_KERNEL) ? ;
	let l7 = KBox::new (0x77u64, kernel::alloc::flags::GFP_KERNEL) ? ;
	let l8 = KBox::new (0x88u64, kernel::alloc::flags::GFP_KERNEL) ? ;

	let p1 = KBox::new (Stp1{ field0: &*l1 },
			    kernel::alloc::flags::GFP_KERNEL) ?
		;
	let p2 = KBox::new (Stp2{ field0: &*l2, field1: &*p1 },
			    kernel::alloc::flags::GFP_KERNEL) ?
		;
	let p3 = KBox::new (Stp3{ field0: &*l3, field1: &*p1, field2: &*p2 },
			    kernel::alloc::flags::GFP_KERNEL) ?
		;
	let p4 = KBox::new (
		Stp4{ field0: &*l4, field1: &*p1, field2: &*p2, field3: &*p3 },
		kernel::alloc::flags::GFP_KERNEL) ?
		;
	let p5 = KBox::new (Stp5{
		field0: &*l5,
		field1: &*p1,
		field2: &*p2,
		field3: &*p3,
		field4: &*p4
	},
			    kernel::alloc::flags::GFP_KERNEL) ?
		;
	let p6 = KBox::new (Stp6{
		field0: &*l6,
		field1: &*p1,
		field2: &*p2,
		field3: &*p3,
		field4: &*p4,
		field5: &*p5
	},
			    kernel::alloc::flags::GFP_KERNEL) ?
		;
	let p7 = KBox::new (Stp7{
		field0: &*l7,
		field1: &*p1,
		field2: &*p2,
		field3: &*p3,
		field4: &*p4,
		field5: &*p5,
		field6: &*p6
	},
			    kernel::alloc::flags::GFP_KERNEL) ?
		;
	let p8 = KBox::new (Stp8{
		field0: &*l8,
		field1: &*p1,
		field2: &*p2,
		field3: &*p3,
		field4: &*p4,
		field5: &*p5,
		field6: &*p6,
		field7: &*p7
	},
			    kernel::alloc::flags::GFP_KERNEL) ?
		;

	Ok(rstpf_8(&*p8))
}

/* Pointer forwarding: receives pointers and passes to inner */
#[no_mangle]
#[inline(never)]
pub extern "C" fn rsf_fwd_inner(a : *const S1, b : *const S2, c : *const S3,
				d : *const S4) -> u64
{
	unsafe
	{
		(*a).a + (*b).b + (*c).c + (*d).d
	}
}

#[no_mangle]
#[inline(never)]
pub extern "C" fn rsf_fwd(a : *const S1, b : *const S2, c : *const S3,
			  d : *const S4) -> u64{ rsf_fwd_inner(a, b, c, d) }

/* Struct return value */
#[no_mangle]
#[inline(never)]
pub extern "C" fn rsf_ret_struct(a : *const S1, b : *const S2)
	->S4
{
	unsafe
	{
		S4
		{
a:
			(*a).a, b : (*b).a, c : (*b).b, d : (*a).a + (*b).b
		}
	}
}

unsafe extern "C" fn write_handler(_file : *mut kernel::bindings::file,
				   _buf : *const core::ffi::c_char,
				   count : usize,
				   _ppos : *mut kernel::bindings::loff_t, )
	-> kernel::ffi::c_long
{
	let v1 = S1{ a: 0x11 };
	let v2 = S2{ a: 0x11, b: 0x22 };
	let v3 = S3{ a: 0x11, b: 0x22, c: 0x33 };
	let v4 = S4{ a: 0x11, b: 0x22, c: 0x33, d: 0x44 };
	let v5 = S5{ a: 0x11, b: 0x22, c: 0x33, d: 0x44, e: 0x55 };
	let v6 = S6{ a: 0x11, b: 0x22, c: 0x33, d: 0x44, e: 0x55, f: 0x66 };
	let v7 =
	S7{ a: 0x11, b: 0x22, c: 0x33, d: 0x44, e: 0x55, f: 0x66, g: 0x77 };
	let v8 = S8{
		a: 0x11,
		b: 0x22,
		c: 0x33,
		d: 0x44,
		e: 0x55,
		f: 0x66,
		g: 0x77,
		h: 0x88
	};

	// Recursively nested values: each embeds all the smaller ones (Copy).
	let t1 = St1{ field0: 0x11 };
	let t2 = St2{ field0: 0x22, field1: t1 };
	let t3 = St3{ field0: 0x33, field1: t1, field2: t2 };
	let t4 = St4{ field0: 0x44, field1: t1, field2: t2, field3: t3 };
	let t5 =
	St5{ field0: 0x55, field1: t1, field2: t2, field3: t3, field4: t4 };
	let t6 = St6{
		field0: 0x66,
		field1: t1,
		field2: t2,
		field3: t3,
		field4: t4,
		field5: t5
	};
	let t7 = St7{
		field0: 0x77,
		field1: t1,
		field2: t2,
		field3: t3,
		field4: t4,
		field5: t5,
		field6: t6
	};
	let t8 = St8{
		field0: 0x88,
		field1: t1,
		field2: t2,
		field3: t3,
		field4: t4,
		field5: t5,
		field6: t6,
		field7: t7
	};

	let mut sum : u64 = 0;
	sum = sum.wrapping_add(rsf_1(&v1 as *const S1));
	sum = sum.wrapping_add(rsf_2(&v1 as *const S1, &v2 as *const S2));
	sum = sum.wrapping_add(rsf_4(&v1 as *const S1, &v2 as *const S2,
				     &v3 as *const S3, &v4 as *const S4));
	sum = sum.wrapping_add(rsf_8(&v1 as *const S1, &v2 as *const S2,
				     &v3 as *const S3, &v4 as *const S4,
				     &v5 as *const S5, &v6 as *const S6,
				     &v7 as *const S7, &v8 as *const S8));

	// Recursively nested struct tests
	sum = sum.wrapping_add(rstf_1(&t1 as *const St1));
	sum = sum.wrapping_add(rstf_2(&t2 as *const St2));
	sum = sum.wrapping_add(rstf_3(&t3 as *const St3));
	sum = sum.wrapping_add(rstf_4(&t4 as *const St4));
	sum = sum.wrapping_add(rstf_5(&t5 as *const St5));
	sum = sum.wrapping_add(rstf_6(&t6 as *const St6));
	sum = sum.wrapping_add(rstf_7(&t7 as *const St7));
	sum = sum.wrapping_add(rstf_8(&t8 as *const St8));

	// Pointer forwarding: rsf_fwd receives and passes to rsf_fwd_inner
	sum = sum.wrapping_add(rsf_fwd(&v1 as *const S1, &v2 as *const S2,
				       &v3 as *const S3, &v4 as *const S4));

	// Struct return value
	let ret = rsf_ret_struct(&v1 as *const S1, &v2 as *const S2);
	sum = sum.wrapping_add(ret.a + ret.b + ret.c + ret.d);

	// Dynamic allocation: pointer-linked Stp8 tower (each node its own KBox)
	if let
		Ok(s) = build_and_run_stp8()
		{
			sum = sum.wrapping_add(s);
		}

	core::hint::black_box(sum);
	count as kernel::ffi::c_long
}

#[repr(transparent)]
struct SyncFops(kernel::bindings::file_operations);
unsafe impl Sync for SyncFops
{
}

static FOPS : SyncFops = SyncFops(kernel::bindings::file_operations{
	write: Some(unsafe{ core::mem::transmute(write_handler as *const()) }),
	..unsafe{ core::mem::zeroed() }
});

struct EightStructArgsRust {
	dir : *mut kernel::bindings::dentry,
}

impl kernel::Module for EightStructArgsRust
{
    fn init(_module: &'static ThisModule) -> Result<Self> {
        let dir = unsafe {
            kernel::bindings::debugfs_create_dir(
                c_str!("kcov_dataflow_test").as_char_ptr(),
                core::ptr::null_mut(),
            )
        };
        unsafe {
            kernel::bindings::debugfs_create_file_unsafe(
                c_str!("trigger_struct_rust").as_char_ptr(),
                0o222,
                dir,
                core::ptr::null_mut(),
                &FOPS.0,
            )
        };
        Ok(Self { dir })
}
}

impl Drop for EightStructArgsRust
{
	fn drop(&mut self)
	{
		unsafe{ kernel::bindings::debugfs_remove(self.dir) };
	}
}

unsafe impl Send for EightStructArgsRust
{
}
unsafe impl Sync for EightStructArgsRust
{
}
