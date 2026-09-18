// SPDX-License-Identifier: GPL-2.0
/*
 * eight_struct_args_c.c - Verify kcov_dataflow captures struct pointer
 * arguments with automatic field expansion.
 *
 * Three families of structs are exercised:
 *
 *  - Flat structs s1..s8: sN has N u64 members side by side; sf_N takes N
 *    struct pointer args (s1*..sN*). Tests plain field expansion and multiple
 *    struct-pointer arguments.
 *
 *  - Recursively (value) nested structs st1..st8: stN embeds every smaller
 *    struct by value, so the nesting deepens with N:
 *        st1 = { u64 field0 }
 *        st2 = { u64 field0, st1 field1 }             // { v, {v} }
 *        stN = { u64 field0, st1 field1, ... st(N-1) field(N-1) }
 *    The deepest chain in st8 is eight levels deep. Used by the stack tests.
 *
 *  - Pointer-linked nested structs stp1..stp8: every member is a POINTER to a
 *    separately allocated object, so the nesting is followed through the heap:
 *        stp1 = { u64 *field0 }
 *        stp2 = { u64 *field0, stp1 *field1 }         // { *v, *{v} }
 *        stpN = { u64 *field0, stp1 *field1, ... stp(N-1) *field(N-1) }
 *    Used by the dynamic-allocation (kmalloc/vmalloc) tests.
 *
 * Write to /sys/kernel/debug/kcov_dataflow_test/trigger_struct to invoke.
 */
#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KCOV dataflow struct field expansion test (flat + nested)");

/* Flat structs: sN has N u64 members. */
struct s1 { u64 a; };
struct s2 { u64 a; u64 b; };
struct s3 { u64 a; u64 b; u64 c; };
struct s4 { u64 a; u64 b; u64 c; u64 d; };
struct s5 { u64 a; u64 b; u64 c; u64 d; u64 e; };
struct s6 { u64 a; u64 b; u64 c; u64 d; u64 e; u64 f; };
struct s7 { u64 a; u64 b; u64 c; u64 d; u64 e; u64 f; u64 g; };
struct s8 { u64 a; u64 b; u64 c; u64 d; u64 e; u64 f; u64 g; u64 h; };

/*
 * Recursively (value) nested structs: stN = { u64 field0; st1 field1; ...;
 * st(N-1) field(N-1); }. Each stN contains every smaller struct by value, so
 * the nesting depth grows with N (st8 is eight levels deep along its st7 chain).
 */
struct st1 { u64 field0; };
struct st2 { u64 field0; struct st1 field1; };
struct st3 { u64 field0; struct st1 field1; struct st2 field2; };
struct st4 {
	u64 field0;
	struct st1 field1;
	struct st2 field2;
	struct st3 field3;
};
struct st5 {
	u64 field0;
	struct st1 field1;
	struct st2 field2;
	struct st3 field3;
	struct st4 field4;
};
struct st6 {
	u64 field0;
	struct st1 field1;
	struct st2 field2;
	struct st3 field3;
	struct st4 field4;
	struct st5 field5;
};
struct st7 {
	u64 field0;
	struct st1 field1;
	struct st2 field2;
	struct st3 field3;
	struct st4 field4;
	struct st5 field5;
	struct st6 field6;
};
struct st8 {
	u64 field0;
	struct st1 field1;
	struct st2 field2;
	struct st3 field3;
	struct st4 field4;
	struct st5 field5;
	struct st6 field6;
	struct st7 field7;
};

/*
 * Pointer-linked nested structs: every member is a POINTER to a separately
 * allocated object. stpN = { u64 *field0; stp1 *field1; ...; stp(N-1)
 * *field(N-1); }. The dynamic-allocation tests build one of these per allocator.
 */
struct stp1 { u64 *field0; };
struct stp2 { u64 *field0; struct stp1 *field1; };
struct stp3 { u64 *field0; struct stp1 *field1; struct stp2 *field2; };
struct stp4 {
	u64 *field0;
	struct stp1 *field1;
	struct stp2 *field2;
	struct stp3 *field3;
};
struct stp5 {
	u64 *field0;
	struct stp1 *field1;
	struct stp2 *field2;
	struct stp3 *field3;
	struct stp4 *field4;
};
struct stp6 {
	u64 *field0;
	struct stp1 *field1;
	struct stp2 *field2;
	struct stp3 *field3;
	struct stp4 *field4;
	struct stp5 *field5;
};
struct stp7 {
	u64 *field0;
	struct stp1 *field1;
	struct stp2 *field2;
	struct stp3 *field3;
	struct stp4 *field4;
	struct stp5 *field5;
	struct stp6 *field6;
};
struct stp8 {
	u64 *field0;
	struct stp1 *field1;
	struct stp2 *field2;
	struct stp3 *field3;
	struct stp4 *field4;
	struct stp5 *field5;
	struct stp6 *field6;
	struct stp7 *field7;
};

/* Prototypes: sf_N takes N struct pointer arguments (s1*, s2*, ..., sN*) */
u64 sf_1(struct s1 *a);
u64 sf_2(struct s1 *a, struct s2 *b);
u64 sf_3(struct s1 *a, struct s2 *b, struct s3 *c);
u64 sf_4(struct s1 *a, struct s2 *b, struct s3 *c, struct s4 *d);
u64 sf_5(struct s1 *a, struct s2 *b, struct s3 *c, struct s4 *d, struct s5 *e);
u64 sf_6(struct s1 *a, struct s2 *b, struct s3 *c, struct s4 *d, struct s5 *e,
	 struct s6 *f);
u64 sf_7(struct s1 *a, struct s2 *b, struct s3 *c, struct s4 *d, struct s5 *e,
	 struct s6 *f, struct s7 *g);
u64 sf_8(struct s1 *a, struct s2 *b, struct s3 *c, struct s4 *d, struct s5 *e,
	 struct s6 *f, struct s7 *g, struct s8 *h);

/* stf_N takes a pointer to the value-nested stN and sums every reachable field0. */
u64 stf_1(struct st1 *p);
u64 stf_2(struct st2 *p);
u64 stf_3(struct st3 *p);
u64 stf_4(struct st4 *p);
u64 stf_5(struct st5 *p);
u64 stf_6(struct st6 *p);
u64 stf_7(struct st7 *p);
u64 stf_8(struct st8 *p);

/* stpf_N follows the pointer-linked stpN and sums every reachable *field0. */
u64 stpf_1(struct stp1 *p);
u64 stpf_2(struct stp2 *p);
u64 stpf_3(struct stp3 *p);
u64 stpf_4(struct stp4 *p);
u64 stpf_5(struct stp5 *p);
u64 stpf_6(struct stp6 *p);
u64 stpf_7(struct stp7 *p);
u64 stpf_8(struct stp8 *p);

noinline u64 sf_1(struct s1 *a) { return a->a; }
EXPORT_SYMBOL(sf_1);

noinline u64 sf_2(struct s1 *a, struct s2 *b) { return a->a + b->b; }
EXPORT_SYMBOL(sf_2);

noinline u64 sf_3(struct s1 *a, struct s2 *b, struct s3 *c)
{
	return a->a + b->b + c->c;
}
EXPORT_SYMBOL(sf_3);

noinline u64 sf_4(struct s1 *a, struct s2 *b, struct s3 *c, struct s4 *d)
{
	return a->a + b->b + c->c + d->d;
}
EXPORT_SYMBOL(sf_4);

noinline u64 sf_5(struct s1 *a, struct s2 *b, struct s3 *c, struct s4 *d,
		  struct s5 *e)
{
	return a->a + b->b + c->c + d->d + e->e;
}
EXPORT_SYMBOL(sf_5);

noinline u64 sf_6(struct s1 *a, struct s2 *b, struct s3 *c, struct s4 *d,
		  struct s5 *e, struct s6 *f)
{
	return a->a + b->b + c->c + d->d + e->e + f->f;
}
EXPORT_SYMBOL(sf_6);

noinline u64 sf_7(struct s1 *a, struct s2 *b, struct s3 *c, struct s4 *d,
		  struct s5 *e, struct s6 *f, struct s7 *g)
{
	return a->a + b->b + c->c + d->d + e->e + f->f + g->g;
}
EXPORT_SYMBOL(sf_7);

noinline u64 sf_8(struct s1 *a, struct s2 *b, struct s3 *c, struct s4 *d,
		  struct s5 *e, struct s6 *f, struct s7 *g, struct s8 *h)
{
	return a->a + b->b + c->c + d->d + e->e + f->f + g->g + h->h;
}
EXPORT_SYMBOL(sf_8);

/*
 * Value-nested functions. Each reads its own field0 and forwards the address of
 * every nested member into the matching stf_k, so the whole recursive tower is
 * walked and each nesting level is a distinct instrumented struct-pointer arg.
 */
noinline u64 stf_1(struct st1 *p) { return p->field0; }
EXPORT_SYMBOL(stf_1);

noinline u64 stf_2(struct st2 *p)
{
	return p->field0 + stf_1(&p->field1);
}
EXPORT_SYMBOL(stf_2);

noinline u64 stf_3(struct st3 *p)
{
	return p->field0 + stf_1(&p->field1) + stf_2(&p->field2);
}
EXPORT_SYMBOL(stf_3);

noinline u64 stf_4(struct st4 *p)
{
	return p->field0 + stf_1(&p->field1) + stf_2(&p->field2) +
	       stf_3(&p->field3);
}
EXPORT_SYMBOL(stf_4);

noinline u64 stf_5(struct st5 *p)
{
	return p->field0 + stf_1(&p->field1) + stf_2(&p->field2) +
	       stf_3(&p->field3) + stf_4(&p->field4);
}
EXPORT_SYMBOL(stf_5);

noinline u64 stf_6(struct st6 *p)
{
	return p->field0 + stf_1(&p->field1) + stf_2(&p->field2) +
	       stf_3(&p->field3) + stf_4(&p->field4) + stf_5(&p->field5);
}
EXPORT_SYMBOL(stf_6);

noinline u64 stf_7(struct st7 *p)
{
	return p->field0 + stf_1(&p->field1) + stf_2(&p->field2) +
	       stf_3(&p->field3) + stf_4(&p->field4) + stf_5(&p->field5) +
	       stf_6(&p->field6);
}
EXPORT_SYMBOL(stf_7);

noinline u64 stf_8(struct st8 *p)
{
	return p->field0 + stf_1(&p->field1) + stf_2(&p->field2) +
	       stf_3(&p->field3) + stf_4(&p->field4) + stf_5(&p->field5) +
	       stf_6(&p->field6) + stf_7(&p->field7);
}
EXPORT_SYMBOL(stf_8);

/*
 * Pointer-linked functions. Each dereferences its own *field0 and forwards each
 * (already pointer-typed) nested member into the matching stpf_k, following the
 * heap-linked tower.
 */
noinline u64 stpf_1(struct stp1 *p) { return *p->field0; }
EXPORT_SYMBOL(stpf_1);

noinline u64 stpf_2(struct stp2 *p)
{
	return *p->field0 + stpf_1(p->field1);
}
EXPORT_SYMBOL(stpf_2);

noinline u64 stpf_3(struct stp3 *p)
{
	return *p->field0 + stpf_1(p->field1) + stpf_2(p->field2);
}
EXPORT_SYMBOL(stpf_3);

noinline u64 stpf_4(struct stp4 *p)
{
	return *p->field0 + stpf_1(p->field1) + stpf_2(p->field2) +
	       stpf_3(p->field3);
}
EXPORT_SYMBOL(stpf_4);

noinline u64 stpf_5(struct stp5 *p)
{
	return *p->field0 + stpf_1(p->field1) + stpf_2(p->field2) +
	       stpf_3(p->field3) + stpf_4(p->field4);
}
EXPORT_SYMBOL(stpf_5);

noinline u64 stpf_6(struct stp6 *p)
{
	return *p->field0 + stpf_1(p->field1) + stpf_2(p->field2) +
	       stpf_3(p->field3) + stpf_4(p->field4) + stpf_5(p->field5);
}
EXPORT_SYMBOL(stpf_6);

noinline u64 stpf_7(struct stp7 *p)
{
	return *p->field0 + stpf_1(p->field1) + stpf_2(p->field2) +
	       stpf_3(p->field3) + stpf_4(p->field4) + stpf_5(p->field5) +
	       stpf_6(p->field6);
}
EXPORT_SYMBOL(stpf_7);

noinline u64 stpf_8(struct stp8 *p)
{
	return *p->field0 + stpf_1(p->field1) + stpf_2(p->field2) +
	       stpf_3(p->field3) + stpf_4(p->field4) + stpf_5(p->field5) +
	       stpf_6(p->field6) + stpf_7(p->field7);
}
EXPORT_SYMBOL(stpf_8);

u64 sf_fwd_inner(struct s1 *a, struct s2 *b, struct s3 *c, struct s4 *d);
u64 sf_fwd(struct s1 *a, struct s2 *b, struct s3 *c, struct s4 *d);
struct s4 sf_ret_struct(struct s1 *a, struct s2 *b);

/* Pointer forwarding: callee receives pointer and passes it to another func */
noinline u64 sf_fwd_inner(struct s1 *a, struct s2 *b, struct s3 *c, struct s4 *d)
{
	return a->a + b->b + c->c + d->d;
}
EXPORT_SYMBOL(sf_fwd_inner);

noinline u64 sf_fwd(struct s1 *a, struct s2 *b, struct s3 *c, struct s4 *d)
{
	return sf_fwd_inner(a, b, c, d);
}
EXPORT_SYMBOL(sf_fwd);

/* Struct return value */
noinline struct s4 sf_ret_struct(struct s1 *a, struct s2 *b)
{
	struct s4 ret = { .a = a->a, .b = b->a, .c = b->b, .d = a->a + b->b };

	return ret;
}
EXPORT_SYMBOL(sf_ret_struct);

/* Allocator shims so run_stp8() can build the pointer tree with either API. */
static void *t_kmalloc(size_t n) { return kmalloc(n, GFP_KERNEL); }
static void *t_vmalloc(size_t n) { return vmalloc(n); }
static void t_kfree(void *p) { kfree(p); }
static void t_vfree(void *p) { vfree(p); }

/*
 * Build the pointer-linked stp8 tower with @alloc (each node separately
 * allocated), run stpf_8() over it, then free every node with @fr. Sub-nodes
 * are shared (a DAG); each unique allocation is freed exactly once.
 */
static u64 run_stp8(void *(*alloc)(size_t), void (*fr)(void *))
{
	u64 ret = 0;
	u64 *l1 = alloc(sizeof(u64));
	u64 *l2 = alloc(sizeof(u64));
	u64 *l3 = alloc(sizeof(u64));
	u64 *l4 = alloc(sizeof(u64));
	u64 *l5 = alloc(sizeof(u64));
	u64 *l6 = alloc(sizeof(u64));
	u64 *l7 = alloc(sizeof(u64));
	u64 *l8 = alloc(sizeof(u64));
	struct stp1 *p1 = alloc(sizeof(*p1));
	struct stp2 *p2 = alloc(sizeof(*p2));
	struct stp3 *p3 = alloc(sizeof(*p3));
	struct stp4 *p4 = alloc(sizeof(*p4));
	struct stp5 *p5 = alloc(sizeof(*p5));
	struct stp6 *p6 = alloc(sizeof(*p6));
	struct stp7 *p7 = alloc(sizeof(*p7));
	struct stp8 *p8 = alloc(sizeof(*p8));

	if (l1 && l2 && l3 && l4 && l5 && l6 && l7 && l8 &&
	    p1 && p2 && p3 && p4 && p5 && p6 && p7 && p8) {
		*l1 = 0x11; *l2 = 0x22; *l3 = 0x33; *l4 = 0x44;
		*l5 = 0x55; *l6 = 0x66; *l7 = 0x77; *l8 = 0x88;

		p1->field0 = l1;
		p2->field0 = l2; p2->field1 = p1;
		p3->field0 = l3; p3->field1 = p1; p3->field2 = p2;
		p4->field0 = l4; p4->field1 = p1; p4->field2 = p2;
		p4->field3 = p3;
		p5->field0 = l5; p5->field1 = p1; p5->field2 = p2;
		p5->field3 = p3; p5->field4 = p4;
		p6->field0 = l6; p6->field1 = p1; p6->field2 = p2;
		p6->field3 = p3; p6->field4 = p4; p6->field5 = p5;
		p7->field0 = l7; p7->field1 = p1; p7->field2 = p2;
		p7->field3 = p3; p7->field4 = p4; p7->field5 = p5;
		p7->field6 = p6;
		p8->field0 = l8; p8->field1 = p1; p8->field2 = p2;
		p8->field3 = p3; p8->field4 = p4; p8->field5 = p5;
		p8->field6 = p6; p8->field7 = p7;

		ret = stpf_8(p8);
	}

	fr(p8); fr(p7); fr(p6); fr(p5); fr(p4); fr(p3); fr(p2); fr(p1);
	fr(l8); fr(l7); fr(l6); fr(l5); fr(l4); fr(l3); fr(l2); fr(l1);
	return ret;
}

static struct dentry *test_dir;

static ssize_t trigger_write(struct file *f, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct s1 v1 = { .a = 0x11 };
	struct s2 v2 = { .a = 0x11, .b = 0x22 };
	struct s3 v3 = { .a = 0x11, .b = 0x22, .c = 0x33 };
	struct s4 v4 = { .a = 0x11, .b = 0x22, .c = 0x33, .d = 0x44 };
	struct s5 v5 = { .a = 0x11, .b = 0x22, .c = 0x33, .d = 0x44,
			 .e = 0x55 };
	struct s6 v6 = { .a = 0x11, .b = 0x22, .c = 0x33, .d = 0x44,
			 .e = 0x55, .f = 0x66 };
	struct s7 v7 = { .a = 0x11, .b = 0x22, .c = 0x33, .d = 0x44,
			 .e = 0x55, .f = 0x66, .g = 0x77 };
	struct s8 v8 = { .a = 0x11, .b = 0x22, .c = 0x33, .d = 0x44,
			 .e = 0x55, .f = 0x66, .g = 0x77, .h = 0x88 };

	/* Recursively (value) nested values: each embeds all the smaller ones. */
	struct st1 t1 = { .field0 = 0x11 };
	struct st2 t2 = { .field0 = 0x22, .field1 = t1 };
	struct st3 t3 = { .field0 = 0x33, .field1 = t1, .field2 = t2 };
	struct st4 t4 = { .field0 = 0x44, .field1 = t1, .field2 = t2,
			  .field3 = t3 };
	struct st5 t5 = { .field0 = 0x55, .field1 = t1, .field2 = t2,
			  .field3 = t3, .field4 = t4 };
	struct st6 t6 = { .field0 = 0x66, .field1 = t1, .field2 = t2,
			  .field3 = t3, .field4 = t4, .field5 = t5 };
	struct st7 t7 = { .field0 = 0x77, .field1 = t1, .field2 = t2,
			  .field3 = t3, .field4 = t4, .field5 = t5,
			  .field6 = t6 };
	u64 sum = 0;

	/* Flat struct tests: sf_N takes N struct pointer args */
	sum += sf_1(&v1);
	sum += sf_2(&v1, &v2);
	sum += sf_3(&v1, &v2, &v3);
	sum += sf_4(&v1, &v2, &v3, &v4);
	sum += sf_5(&v1, &v2, &v3, &v4, &v5);
	sum += sf_6(&v1, &v2, &v3, &v4, &v5, &v6);
	sum += sf_7(&v1, &v2, &v3, &v4, &v5, &v6, &v7);
	sum += sf_8(&v1, &v2, &v3, &v4, &v5, &v6, &v7, &v8);

	/* Value-nested struct tests (on-stack) */
	sum += stf_1(&t1);
	sum += stf_2(&t2);
	sum += stf_3(&t3);
	sum += stf_4(&t4);
	sum += stf_5(&t5);
	sum += stf_6(&t6);
	sum += stf_7(&t7);
	/*
	 * st8 is 1 KiB; keeping it on the stack alongside t1..t7 blows the 2048-byte
	 * frame limit (-Wframe-larger-than). Build it on the heap (member-wise, so no
	 * 1 KiB compound-literal temporary lands on the stack either).
	 */
	{
		struct st8 *t8 = kmalloc(sizeof(*t8), GFP_KERNEL);

		if (t8) {
			t8->field0 = 0x88;
			t8->field1 = t1;
			t8->field2 = t2;
			t8->field3 = t3;
			t8->field4 = t4;
			t8->field5 = t5;
			t8->field6 = t6;
			t8->field7 = t7;
			sum += stf_8(t8);
			kfree(t8);
		}
	}

	/* Dynamic allocation: pointer-linked stp8, each node separately alloc'd */
	sum += run_stp8(t_kmalloc, t_kfree);	/* heap/slab */
	sum += run_stp8(t_vmalloc, t_vfree);	/* vmalloc address space */

	/* Pointer forwarding: sf_fwd receives pointers and forwards to inner */
	sum += sf_fwd(&v1, &v2, &v3, &v4);

	/* Struct return value */
	{
		struct s4 ret = sf_ret_struct(&v1, &v2);

		sum += ret.a + ret.b + ret.c + ret.d;
	}

	/* Keep every call above from being optimised away (sum is otherwise dead). */
	OPTIMIZER_HIDE_VAR(sum);
	return count;
}

static const struct file_operations trigger_fops = {
	.write = trigger_write,
};

static int __init eight_struct_args_init(void)
{
	test_dir = debugfs_create_dir("kcov_dataflow_test", NULL);
	debugfs_create_file("trigger_struct", 0200, test_dir, NULL,
			    &trigger_fops);
	return 0;
}

static void __exit eight_struct_args_exit(void)
{
	debugfs_remove_recursive(test_dir);
}

module_init(eight_struct_args_init);
module_exit(eight_struct_args_exit);
