// MIT. GDIR VM for the Blazium PS2 guest: runs the cooked GDScript subset.
// See gdir_ops.h for the bytecode and blazium/platform/ps2/export/cook/
// gdir_compile.cpp for the compiler. Not the Godot VM: no Variant/ClassDB.

#include "gdir_vm.h"
#include "mc_io.h"

#include "gdir_ops.h"
#include "gs_draw.h"
#include "iso_path.h"
#include "pack_io.h"
#include "pad_io.h"
#include "sfx_io.h"
#include "sys_io.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* sizes: a guest with less RAM (the PS1) sets smaller ones */
#ifndef GDIR_MAX_NODES
#define GDIR_MAX_NODES 512
#endif
#ifndef GDIR_MAX_CLASSES
#define GDIR_MAX_CLASSES 64
#endif
#ifndef GDIR_MAX_FUNCS
#define GDIR_MAX_FUNCS 512
#endif
#ifndef GDIR_MAX_CONSTS
#define GDIR_MAX_CONSTS 16384
#endif
#ifndef GDIR_MAX_KARR
#define GDIR_MAX_KARR 1024
#endif
#ifndef GDIR_POKE_BYTES
#define GDIR_POKE_BYTES (24 * 1024)
#endif
#ifndef GDIR_MAX_PACKS
#define GDIR_MAX_PACKS 100
#endif
#ifndef GDIR_MAX_INST
#define GDIR_MAX_INST 128
#endif
#ifndef GDIR_MAX_MEMBERS
#define GDIR_MAX_MEMBERS 2048
#endif
#ifndef GDIR_STACK
#define GDIR_STACK 1024
#endif
#ifndef GDIR_DEPTH
#define GDIR_DEPTH 48
#endif
#ifndef GDIR_STR
#define GDIR_STR 64
#endif
#ifndef GDIR_RING
#define GDIR_RING 256
#endif
#ifndef GDIR_ARRAYS
#define GDIR_ARRAYS 96
#endif
#ifndef GDIR_ARR_LEN
#define GDIR_ARR_LEN 64
#endif
#ifndef GDIR_OPS_PER_FRAME
#define GDIR_OPS_PER_FRAME 400000
#endif

enum {
	T_NIL = 0,
	T_BOOL,
	T_INT,
	T_FLOAT,
	T_STR,
	T_VEC2,
	T_VEC3,
	T_NODE,
	T_ARR,
	T_KARR, /* read-only const array: i = s_karr slot */
};

struct Val {
	unsigned char t;
	union {
		int i;
		float f;
		float v[3];
		const char *s;
	};
};

struct Func {
	unsigned char argc;
	unsigned char locals;
	unsigned short need; /* stack slots a call takes (locals and operands), 0xffff unknown */
	unsigned code;
	unsigned short name; /* const index */
};

struct Class {
	unsigned short members;
	unsigned short noderefs;
	unsigned short funcs;
	unsigned short first_func;
	short fn_init, fn_ready, fn_process, fn_physics;
};

struct PackNode {
	short parent;
	unsigned char flags; // 1 visible, 2 node3d, 4 control, 8 camera
	signed char hud;
	float lpos[3], lrot[3], lscale[3];
	float w0[12];
};

struct PackInst {
	unsigned short node;
	unsigned short cls;
	const unsigned char *refs; // u16 * noderefs
	unsigned char ov_n; // per-instance @export values from the scene
	const unsigned char *ov; // (u8 member, u16 const) * ov_n
};

struct PackConn {
	unsigned short node;
	unsigned char signal;
	unsigned short inst;
	unsigned short func;
};

struct Pack {
	int present;
	unsigned char clear[3];
	unsigned char has_clear;
	short camera;
	float fov;
	unsigned short node_n;
	unsigned short path_k; /* const: res:// scene path */
	const unsigned char *nodes;
	unsigned short inst_n;
	const unsigned char *insts;
	unsigned short conn_n;
	const unsigned char *conns;
};

/* ---- program ---- */
static int s_active;
/* GDIR header flags (u16 at 6) */
#define GDIR_FLAG_PNOD 1 /* each pack's node records are in PNODnn.BIN, not here */
static unsigned s_gflags;
static Val s_k[GDIR_MAX_CONSTS];
static const unsigned char *s_karr[GDIR_MAX_KARR]; /* u16 n, u16 items[n] */
static unsigned s_karr_n;
static unsigned char s_poke[GDIR_POKE_BYTES]; /* PS2.poke/peek: kept across change_scene */
static unsigned s_k_n;
static Class s_cls[GDIR_MAX_CLASSES];
static unsigned s_cls_n;
static Func s_fn[GDIR_MAX_FUNCS];
static unsigned s_fn_n;
static const unsigned char *s_code;
static unsigned s_code_n;
static Pack s_pack[GDIR_MAX_PACKS];

/* ---- current pack state ---- */
static int s_cur = -1;
static int s_entered = -2;
static int s_pending = -1;
static float s_pending_fade = 0.0f;
static unsigned s_node_n;
static PackNode s_nodes[GDIR_MAX_NODES];
static float s_world[GDIR_MAX_NODES][12];
static float s_w0inv[GDIR_MAX_NODES][12];
static unsigned char s_vis[GDIR_MAX_NODES];
static unsigned char s_dirty;
/* Node worlds are worked out lazily. s_ndirty: the node's own transform
 * changed. s_wver counts each recompute of a node's world, and s_pver holds
 * the parent's count that world was built on, so a node is stale when it
 * moved or its parent's world changed since. A script that reads a node
 * (global_position, look_at) brings just that node and its parents up to
 * date; recompute_nodes, once a frame, does the rest and hands the
 * renderer only the matrices that changed (s_sent). */
static unsigned char s_ndirty[GDIR_MAX_NODES];
static unsigned char s_lstale[GDIR_MAX_NODES]; /* s_local needs mat_from_trs */
static unsigned s_wver[GDIR_MAX_NODES];
static unsigned s_pver[GDIR_MAX_NODES];
static unsigned s_sent[GDIR_MAX_NODES];
static unsigned char s_vis_sent[GDIR_MAX_NODES]; /* the visibility the renderer has */
static unsigned char s_lident[GDIR_MAX_NODES]; /* s_local is the identity */
static unsigned char s_lmove[GDIR_MAX_NODES]; /* s_local is a translation only */
static short s_par[GDIR_MAX_NODES]; /* the parent, -1 none (always a lower index) */
/* children in node order: first child, next sibling (-1 none), with counts */
static short s_child0[GDIR_MAX_NODES];
static short s_sib[GDIR_MAX_NODES];
static short s_nchild[GDIR_MAX_NODES];

/* Whether the renderer draws anything with a node's matrix. A guest
 * renderer that knows (the PS1's) defines this; otherwise every node's
 * matrix is sent. */
extern "C" int gs_draw_node_drawn(int node) __attribute__((weak));
static int node_drawn(int n)
{
	return gs_draw_node_drawn ? gs_draw_node_drawn(n) : 1;
}
static float s_local[GDIR_MAX_NODES][12];
static unsigned char s_all_dirty = 1;
/* The node pass looks only at the nodes moved since the last one and their
 * children: when the pack lists nodes depth first (each node's subtree the
 * nodes n .. s_subend[n] - 1), that is a run of indices per moved node. It
 * looks at all of them after a visibility or camera change (s_pass_full),
 * or when more nodes than GDIR_MARKS moved. */
#define GDIR_MARKS 32
static short s_mark[GDIR_MARKS];
static unsigned s_mark_n;
static unsigned short s_subend[GDIR_MAX_NODES];
static unsigned char s_preorder;
static unsigned char s_pass_full = 1;

static void mark_node(int n)
{
	s_dirty = 1;
	if (n >= 0 && n < GDIR_MAX_NODES) {
		if (!s_ndirty[n]) {
			if (s_mark_n < GDIR_MARKS) {
				s_mark[s_mark_n] = (short)n;
			}
			s_mark_n++;
		}
		s_ndirty[n] = 1;
		s_lstale[n] = 1;
	} else {
		s_all_dirty = 1;
	}
}
/* PS2.set_views: split-screen cameras (nodes of this pack), s_view_n >= 2 */
static short s_view_cam[4];
static int s_view_n;
static unsigned s_inst_n;
static PackInst s_inst[GDIR_MAX_INST];
static unsigned s_inst_mem[GDIR_MAX_INST];
static Val s_mem[GDIR_MAX_MEMBERS];
static char s_memstr[GDIR_MAX_MEMBERS][GDIR_STR];
static unsigned s_mem_n;
static unsigned s_conn_n;
static PackConn s_conn[64];

/* ---- execution ---- */
static Val s_stack[GDIR_STACK];
static unsigned s_sp;
/* A guest with a small fast RAM (the PS1's 1 KiB scratchpad, free while the
 * scripts run) gives it as a second stack: a call whose frame fits goes
 * there, the rest to s_stack. Frames in either are last in, first out. */
#ifdef GDIR_FAST_STACK_AT
#ifndef GDIR_FAST_STACK
#define GDIR_FAST_STACK 64
#endif
#define s_fast ((Val *)(GDIR_FAST_STACK_AT))
#endif
static unsigned s_fsp;
static int s_depth;
static unsigned s_ops;
static char s_ring[GDIR_RING][GDIR_STR];
static unsigned s_ring_i;
static Val s_arr[GDIR_ARRAYS][GDIR_ARR_LEN];
#ifndef GDIR_ARR_STR
#define GDIR_ARR_STR 40 /* a string kept in an array: bytes, with the 0 */
#endif
static char s_arrstr[GDIR_ARRAYS][GDIR_ARR_LEN][GDIR_ARR_STR];
static unsigned char s_arr_n[GDIR_ARRAYS];
static unsigned char s_arr_used[GDIR_ARRAYS];
static char s_err[96];
static float s_delta = 1.0f / 60.0f;
static unsigned s_rng = 0x2545F491u;
static unsigned s_ticks_ms;

static unsigned ru16(const unsigned char *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned ru32(const unsigned char *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static float rf32(const unsigned char *p)
{
	unsigned u = ru32(p);
	float f;
	memcpy(&f, &u, 4);
	return f;
}

static unsigned ru16(const unsigned char *p);
static int is_arr(const struct Val &v);
static int alen(const struct Val &v);
static struct Val aget(const struct Val &v, int i);

static __attribute__((noinline, cold)) void fail(const char *msg)
{
	if (!s_err[0]) {
		strncpy(s_err, msg, sizeof(s_err) - 1);
		printf("GDIR: %s\n", s_err);
	}
}

/* ------------------------------------------------------------ values */
static Val vnil(void)
{
	Val v;
	v.t = T_NIL;
	v.v[0] = 0.0f;
	v.v[1] = 0.0f;
	v.v[2] = 0.0f;
	return v;
}

static Val vbool(int b)
{
	Val v = vnil();
	v.t = T_BOOL;
	v.i = b ? 1 : 0;
	return v;
}

static Val vint(int i)
{
	Val v = vnil();
	v.t = T_INT;
	v.i = i;
	return v;
}

static Val vfloat(float f)
{
	Val v = vnil();
	v.t = T_FLOAT;
	v.f = f;
	return v;
}

static Val vvec(int t, float x, float y, float z)
{
	Val v = vnil();
	v.t = (unsigned char)t;
	v.v[0] = x;
	v.v[1] = y;
	v.v[2] = z;
	return v;
}

static Val vnode(int n)
{
	Val v = vnil();
	v.t = T_NODE;
	v.i = n;
	return v;
}

static char *ring_str(void)
{
	char *s = s_ring[s_ring_i++ % GDIR_RING];
	s[0] = 0;
	return s;
}

static Val vstr(const char *s)
{
	Val v = vnil();
	v.t = T_STR;
	v.s = s ? s : "";
	return v;
}

static int is_arr(const Val &v)
{
	return (v.t == T_ARR || v.t == T_KARR) && v.i >= 0;
}

static int alen(const Val &v)
{
	if (v.t == T_ARR && v.i >= 0) {
		return s_arr_n[v.i];
	}
	if (v.t == T_KARR && v.i >= 0) {
		return (int)ru16(s_karr[v.i]);
	}
	return 0;
}

static Val aget(const Val &v, int i)
{
	if (i < 0 || i >= alen(v)) {
		return vnil();
	}
	if (v.t == T_ARR) {
		return s_arr[v.i][i];
	}
	const unsigned k = ru16(s_karr[v.i] + 2 + i * 2);
	return k < s_k_n ? s_k[k] : vnil();
}

static int is_num(const Val &v)
{
	return v.t == T_INT || v.t == T_FLOAT || v.t == T_BOOL;
}

static float num(const Val &v)
{
	if (v.t == T_FLOAT) {
		return v.f;
	}
	if (v.t == T_INT || v.t == T_BOOL) {
		return (float)v.i;
	}
	return 0.0f;
}

static int inum(const Val &v)
{
	if (v.t == T_FLOAT) {
		return (int)v.f;
	}
	if (v.t == T_INT || v.t == T_BOOL) {
		return v.i;
	}
	return 0;
}

static int truthy(const Val &v)
{
	switch (v.t) {
		case T_NIL:
			return 0;
		case T_BOOL:
		case T_INT:
			return v.i != 0;
		case T_FLOAT:
			return v.f != 0.0f;
		case T_STR:
			return v.s && v.s[0];
		case T_VEC2:
			return v.v[0] != 0.0f || v.v[1] != 0.0f;
		case T_VEC3:
			return v.v[0] != 0.0f || v.v[1] != 0.0f || v.v[2] != 0.0f;
		case T_NODE:
			return v.i >= 0;
		case T_ARR:
		case T_KARR:
			return alen(v) > 0;
	}
	return 0;
}

static void fmt_float(char *o, unsigned n, float f)
{
	/* GDScript prints floats with at least one decimal. */
	if (f == (float)(int)f && f > -1.0e7f && f < 1.0e7f) {
		snprintf(o, n, "%d.0", (int)f);
		return;
	}
	snprintf(o, n, "%.6g", (double)f);
}

static const char *to_text(const Val &v, char *o, unsigned n)
{
	switch (v.t) {
		case T_NIL:
			snprintf(o, n, "<null>");
			break;
		case T_BOOL:
			snprintf(o, n, "%s", v.i ? "true" : "false");
			break;
		case T_INT:
			snprintf(o, n, "%d", v.i);
			break;
		case T_FLOAT:
			fmt_float(o, n, v.f);
			break;
		case T_STR:
			return v.s;
		case T_VEC2: {
			char a[24], b[24];
			fmt_float(a, sizeof(a), v.v[0]);
			fmt_float(b, sizeof(b), v.v[1]);
			snprintf(o, n, "(%s, %s)", a, b);
		} break;
		case T_VEC3: {
			char a[24], b[24], c[24];
			fmt_float(a, sizeof(a), v.v[0]);
			fmt_float(b, sizeof(b), v.v[1]);
			fmt_float(c, sizeof(c), v.v[2]);
			snprintf(o, n, "(%s, %s, %s)", a, b, c);
		} break;
		case T_NODE:
			snprintf(o, n, "<node %d>", v.i);
			break;
		case T_ARR:
		case T_KARR:
			snprintf(o, n, "[array]");
			break;
	}
	return o;
}

static int in_buf(const char *s, const void *base, unsigned long n)
{
	const char *b = (const char *)base;
	return s >= b && s < b + n;
}

static int in_ring(const char *s)
{
	return in_buf(s, s_ring, sizeof(s_ring));
}

/* Store a value somewhere that outlives the frame: strings from the temp ring
 * (or another slot) are copied into the slot's own buffer. */
static Val persist(const Val &v, char *buf, unsigned n)
{
	if (v.t == T_STR && v.s != buf &&
			(in_ring(v.s) || in_buf(v.s, s_memstr, sizeof(s_memstr)) || in_buf(v.s, s_arrstr, sizeof(s_arrstr)))) {
		strncpy(buf, v.s, n - 1);
		buf[n - 1] = 0;
		return vstr(buf);
	}
	return v;
}

static int vequal(const Val &a, const Val &b)
{
	if (is_num(a) && is_num(b)) {
		if (a.t == T_FLOAT || b.t == T_FLOAT) {
			return num(a) == num(b);
		}
		return a.i == b.i;
	}
	if (a.t != b.t) {
		return 0;
	}
	switch (a.t) {
		case T_NIL:
			return 1;
		case T_STR:
			return strcmp(a.s, b.s) == 0;
		case T_VEC2:
			return a.v[0] == b.v[0] && a.v[1] == b.v[1];
		case T_VEC3:
			return a.v[0] == b.v[0] && a.v[1] == b.v[1] && a.v[2] == b.v[2];
		case T_NODE:
		case T_ARR:
		case T_KARR:
			return a.i == b.i;
	}
	return 0;
}

static unsigned s_mem_n_gc;
static unsigned s_sp_gc;
static unsigned s_fsp_gc;
static Val *s_mem_gc;
static Val *s_stack_gc;

static void arr_mark(int a, unsigned char *mark)
{
	if (a < 0 || a >= GDIR_ARRAYS || mark[a]) {
		return;
	}
	mark[a] = 1;
	for (int i = 0; i < s_arr_n[a]; i++) {
		if (s_arr[a][i].t == 8 /* T_ARR */) {
			arr_mark(s_arr[a][i].i, mark);
		}
	}
}

/* Arrays live in a small pool: anything not reachable from members or the
 * live stack is free again (locals never outlive a call). */
static void arr_gc(void)
{
	unsigned char mark[GDIR_ARRAYS];
	memset(mark, 0, sizeof(mark));
	for (unsigned i = 0; i < s_mem_n_gc; i++) {
		if (s_mem_gc[i].t == 8) {
			arr_mark(s_mem_gc[i].i, mark);
		}
	}
	for (unsigned i = 0; i < s_sp_gc; i++) {
		if (s_stack_gc[i].t == 8) {
			arr_mark(s_stack_gc[i].i, mark);
		}
	}
#ifdef GDIR_FAST_STACK_AT
	for (unsigned i = 0; i < s_fsp_gc; i++) {
		if (s_fast[i].t == 8) {
			arr_mark(s_fast[i].i, mark);
		}
	}
#endif
	for (int i = 0; i < GDIR_ARRAYS; i++) {
		s_arr_used[i] = mark[i];
	}
}

static unsigned *s_sp_live;
static int arr_new(void)
{
	if (s_sp_live) {
		s_sp_gc = *s_sp_live;
		s_fsp_gc = s_fsp;
	}
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < GDIR_ARRAYS; i++) {
			if (!s_arr_used[i]) {
				s_arr_used[i] = 1;
				s_arr_n[i] = 0;
				return i;
			}
		}
		arr_gc();
	}
	char m[48];
	snprintf(m, sizeof(m), "out of arrays (%d live)", GDIR_ARRAYS);
	fail(m);
	return -1;
}

static void arr_set(int a, int i, const Val &v)
{
	if (a < 0 || i < 0 || i >= GDIR_ARR_LEN) {
		return;
	}
	s_arr[a][i] = persist(v, s_arrstr[a][i], sizeof(s_arrstr[a][i]));
}

/* ------------------------------------------------------------ nodes */
static void mat_from_trs(const float *p, const float *r, const float *s, float *m)
{
	/* Godot Basis::from_euler(YXZ): Ry * Rx * Rz, then scale columns. */
	if (r[0] == 0.0f && r[2] == 0.0f) {
		/* a yaw only (most moving nodes): Ry */
		const float cy = cosf(r[1]), sy = sinf(r[1]);
		const float s0 = s[0], s1 = s[1], s2 = s[2];
		const int one = s0 == 1.0f && s1 == 1.0f && s2 == 1.0f;
		m[0] = one ? cy : cy * s0;
		m[1] = 0.0f;
		m[2] = one ? sy : sy * s2;
		m[3] = p[0];
		m[4] = 0.0f;
		m[5] = s1;
		m[6] = 0.0f;
		m[7] = p[1];
		m[8] = one ? -sy : -sy * s0;
		m[9] = 0.0f;
		m[10] = one ? cy : cy * s2;
		m[11] = p[2];
		if (r[1] == 0.0f) {
			m[0] = s0;
			m[2] = 0.0f;
			m[8] = 0.0f;
			m[10] = s2;
		}
		return;
	}
	const float cx = cosf(r[0]), sx = sinf(r[0]);
	const float cy = cosf(r[1]), sy = sinf(r[1]);
	const float cz = cosf(r[2]), sz = sinf(r[2]);
	const float m00 = cy * cz + sy * sx * sz;
	const float m01 = cz * sy * sx - cy * sz;
	const float m02 = cx * sy;
	const float m10 = cx * sz;
	const float m11 = cx * cz;
	const float m12 = -sx;
	const float m20 = cy * sx * sz - cz * sy;
	const float m21 = cy * cz * sx + sy * sz;
	const float m22 = cy * cx;
	m[0] = m00 * s[0];
	m[1] = m01 * s[1];
	m[2] = m02 * s[2];
	m[3] = p[0];
	m[4] = m10 * s[0];
	m[5] = m11 * s[1];
	m[6] = m12 * s[2];
	m[7] = p[1];
	m[8] = m20 * s[0];
	m[9] = m21 * s[1];
	m[10] = m22 * s[2];
	m[11] = p[2];
}

static void mat_mul34(const float *a, const float *b, float *o)
{
	float t[12];
	for (int r = 0; r < 3; r++) {
		const float a0 = a[r * 4 + 0], a1 = a[r * 4 + 1], a2 = a[r * 4 + 2];
		t[r * 4 + 0] = a0 * b[0] + a1 * b[4] + a2 * b[8];
		t[r * 4 + 1] = a0 * b[1] + a1 * b[5] + a2 * b[9];
		t[r * 4 + 2] = a0 * b[2] + a1 * b[6] + a2 * b[10];
		t[r * 4 + 3] = a0 * b[3] + a1 * b[7] + a2 * b[11] + a[r * 4 + 3];
	}
	memcpy(o, t, sizeof(t));
}

static void mat_inv34(const float *m, float *o)
{
	const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
	const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
	float det = a * A + b * B + c * C;
	if (det > -1e-12f && det < 1e-12f) {
		det = 1e-12f;
	}
	const float id = 1.0f / det;
	o[0] = A * id;
	o[1] = -(b * i - c * h) * id;
	o[2] = (b * f - c * e) * id;
	o[4] = B * id;
	o[5] = (a * i - c * g) * id;
	o[6] = -(a * f - c * d) * id;
	o[8] = C * id;
	o[9] = -(a * h - b * g) * id;
	o[10] = (a * e - b * d) * id;
	const float tx = m[3], ty = m[7], tz = m[11];
	o[3] = -(o[0] * tx + o[1] * ty + o[2] * tz);
	o[7] = -(o[4] * tx + o[5] * ty + o[6] * tz);
	o[11] = -(o[8] * tx + o[9] * ty + o[10] * tz);
}

/* Godot Basis::get_euler(YXZ) for a rotation matrix (row major 3x4). */
static void euler_yxz(const float *m, float *r)
{
	const float m12 = m[6];
	if (m12 < 0.9999f) {
		if (m12 > -0.9999f) {
			r[0] = asinf(-m12);
			r[1] = atan2f(m[2], m[10]);
			r[2] = atan2f(m[4], m[5]);
		} else {
			r[0] = 1.5707963f;
			r[1] = atan2f(m[1], m[0]);
			r[2] = 0.0f;
		}
	} else {
		r[0] = -1.5707963f;
		r[1] = -atan2f(m[1], m[0]);
		r[2] = 0.0f;
	}
}

static int node_ok(int n)
{
	return n >= 0 && (unsigned)n < s_node_n;
}

/* the cameras and their ancestors: their worlds are never left stale */
static unsigned char s_keep[GDIR_MAX_NODES];

static void mark_keep(int n)
{
	while (node_ok(n)) {
		s_keep[n] = 1;
		const int p = s_nodes[n].parent;
		if (p < 0 || p >= n) {
			break;
		}
		n = p;
	}
}

static int node_parent(int n)
{
	return s_par[n];
}

static int node_stale(int n)
{
	const int p = node_parent(n);
	return s_ndirty[n] || (p >= 0 && s_pver[n] != s_wver[p]);
}

/* n's world from its parent's (already current) and its own transform */
#ifdef GDIR_STATS
static unsigned s_st_computed, s_st_sent, s_st_passes;
#endif
static void compute_one(int n)
{
#ifdef GDIR_STATS
	s_st_computed++;
#endif
	PackNode *pn = &s_nodes[n];
	const int p = node_parent(n);
	if (!(pn->flags & 2)) {
		/* Non-3D nodes pass the parent's transform through. */
		if (p >= 0) {
			memcpy(s_world[n], s_world[p], sizeof(s_world[n]));
		} else {
			const float id[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
			memcpy(s_world[n], id, sizeof(id));
		}
	} else {
		/* the local matrix only when the node's own TRS changed */
		if (s_lstale[n]) {
			mat_from_trs(pn->lpos, pn->lrot, pn->lscale, s_local[n]);
			s_lstale[n] = 0;
			const float *l = s_local[n];
			s_lmove[n] = l[0] == 1.0f && l[5] == 1.0f && l[10] == 1.0f && l[1] == 0.0f && l[2] == 0.0f && l[4] == 0.0f &&
					l[6] == 0.0f && l[8] == 0.0f && l[9] == 0.0f;
			s_lident[n] = s_lmove[n] && l[3] == 0.0f && l[7] == 0.0f && l[11] == 0.0f;
		}
		if (p >= 0 && s_lident[n]) {
			memcpy(s_world[n], s_world[p], sizeof(s_world[n])); /* a child at its parent's origin */
		} else if (p >= 0 && s_lmove[n]) {
			/* a child moved but not turned: the parent's basis, its origin moved */
			const float *w = s_world[p], *l = s_local[n];
			float *o = s_world[n];
			const float x = l[3], y = l[7], z = l[11];
			for (int r = 0; r < 3; r++) {
				o[r * 4 + 0] = w[r * 4 + 0];
				o[r * 4 + 1] = w[r * 4 + 1];
				o[r * 4 + 2] = w[r * 4 + 2];
				o[r * 4 + 3] = w[r * 4 + 0] * x + w[r * 4 + 1] * y + w[r * 4 + 2] * z + w[r * 4 + 3];
			}
		} else if (p >= 0) {
			mat_mul34(s_world[p], s_local[n], s_world[n]);
		} else {
			memcpy(s_world[n], s_local[n], sizeof(s_local[n]));
		}
	}
	s_pver[n] = p >= 0 ? s_wver[p] : 0;
	s_wver[n]++;
	s_ndirty[n] = 0;
}

static void recompute_nodes(void);

/* n's world up to date for a script that reads it: n and its parents only */
static void world_fresh(int n)
{
	if (!node_ok(n)) {
		return;
	}
	if (s_all_dirty) {
		recompute_nodes();
		return;
	}
	int chain[64];
	int k = 0;
	for (int c = n; c >= 0 && k < 64; c = node_parent(c)) {
		chain[k++] = c;
	}
	/* from the top down, redo each stale one (a redone parent makes the
	 * rest stale) */
	for (int i = k - 1; i >= 0; i--) {
		if (node_stale(chain[i])) {
			compute_one(chain[i]);
		}
	}
}

/* visible: the node and all its parents */
static int node_visible(int n)
{
	for (int c = n; c >= 0; c = node_parent(c)) {
		if (!(s_nodes[c].flags & 1)) {
			return 0;
		}
	}
	return 1;
}

/* the node pass, first half: visibility and worlds for nodes [a, b) */
static void pass_worlds(unsigned a, unsigned b, int all)
{
	for (unsigned n = a; n < b; n++) {
		const PackNode *pn = &s_nodes[n];
		const int p = node_parent((int)n);
		const int pvis = p >= 0 ? s_vis[p] : 1;
		s_vis[n] = (unsigned char)(((pn->flags & 1) && pvis) ? 1 : 0);
		if (all) {
			s_lstale[n] = 1;
			compute_one((int)n);
			continue;
		}
		/* a hidden node's world waits until it is shown (its children are
		 * hidden too, and wait with it); the cameras never wait */
		if (node_stale((int)n) && (s_vis[n] || s_keep[n])) {
			compute_one((int)n);
		}
	}
}

/* second half: what changed to the renderer */
static void pass_send(unsigned a, unsigned b, int all)
{
	for (unsigned n = a; n < b; n++) {
		const PackNode *pn = &s_nodes[n];
		/* visibility only when it changed (or the pack is new) */
		if (all || s_vis_sent[n] != s_vis[n]) {
			s_vis_sent[n] = s_vis[n];
			gs_draw_node_hidden((int)n, !s_vis[n]);
			if (pn->hud >= 0) {
				sys_io_hud_set_hidden(pn->hud, !s_vis[n]);
			}
		}
		if (s_sent[n] == s_wver[n] && !all) {
			continue; /* the renderer keeps the matrix it has */
		}
		s_sent[n] = s_wver[n];
		if (!all && !node_drawn((int)n)) {
			continue; /* nothing is drawn with it */
		}
#ifdef GDIR_STATS
		s_st_sent++;
#endif
		if (!(pn->flags & 2)) {
			gs_draw_node_matrix((int)n, 0);
			continue;
		}
		float d[12];
		mat_mul34(s_world[n], s_w0inv[n], d);
		const float eps = 1e-4f;
		const int ident = fabsf(d[0] - 1) < eps && fabsf(d[5] - 1) < eps && fabsf(d[10] - 1) < eps && fabsf(d[1]) < eps &&
				fabsf(d[2]) < eps && fabsf(d[4]) < eps && fabsf(d[6]) < eps && fabsf(d[8]) < eps && fabsf(d[9]) < eps &&
				fabsf(d[3]) < eps && fabsf(d[7]) < eps && fabsf(d[11]) < eps;
		gs_draw_node_matrix((int)n, ident ? 0 : d);
	}
}

static void recompute_nodes(void)
{
	/* Parents come before children, so one pass carries a change down: a
	 * node's world is redone when it or an ancestor moved. */
	const int all = s_all_dirty;
	const int full = all || s_pass_full || !s_preorder || s_mark_n > GDIR_MARKS;
	if (full) {
		memset(s_keep, 0, s_node_n);
		if (s_cur >= 0 && s_cur < GDIR_MAX_PACKS) {
			mark_keep(s_pack[s_cur].camera);
		}
		for (int v = 0; v < s_view_n; v++) {
			mark_keep(s_view_cam[v]);
		}
		pass_worlds(0, s_node_n, all);
		pass_send(0, s_node_n, all);
	} else {
		/* each moved node's subtree (overlaps only repeat cheap checks) */
		for (unsigned i = 0; i < s_mark_n; i++) {
			const unsigned n = (unsigned)s_mark[i];
			if (n < s_node_n) {
				pass_worlds(n, s_subend[n], 0);
			}
		}
		for (unsigned i = 0; i < s_mark_n; i++) {
			const unsigned n = (unsigned)s_mark[i];
			if (n < s_node_n) {
				pass_send(n, s_subend[n], 0);
			}
		}
	}
	s_all_dirty = 0;
	s_pass_full = 0;
	s_mark_n = 0;
	const Pack *pk = &s_pack[s_cur];
	for (int v = 0; v < s_view_n; v++) {
		const int cn = s_view_cam[v];
		if (!node_ok(cn)) {
			continue;
		}
		const float *w = s_world[cn];
		float fx = -w[2], fy = -w[6], fz = -w[10];
		const float l = sqrtf(fx * fx + fy * fy + fz * fz);
		if (l > 1e-6f) {
			fx /= l;
			fy /= l;
			fz /= l;
		}
		gs_draw_view_look(v, w[3], w[7], w[11], w[3] + fx * 10.0f, w[7] + fy * 10.0f, w[11] + fz * 10.0f, pk->fov);
	}
	if (node_ok(pk->camera)) {
		const float *w = s_world[pk->camera];
		float fx = -w[2], fy = -w[6], fz = -w[10];
		const float l = sqrtf(fx * fx + fy * fy + fz * fz);
		if (l > 1e-6f) {
			fx /= l;
			fy /= l;
			fz /= l;
		}
		gs_draw_camera_look(w[3], w[7], w[11], w[3] + fx * 10.0f, w[7] + fy * 10.0f, w[11] + fz * 10.0f, pk->fov);
	}
	s_dirty = 0;
}

static int hud_of(int n)
{
	return node_ok(n) ? s_nodes[n].hud : -1;
}

static Val get_prop(const Val &o, int prop)
{
	if (o.t == T_VEC2 || o.t == T_VEC3) {
		if (prop == GP_X || prop == GP_R) {
			return vfloat(o.v[0]);
		}
		if (prop == GP_Y || prop == GP_G) {
			return vfloat(o.v[1]);
		}
		if ((prop == GP_Z || prop == GP_B) && o.t == T_VEC3) {
			return vfloat(o.v[2]);
		}
		return vnil();
	}
	if (o.t != T_NODE || !node_ok(o.i)) {
		fail("property on a non-node value");
		return vnil();
	}
	const PackNode *pn = &s_nodes[o.i];
	const int hud = pn->hud;
	switch (prop) {
		case GP_POSITION:
			if (pn->flags & 4) {
				return vvec(T_VEC2, pn->lpos[0], pn->lpos[1], 0.0f);
			}
			return vvec(T_VEC3, pn->lpos[0], pn->lpos[1], pn->lpos[2]);
		case GP_ROTATION:
			return vvec(T_VEC3, pn->lrot[0], pn->lrot[1], pn->lrot[2]);
		case GP_ROTATION_DEGREES:
			return vvec(T_VEC3, pn->lrot[0] * 57.29578f, pn->lrot[1] * 57.29578f, pn->lrot[2] * 57.29578f);
		case GP_SCALE:
			return vvec(T_VEC3, pn->lscale[0], pn->lscale[1], pn->lscale[2]);
		case GP_VISIBLE:
			return vbool(pn->flags & 1);
		case GP_TEXT:
			return vstr(hud >= 0 ? sys_io_hud_text(hud) : "");
		case GP_GLOBAL_POSITION:
			world_fresh(o.i);
			if (pn->flags & 4) {
				return vvec(T_VEC2, pn->w0[3] - pn->w0[0] + pn->lpos[0], pn->w0[7] - pn->w0[1] + pn->lpos[1], 0.0f);
			}
			return vvec(T_VEC3, s_world[o.i][3], s_world[o.i][7], s_world[o.i][11]);
		case GP_SIZE:
			return vvec(T_VEC2, pn->lscale[0], pn->lscale[1], 0.0f);
		default:
			break;
	}
	fail("unsupported node property");
	return vnil();
}

static void control_pos_changed(int n)
{
	PackNode *pn = &s_nodes[n];
	if (pn->hud < 0) {
		return;
	}
	/* w0[3], w0[7] keep the cooked global position; w0[0], w0[1] the cooked local. */
	const int x = (int)(pn->w0[3] - pn->w0[0] + pn->lpos[0]);
	const int y = (int)(pn->w0[7] - pn->w0[1] + pn->lpos[1]);
	sys_io_hud_set_pos(pn->hud, x, y);
}

static Val set_prop(Val o, int prop, const Val &v)
{
	if (o.t == T_VEC2 || o.t == T_VEC3) {
		const int k = (prop == GP_X || prop == GP_R) ? 0 : ((prop == GP_Y || prop == GP_G) ? 1 : 2);
		o.v[k] = num(v);
		return o;
	}
	if (o.t != T_NODE || !node_ok(o.i)) {
		fail("property set on a non-node value");
		return o;
	}
	PackNode *pn = &s_nodes[o.i];
	switch (prop) {
		case GP_POSITION:
			pn->lpos[0] = v.v[0];
			pn->lpos[1] = v.v[1];
			if (pn->flags & 4) {
				control_pos_changed(o.i);
			} else {
				pn->lpos[2] = v.t == T_VEC3 ? v.v[2] : 0.0f;
				mark_node(o.i);
			}
			break;
		case GP_ROTATION:
			memcpy(pn->lrot, v.v, sizeof(pn->lrot));
			mark_node(o.i);
			break;
		case GP_ROTATION_DEGREES:
			for (int k = 0; k < 3; k++) {
				pn->lrot[k] = v.v[k] * 0.01745329f;
			}
			mark_node(o.i);
			break;
		case GP_SCALE:
			memcpy(pn->lscale, v.v, sizeof(pn->lscale));
			mark_node(o.i);
			break;
		case GP_VISIBLE: {
			const unsigned char f = (unsigned char)(truthy(v) ? (pn->flags | 1) : (pn->flags & ~1));
			if (f != pn->flags) {
				pn->flags = f;
				s_dirty = 1;
				s_pass_full = 1;
			}
		} break;
		case GP_TEXT:
			if (pn->hud >= 0) {
				char tmp[GDIR_STR];
				sys_io_set_hud_text(pn->hud, to_text(v, tmp, sizeof(tmp)));
			}
			break;
		case GP_COLOR:
			if (pn->hud >= 0) {
				sys_io_hud_set_color(pn->hud, (int)(v.v[0] * 255.0f), (int)(v.v[1] * 255.0f), (int)(v.v[2] * 255.0f));
			}
			break;
		default:
			fail("unsupported node property set");
			break;
	}
	return o;
}

/* ------------------------------------------------------------ input */
static int action_id(const char *a)
{
	static const struct {
		const char *name;
		int id;
	} map[] = {
		{ "ui_left", 0 },
		{ "ui_right", 1 },
		{ "ui_up", 2 },
		{ "ui_down", 3 },
		{ "ui_accept", 4 },
		{ "ui_cancel", 5 },
		{ "ui_square", 6 },
		{ "ui_triangle", 7 },
		{ "ui_focus_prev", 8 },
		{ "ui_focus_next", 9 },
		{ "ui_l2", 10 },
		{ "ui_r2", 11 },
		{ "ui_select", 12 },
		{ "ui_back", 13 },
	};
	for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
		if (strcmp(a, map[i].name) == 0) {
			return map[i].id;
		}
	}
	return -1;
}

static float action_strength(const char *a)
{
	const int id = action_id(a);
	if (id >= 0 && pad_io_pressed(id)) {
		return 1.0f;
	}
	float sx = 0.0f, sy = 0.0f;
	pad_io_stick(0, &sx, &sy);
	if (id == 0 && sx < 0.0f) {
		return -sx;
	}
	if (id == 1 && sx > 0.0f) {
		return sx;
	}
	if (id == 2 && sy < 0.0f) {
		return -sy;
	}
	if (id == 3 && sy > 0.0f) {
		return sy;
	}
	return 0.0f;
}

/* ------------------------------------------------------------ strings */
static Val str_format(const char *fmt, const Val &arg)
{
	char *o = ring_str();
	unsigned n = 0;
	int ai = 0;
	for (const char *p = fmt; *p && n + 1 < GDIR_STR; p++) {
		if (*p != '%') {
			o[n++] = *p;
			continue;
		}
		p++;
		if (*p == '%') {
			o[n++] = '%';
			continue;
		}
		char spec[16];
		unsigned k = 0;
		spec[k++] = '%';
		while (*p && strchr("0123456789.-+ ", *p) && k < 12) {
			spec[k++] = *p++;
		}
		const char conv = *p;
		Val v = arg;
		if (is_arr(arg)) {
			v = aget(arg, (int)ai);
		}
		ai++;
		char piece[GDIR_STR];
		if (conv == 'd') {
			spec[k++] = 'd';
			spec[k] = 0;
			snprintf(piece, sizeof(piece), spec, inum(v));
		} else if (conv == 'f') {
			spec[k++] = 'f';
			spec[k] = 0;
			snprintf(piece, sizeof(piece), spec, (double)num(v));
		} else {
			char t[GDIR_STR];
			spec[k++] = 's';
			spec[k] = 0;
			snprintf(piece, sizeof(piece), spec, to_text(v, t, sizeof(t)));
		}
		for (const char *q = piece; *q && n + 1 < GDIR_STR; q++) {
			o[n++] = *q;
		}
		if (!*p) {
			break;
		}
	}
	o[n] = 0;
	return vstr(o);
}

/* ------------------------------------------------------------ arithmetic */
static Val arith(int op, const Val &a, const Val &b)
{
	if (op == GOP_ADD && (a.t == T_STR || b.t == T_STR)) {
		char *o = ring_str();
		char ta[GDIR_STR], tb[GDIR_STR];
		snprintf(o, GDIR_STR, "%s%s", to_text(a, ta, sizeof(ta)), to_text(b, tb, sizeof(tb)));
		return vstr(o);
	}
	if (op == GOP_MOD && a.t == T_STR) {
		return str_format(a.s, b);
	}
	if ((a.t == T_VEC2 || a.t == T_VEC3) || (b.t == T_VEC2 || b.t == T_VEC3)) {
		const int t = (a.t == T_VEC3 || b.t == T_VEC3) ? T_VEC3 : T_VEC2;
		Val r = vvec(t, 0, 0, 0);
		for (int k = 0; k < 3; k++) {
			const float x = (a.t == T_VEC2 || a.t == T_VEC3) ? a.v[k] : num(a);
			const float y = (b.t == T_VEC2 || b.t == T_VEC3) ? b.v[k] : num(b);
			switch (op) {
				case GOP_ADD: r.v[k] = x + y; break;
				case GOP_SUB: r.v[k] = x - y; break;
				case GOP_MUL: r.v[k] = x * y; break;
				case GOP_DIV: r.v[k] = y != 0.0f ? x / y : 0.0f; break;
				default: break;
			}
		}
		if (t == T_VEC2) {
			r.v[2] = 0.0f;
		}
		return r;
	}
	if (a.t == T_FLOAT || b.t == T_FLOAT) {
		const float x = num(a), y = num(b);
		switch (op) {
			case GOP_ADD: return vfloat(x + y);
			case GOP_SUB: return vfloat(x - y);
			case GOP_MUL: return vfloat(x * y);
			case GOP_DIV: return vfloat(y != 0.0f ? x / y : 0.0f);
			case GOP_MOD: return vfloat(y != 0.0f ? fmodf(x, y) : 0.0f);
		}
		return vnil();
	}
	const int x = inum(a), y = inum(b);
	switch (op) {
		case GOP_ADD: return vint(x + y);
		case GOP_SUB: return vint(x - y);
		case GOP_MUL: return vint(x * y);
		case GOP_DIV: return vint(y ? x / y : 0);
		case GOP_MOD: return vint(y ? x % y : 0);
		case GOP_BAND: return vint(x & y);
		case GOP_BOR: return vint(x | y);
	}
	return vnil();
}

/* -1, 0, 1 as x < y, neither, x > y (a NaN: 0). The PS1 has no FPU: its
 * float compares are calls, so it compares the bits, as sign and size. */
static inline int fcmp(float x, float y)
{
#ifdef BLAZIUM_PS1_GDIR
	unsigned ux, uy;
	memcpy(&ux, &x, 4);
	memcpy(&uy, &y, 4);
	if ((ux & 0x7fffffffu) > 0x7f800000u || (uy & 0x7fffffffu) > 0x7f800000u) {
		return 0;
	}
	/* subnormals are zero, as ps1_float.c has them */
	const int mx = (ux & 0x7f800000u) ? (int)(ux & 0x7fffffffu) : 0;
	const int my = (uy & 0x7f800000u) ? (int)(uy & 0x7fffffffu) : 0;
	const int kx = (ux & 0x80000000u) ? -mx : mx;
	const int ky = (uy & 0x80000000u) ? -my : my;
	return (kx > ky) - (kx < ky);
#else
	return x < y ? -1 : (x > y ? 1 : 0);
#endif
}

static int compare(int op, const Val &a, const Val &b)
{
	if (op == GOP_EQ) {
		return vequal(a, b);
	}
	if (op == GOP_NE) {
		return !vequal(a, b);
	}
	int c;
	if (a.t == T_STR && b.t == T_STR) {
		c = strcmp(a.s, b.s);
	} else {
		const float x = num(a), y = num(b);
		c = fcmp(x, y);
	}
	switch (op) {
		case GOP_LT: return c < 0;
		case GOP_LE: return c <= 0;
		case GOP_GT: return c > 0;
		case GOP_GE: return c >= 0;
	}
	return 0;
}

/* ------------------------------------------------------------ natives */
static Val call(unsigned inst, unsigned func, const Val *args, int argc);
#ifdef GDIR_STATS
static unsigned s_fn_ops[GDIR_MAX_FUNCS]; /* inclusive, since the last report */
#if defined(GDIR_STATS) && defined(GDIR_CLOCK)
static unsigned s_fn_self[GDIR_MAX_FUNCS]; /* clock ticks in the function itself */
static unsigned s_child_ticks; /* the running function's callees' so far */
#endif
#ifdef GDIR_CLOCK
extern "C" unsigned GDIR_CLOCK(void);
/* GDIR_CLOCK(): a 16-bit tick counter the guest provides; time in natives */
static unsigned s_nat_time[GN_COUNT], s_nat_calls[GN_COUNT];
static unsigned s_recompute_ticks; /* the frame-end node pass */
static unsigned s_script_ticks; /* _process and _physics_process */
#endif
#endif
static unsigned s_ops_last; /* interpreter ops of the last frame (GS_FRAMELOG) */
unsigned gdir_vm_last_ops(void);

static float frand(void)
{
	s_rng = s_rng * 1664525u + 1013904223u;
	return (float)((s_rng >> 8) & 0xffffff) / 16777216.0f;
}

static void rotate_node(int n, int axis, float a)
{
	if (!node_ok(n)) {
		return;
	}
	PackNode *pn = &s_nodes[n];
	if (axis == 1 && pn->lrot[0] == 0.0f && pn->lrot[2] == 0.0f) {
		pn->lrot[1] += a;
		mark_node(n);
		return;
	}
	/* R' = R_axis(a) * R (parent space), back to YXZ euler. */
	const float one[3] = { 1, 1, 1 };
	const float zero[3] = { 0, 0, 0 };
	float m[12], r[12], o[12];
	mat_from_trs(zero, pn->lrot, one, m);
	float e[3] = { 0, 0, 0 };
	e[axis] = a;
	mat_from_trs(zero, e, one, r);
	mat_mul34(r, m, o);
	euler_yxz(o, pn->lrot);
	mark_node(n);
}

static Val native(unsigned inst, int id, Val *a, int n)
{
	(void)inst;
	char t0[GDIR_STR];
	switch (id) {
		case GN_PRINT: {
			char line[160];
			unsigned k = 0;
			line[0] = 0;
			for (int i = 0; i < n; i++) {
				const char *s = to_text(a[i], t0, sizeof(t0));
				for (; *s && k + 1 < sizeof(line); s++) {
					line[k++] = *s;
				}
			}
			line[k] = 0;
			printf("%s\n", line);
			return vnil();
		}
		case GN_STR: {
			char *o = ring_str();
			unsigned k = 0;
			for (int i = 0; i < n; i++) {
				const char *s = to_text(a[i], t0, sizeof(t0));
				for (; *s && k + 1 < GDIR_STR; s++) {
					o[k++] = *s;
				}
			}
			o[k] = 0;
			return vstr(o);
		}
		case GN_INT:
			if (n && a[0].t == T_STR) {
				return vint(atoi(a[0].s));
			}
			return vint(n ? inum(a[0]) : 0);
		case GN_FLOAT:
			if (n && a[0].t == T_STR) {
				return vfloat((float)atof(a[0].s));
			}
			return vfloat(n ? num(a[0]) : 0.0f);
		case GN_BOOL:
			return vbool(n ? truthy(a[0]) : 0);
		case GN_ABS:
			if (a[0].t == T_INT) {
				return vint(a[0].i < 0 ? -a[0].i : a[0].i);
			}
			return vfloat(fabsf(num(a[0])));
		case GN_SIGN:
			if (a[0].t == T_INT) {
				return vint(a[0].i > 0 ? 1 : (a[0].i < 0 ? -1 : 0));
			}
			return vfloat(num(a[0]) > 0 ? 1.0f : (num(a[0]) < 0 ? -1.0f : 0.0f));
		case GN_MIN:
		case GN_MAX: {
			Val best = a[0];
			for (int i = 1; i < n; i++) {
				const int better = id == GN_MIN ? compare(GOP_LT, a[i], best) : compare(GOP_GT, a[i], best);
				if (better) {
					best = a[i];
				}
			}
			if (best.t == T_INT) {
				for (int i = 0; i < n; i++) {
					if (a[i].t == T_FLOAT) {
						return vfloat(num(best));
					}
				}
			}
			return best;
		}
		case GN_CLAMP:
			if (a[0].t == T_INT && a[1].t == T_INT && a[2].t == T_INT) {
				int v = a[0].i;
				v = v < a[1].i ? a[1].i : (v > a[2].i ? a[2].i : v);
				return vint(v);
			} else {
				float v = num(a[0]);
				v = v < num(a[1]) ? num(a[1]) : (v > num(a[2]) ? num(a[2]) : v);
				return vfloat(v);
			}
		case GN_LERP:
		case GN_VEC_LERP:
			if (a[0].t == T_VEC2 || a[0].t == T_VEC3) {
				Val r = a[0];
				for (int k = 0; k < 3; k++) {
					r.v[k] = a[0].v[k] + (a[1].v[k] - a[0].v[k]) * num(a[2]);
				}
				return r;
			}
			return vfloat(num(a[0]) + (num(a[1]) - num(a[0])) * num(a[2]));
		case GN_MOVE_TOWARD: {
			const float f = num(a[0]), t = num(a[1]), d = num(a[2]);
			if (fabsf(t - f) <= d) {
				return vfloat(t);
			}
			return vfloat(f + (t > f ? d : -d));
		}
		case GN_SIN: return vfloat(sinf(num(a[0])));
		case GN_COS: return vfloat(cosf(num(a[0])));
		case GN_TAN: return vfloat(tanf(num(a[0])));
		case GN_ASIN: return vfloat(asinf(num(a[0])));
		case GN_ACOS: return vfloat(acosf(num(a[0])));
		case GN_EXP: return vfloat(expf(num(a[0])));
		case GN_ATAN2: return vfloat(atan2f(num(a[0]), num(a[1])));
		case GN_SQRT: return vfloat(sqrtf(num(a[0])));
		case GN_POW: return vfloat(powf(num(a[0]), num(a[1])));
		case GN_FLOOR: return a[0].t == T_INT ? a[0] : vfloat(floorf(num(a[0])));
		case GN_CEIL: return a[0].t == T_INT ? a[0] : vfloat(ceilf(num(a[0])));
		case GN_ROUND: return a[0].t == T_INT ? a[0] : vfloat(floorf(num(a[0]) + 0.5f));
		case GN_RANDF: return vfloat(frand());
		case GN_RANDI: {
			s_rng = s_rng * 1664525u + 1013904223u;
			return vint((int)(s_rng >> 1));
		}
		case GN_RANDF_RANGE: return vfloat(num(a[0]) + (num(a[1]) - num(a[0])) * frand());
		case GN_RANDI_RANGE: {
			const int lo = inum(a[0]), hi = inum(a[1]);
			const int span = hi - lo + 1;
			return vint(span > 0 ? lo + (int)(frand() * (float)span) % span : lo);
		}
		case GN_FMOD: return vfloat(fmodf(num(a[0]), num(a[1])));
		case GN_POSMOD: {
			if (a[0].t == T_INT && a[1].t == T_INT) {
				const int m = a[1].i ? a[0].i % a[1].i : 0;
				return vint(m < 0 ? m + a[1].i : m);
			}
			float m = fmodf(num(a[0]), num(a[1]));
			if (m < 0) {
				m += num(a[1]);
			}
			return vfloat(m);
		}
		case GN_WRAPI: {
			const int v = inum(a[0]), lo = inum(a[1]), hi = inum(a[2]);
			const int r = hi - lo;
			if (r <= 0) {
				return vint(lo);
			}
			int m = (v - lo) % r;
			if (m < 0) {
				m += r;
			}
			return vint(lo + m);
		}
		case GN_WRAPF: {
			const float v = num(a[0]), lo = num(a[1]), hi = num(a[2]);
			const float r = hi - lo;
			if (r == 0.0f) {
				return vfloat(lo);
			}
			float m = fmodf(v - lo, r);
			if (m < 0) {
				m += r;
			}
			return vfloat(lo + m);
		}
		case GN_DEG_TO_RAD: return vfloat(num(a[0]) * 0.01745329f);
		case GN_RAD_TO_DEG: return vfloat(num(a[0]) * 57.29578f);
		case GN_IS_EQUAL_APPROX: return vbool(fabsf(num(a[0]) - num(a[1])) < 1e-5f);
		case GN_LEN:
		case GN_ARR_SIZE:
		case GN_STR_LENGTH:
			if (a[0].t == T_STR) {
				return vint((int)strlen(a[0].s));
			}
			if (is_arr(a[0])) {
				return vint(alen(a[0]));
			}
			return vint(0);
		case GN_VEC2: return vvec(T_VEC2, n > 0 ? num(a[0]) : 0, n > 1 ? num(a[1]) : 0, 0);
		case GN_VEC3: return vvec(T_VEC3, n > 0 ? num(a[0]) : 0, n > 1 ? num(a[1]) : 0, n > 2 ? num(a[2]) : 0);
		case GN_COLOR: return vvec(T_VEC3, n > 0 ? num(a[0]) : 0, n > 1 ? num(a[1]) : 0, n > 2 ? num(a[2]) : 0);
		case GN_STR_UPPER:
		case GN_STR_LOWER: {
			char *o = ring_str();
			unsigned k = 0;
			for (const char *s = a[0].t == T_STR ? a[0].s : ""; *s && k + 1 < GDIR_STR; s++) {
				char c = *s;
				if (id == GN_STR_UPPER && c >= 'a' && c <= 'z') {
					c = (char)(c - 32);
				} else if (id == GN_STR_LOWER && c >= 'A' && c <= 'Z') {
					c = (char)(c + 32);
				}
				o[k++] = c;
			}
			o[k] = 0;
			return vstr(o);
		}
		case GN_STR_BEGINS:
		case GN_STR_ENDS: {
			const char *s = a[0].t == T_STR ? a[0].s : "";
			const char *p = a[1].t == T_STR ? a[1].s : "";
			const size_t ls = strlen(s), lp = strlen(p);
			if (lp > ls) {
				return vbool(0);
			}
			return vbool(id == GN_STR_BEGINS ? strncmp(s, p, lp) == 0 : strcmp(s + ls - lp, p) == 0);
		}
		case GN_STR_SUBSTR: {
			const char *s = a[0].t == T_STR ? a[0].s : "";
			const int ls = (int)strlen(s);
			int from = inum(a[1]);
			int len = n > 2 ? inum(a[2]) : -1;
			if (from < 0) {
				from = 0;
			}
			if (from > ls) {
				from = ls;
			}
			if (len < 0 || from + len > ls) {
				len = ls - from;
			}
			char *o = ring_str();
			if (len > GDIR_STR - 1) {
				len = GDIR_STR - 1;
			}
			memcpy(o, s + from, (size_t)len);
			o[len] = 0;
			return vstr(o);
		}
		case GN_STR_PAD_ZEROS: {
			const char *s = a[0].t == T_STR ? a[0].s : "";
			const int want = inum(a[1]);
			const int ls = (int)strlen(s);
			char *o = ring_str();
			int k = 0;
			for (int i = ls; i < want && k < GDIR_STR - 1; i++) {
				o[k++] = '0';
			}
			for (int i = 0; i < ls && k < GDIR_STR - 1; i++) {
				o[k++] = s[i];
			}
			o[k] = 0;
			return vstr(o);
		}
		case GN_ARR_APPEND:
			if (a[0].t == T_KARR) {
				fail("const arrays are read-only");
				return vnil();
			}
			if (a[0].t == T_ARR && a[0].i >= 0 && s_arr_n[a[0].i] < GDIR_ARR_LEN) {
				arr_set(a[0].i, s_arr_n[a[0].i], a[1]);
				s_arr_n[a[0].i]++;
			}
			return vnil();
		case GN_ARR_INSERT:
			if (a[0].t == T_KARR) {
				fail("const arrays are read-only");
				return vnil();
			}
			if (a[0].t == T_ARR && a[0].i >= 0 && s_arr_n[a[0].i] < GDIR_ARR_LEN) {
				const int ai = a[0].i;
				int at = inum(a[1]);
				if (at < 0) {
					at = 0;
				}
				if (at > s_arr_n[ai]) {
					at = s_arr_n[ai];
				}
				for (int i = s_arr_n[ai]; i > at; i--) {
					arr_set(ai, i, s_arr[ai][i - 1]);
				}
				arr_set(ai, at, a[2]);
				s_arr_n[ai]++;
			}
			return vnil();
		case GN_ARR_POP_BACK:
			if (a[0].t == T_KARR) {
				fail("const arrays are read-only");
				return vnil();
			}
			if (a[0].t == T_ARR && a[0].i >= 0 && s_arr_n[a[0].i] > 0) {
				s_arr_n[a[0].i]--;
				return s_arr[a[0].i][s_arr_n[a[0].i]];
			}
			return vnil();
		case GN_ARR_CLEAR:
			if (a[0].t == T_KARR) {
				fail("const arrays are read-only");
				return vnil();
			}
			if (a[0].t == T_ARR && a[0].i >= 0) {
				s_arr_n[a[0].i] = 0;
			}
			return vnil();
		case GN_ARR_HAS:
		case GN_ARR_FIND:
			if (is_arr(a[0])) {
				const int ln = alen(a[0]);
				for (int i = 0; i < ln; i++) {
					if (vequal(aget(a[0], i), a[1])) {
						return id == GN_ARR_HAS ? vbool(1) : vint(i);
					}
				}
			}
			if (a[0].t == T_STR && a[1].t == T_STR) {
				const char *hit = strstr(a[0].s, a[1].s);
				return id == GN_ARR_HAS ? vbool(hit != 0) : vint(hit ? (int)(hit - a[0].s) : -1);
			}
			return id == GN_ARR_HAS ? vbool(0) : vint(-1);
		case GN_ARR_ERASE:
			if (a[0].t == T_KARR) {
				fail("const arrays are read-only");
				return vnil();
			}
			if (a[0].t == T_ARR && a[0].i >= 0) {
				const int ai = a[0].i;
				for (int i = 0; i < s_arr_n[ai]; i++) {
					if (vequal(s_arr[ai][i], a[1])) {
						for (int j = i; j + 1 < s_arr_n[ai]; j++) {
							arr_set(ai, j, s_arr[ai][j + 1]);
						}
						s_arr_n[ai]--;
						break;
					}
				}
			}
			return vnil();
		case GN_ARR_IS_EMPTY:
			if (a[0].t == T_STR) {
				return vbool(!a[0].s[0]);
			}
			return vbool(alen(a[0]) == 0);
		case GN_ARR_FRONT:
			return aget(a[0], 0);
		case GN_ARR_BACK:
			return aget(a[0], alen(a[0]) - 1);
		case GN_VEC_LENGTH:
			return vfloat(sqrtf(a[0].v[0] * a[0].v[0] + a[0].v[1] * a[0].v[1] + a[0].v[2] * a[0].v[2]));
		case GN_VEC_NORMALIZED: {
			Val r = a[0];
			const float l = sqrtf(r.v[0] * r.v[0] + r.v[1] * r.v[1] + r.v[2] * r.v[2]);
			if (l > 0.0f) {
				r.v[0] /= l;
				r.v[1] /= l;
				r.v[2] /= l;
			}
			return r;
		}
		case GN_VEC_DOT:
			return vfloat(a[0].v[0] * a[1].v[0] + a[0].v[1] * a[1].v[1] + a[0].v[2] * a[1].v[2]);
		case GN_VEC_DISTANCE: {
			const float dx = a[0].v[0] - a[1].v[0], dy = a[0].v[1] - a[1].v[1], dz = a[0].v[2] - a[1].v[2];
			return vfloat(sqrtf(dx * dx + dy * dy + dz * dz));
		}
		case GN_INPUT_PRESSED:
		case GN_INPUT_JUST_PRESSED:
		case GN_INPUT_JUST_RELEASED: {
			const int act = a[0].t == T_STR ? action_id(a[0].s) : -1;
			if (act < 0) {
				return vbool(0);
			}
			if (id == GN_INPUT_PRESSED) {
				return vbool(pad_io_pressed(act));
			}
			if (id == GN_INPUT_JUST_PRESSED) {
				return vbool(pad_io_just_pressed(act));
			}
			return vbool(pad_io_just_released(act));
		}
		case GN_INPUT_STRENGTH:
			return vfloat(a[0].t == T_STR ? action_strength(a[0].s) : 0.0f);
		case GN_INPUT_AXIS:
			return vfloat(action_strength(a[1].s) - action_strength(a[0].s));
		case GN_INPUT_VECTOR: {
			float x = action_strength(a[1].s) - action_strength(a[0].s);
			float y = action_strength(a[3].s) - action_strength(a[2].s);
			const float l = sqrtf(x * x + y * y);
			if (l > 1.0f) {
				x /= l;
				y /= l;
			}
			return vvec(T_VEC2, x, y, 0.0f);
		}
		case GN_HAS_FEATURE:
			/* a guest running the PS2's scenes elsewhere names itself too
			 * (the PS1 build defines GDIR_FEATURE "ps1") */
			return vbool(a[0].t == T_STR && (strcmp(a[0].s, "ps2") == 0 || strcmp(a[0].s, "release") == 0 ||
													strcmp(a[0].s, "template") == 0
#ifdef GDIR_FEATURE
													|| strcmp(a[0].s, GDIR_FEATURE) == 0
#endif
													));
		case GN_CHANGE_SCENE:
		case GN_CHANGE_SCENE_FADE: {
			int pk = -1;
			if (a[0].t == T_STR) {
				/* GDIR knows every cooked pack's path, the main scene (pack 0) too */
				for (int i = 0; i < GDIR_MAX_PACKS && pk < 0; i++) {
					if (s_pack[i].present && s_pack[i].path_k < s_k_n && s_k[s_pack[i].path_k].t == T_STR &&
							strcmp(s_k[s_pack[i].path_k].s, a[0].s) == 0) {
						pk = i;
					}
				}
				if (pk < 0) {
					pk = pack_io_find_path(a[0].s);
				}
			}
			if (pk < 0) {
				printf("GDIR: change_scene: %s is not a cooked pack\n", a[0].t == T_STR ? a[0].s : "?");
				return vint(1);
			}
			s_pending = pk;
			s_pending_fade = (id == GN_CHANGE_SCENE_FADE && n > 1) ? num(a[1]) : 0.0f;
			return vint(0);
		}
		case GN_SET_FADE:
			sys_io_set_fade(num(a[0]), n > 1 ? a[1].v[0] : 0.0f, n > 1 ? a[1].v[1] : 0.0f, n > 1 ? a[1].v[2] : 0.0f);
			return vnil();
		case GN_QUIT:
			return vnil();
		case GN_GET_DELTA_TIME:
			return vfloat(s_delta);
		case GN_TICKS_MSEC:
			return vint((int)s_ticks_ms);
		case GN_NODE_ROTATE_X:
		case GN_NODE_ROTATE_Y:
		case GN_NODE_ROTATE_Z:
			if (a[0].t == T_NODE) {
				rotate_node(a[0].i, id - GN_NODE_ROTATE_X, num(a[1]));
			}
			return vnil();
		case GN_NODE_TRANSLATE:
			if (a[0].t == T_NODE && node_ok(a[0].i)) {
				PackNode *pn = &s_nodes[a[0].i];
				float m[12];
				const float zero[3] = { 0, 0, 0 };
				mat_from_trs(zero, pn->lrot, pn->lscale, m);
				for (int r = 0; r < 3; r++) {
					pn->lpos[r] += m[r * 4 + 0] * a[1].v[0] + m[r * 4 + 1] * a[1].v[1] + m[r * 4 + 2] * a[1].v[2];
				}
				mark_node(a[0].i);
			}
			return vnil();
		case GN_NODE_HIDE:
		case GN_NODE_SHOW:
			if (a[0].t == T_NODE) {
				set_prop(a[0], GP_VISIBLE, vbool(id == GN_NODE_SHOW));
			}
			return vnil();
		case GN_NODE_SET_VISIBLE:
			if (a[0].t == T_NODE) {
				set_prop(a[0], GP_VISIBLE, a[1]);
			}
			return vnil();
		case GN_NODE_IS_VISIBLE:
			if (a[0].t == T_NODE && node_ok(a[0].i)) {
				return vbool(node_visible(a[0].i));
			}
			return vbool(0);
		case GN_NODE_GRAB_FOCUS:
			if (a[0].t == T_NODE && hud_of(a[0].i) >= 0) {
				sys_io_hud_set_focus(hud_of(a[0].i));
			}
			return vnil();
		case GN_NODE_HAS_FOCUS:
			return vbool(a[0].t == T_NODE && hud_of(a[0].i) >= 0 && sys_io_hud_focus() == hud_of(a[0].i));
		case GN_NODE_LOOK_AT:
			if (a[0].t == T_NODE && node_ok(a[0].i)) {
				PackNode *pn = &s_nodes[a[0].i];
				world_fresh(a[0].i);
				const float *w = s_world[a[0].i];
				const float dx = a[1].v[0] - w[3], dy = a[1].v[1] - w[7], dz = a[1].v[2] - w[11];
				const float h = sqrtf(dx * dx + dz * dz);
				pn->lrot[1] = atan2f(-dx, -dz);
				pn->lrot[0] = atan2f(dy, h);
				pn->lrot[2] = 0.0f;
				mark_node(a[0].i);
			}
			return vnil();
		case GN_NODE_CHILD_COUNT:
			return vint(a[0].t == T_NODE && node_ok(a[0].i) ? s_nchild[a[0].i] : 0);
		case GN_NODE_GET_CHILD: {
			int want = inum(a[1]);
			if (a[0].t == T_NODE && node_ok(a[0].i) && want >= 0) {
				for (int c = s_child0[a[0].i]; c >= 0; c = s_sib[c]) {
					if (want-- == 0) {
						return vnode(c);
					}
				}
			}
			return vnil();
		}
		case GN_PS2_PLAY_SFX:
			sfx_io_play_clip(n > 0 && is_num(a[0]) ? inum(a[0]) : 0);
			return vnil();
		case GN_PS2_SET_RUMBLE:
			pad_io_set_rumble(n > 0 ? inum(a[0]) : 1, n > 1 ? inum(a[1]) : 0);
			return vnil();
		case GN_PS2_SET_FOG:
			if (n >= 3) {
				gs_draw_set_fog(1, (int)(a[0].v[0] * 255.0f), (int)(a[0].v[1] * 255.0f), (int)(a[0].v[2] * 255.0f), num(a[1]), num(a[2]));
			} else {
				gs_draw_set_fog(0, 0, 0, 0, 0.0f, 0.0f);
			}
			return vnil();
		case GN_PS2_STOP_RUMBLE:
			pad_io_set_rumble(0, 0);
			return vnil();
		case GN_PS2_SET_HUD_TEXT: {
			const int slot = a[0].t == T_NODE ? hud_of(a[0].i) : inum(a[0]);
			if (slot >= 0) {
				sys_io_set_hud_text(slot, to_text(a[1], t0, sizeof(t0)));
			}
			return vnil();
		}
		case GN_PS2_SHAKE_CAMERA:
			sys_io_shake(n > 0 ? num(a[0]) : 0.2f, n > 1 ? num(a[1]) * 1000.0f : 300.0f);
			return vnil();
		case GN_PS2_GET_DISPLAY_W: return vint(640);
		case GN_PS2_GET_DISPLAY_H: return vint(448);
		case GN_PS2_GET_ABI: return vint(1);
		case GN_PS2_EE_USED: return vint((int)pack_io_ee_used());
		case GN_PS2_EE_FREE: return vint((int)pack_io_ee_free());
		case GN_PS2_EE_LIMIT: return vint((int)pack_io_ee_limit());
		case GN_PS2_GS_USED: return vint((int)pack_io_gs_used());
		case GN_PS2_GS_FREE: return vint((int)pack_io_gs_free());
		case GN_PS2_GS_LIMIT: return vint((int)pack_io_gs_limit());
		case GN_PS2_LOAD_MUSIC:
			sfx_io_music_load(s_cur);
			sfx_io_music_play();
			return vnil();
		case GN_PS2_STOP_MUSIC:
			sfx_io_music_stop();
			return vnil();
		case GN_PS2_SET_MUSIC_VOLUME:
			sfx_io_music_set_vol(num(a[0]));
			return vnil();
		case GN_PS2_SAY:
			sys_io_say(a[0].t == T_STR ? a[0].s : to_text(a[0], t0, sizeof(t0)));
			return vnil();
		case GN_PS2_POKE: {
			const int off = inum(a[0]);
			if (off < 0 || off >= GDIR_POKE_BYTES) {
				return vbool(0);
			}
			s_poke[off] = (unsigned char)inum(a[1]);
			return vbool(1);
		}
		case GN_PS2_PEEK: {
			const int off = inum(a[0]);
			return vint((off >= 0 && off < GDIR_POKE_BYTES) ? s_poke[off] : 0);
		}
		case GN_PS2_SAVE_SLOT: {
			// only as much of the poke bytes as is in use: up to the last
			// non-zero byte, in 16s
			int n = GDIR_POKE_BYTES;
			while (n > 0 && s_poke[n - 1] == 0) {
				n--;
			}
			n = (n + 15) & ~15;
			if (n < 16) {
				n = 16;
			}
			return vbool(mc_io_save(inum(a[0]), s_poke, (unsigned)n));
		}
		case GN_PS2_LOAD_SLOT:
			return vint(mc_io_load(inum(a[0]), s_poke, GDIR_POKE_BYTES));
		case GN_PS2_MEMCARD_PRESENT:
			return vbool(mc_io_present());
		case GN_PS2_SET_VIEWS: {
			/* count, then a camera node for each view; 0 or 1: one view */
			int nv = n > 0 ? inum(a[0]) : 0;
			if (nv < 2 || nv > 4 || n < 1 + nv) {
				nv = 0;
			}
			for (int v = 0; v < nv; v++) {
				s_view_cam[v] = a[1 + v].t == T_NODE ? (short)a[1 + v].i : (short)-1;
			}
			s_view_n = nv;
			gs_draw_set_views(nv);
			s_dirty = 1;
			s_pass_full = 1;
			return vbool(nv >= 2);
		}
		case GN_PS2_PAD_COUNT:
			return vint(pad_io_count());
		case GN_PS2_PAD_PRESSED:
			return vbool(a[1].t == T_STR && pad_io_pad_pressed(inum(a[0]), a[1].s));
		case GN_PS2_PAD_JUST_PRESSED:
			return vbool(a[1].t == T_STR && pad_io_pad_just_pressed(inum(a[0]), a[1].s));
		case GN_PS2_PAD_AXIS:
			return vfloat(pad_io_pad_axis(inum(a[0])));
	}
	fail("unknown native");
	return vnil();
}

/* ------------------------------------------------------------ stack needs */
/* The deepest the operand stack goes in the function at code [start, end),
 * by following its jumps (each place has one depth: the compiler emits
 * structured code). -1 if it can't tell. */
static int operand_depth(unsigned start, unsigned end)
{
	/* scratch: s_stack is idle while the program loads (depth + 1 per byte) */
	unsigned char *seen = (unsigned char *)s_stack;
	if (end <= start || end - start > sizeof(s_stack)) {
		return -1;
	}
	memset(seen, 0, end - start);
	unsigned wpc[48];
	int wd[48];
	int wn = 0;
	wpc[wn] = start;
	wd[wn++] = 0;
	int maxd = 0;
	while (wn > 0) {
		wn--;
		unsigned pc = wpc[wn];
		int d = wd[wn];
		while (pc >= start && pc < end && !seen[pc - start]) {
			if (d > 200) {
				return -1;
			}
			seen[pc - start] = (unsigned char)(d + 1);
			const unsigned op = s_code[pc];
			unsigned len = 1;
			int eff = 0, stop = 0, branch = 0, keep = 0;
			switch (op) {
				case GOP_NOP: break;
				case GOP_CONST: len = 3; eff = 1; break;
				case GOP_NIL: case GOP_TRUE: case GOP_FALSE: case GOP_SELF: case GOP_DUP: eff = 1; break;
				case GOP_LLOAD: case GOP_MLOAD: case GOP_NODEREF: len = 2; eff = 1; break;
				case GOP_LSTORE: case GOP_MSTORE: case GOP_SETP: len = 2; eff = -1; break;
				case GOP_GETP: len = 2; break;
				case GOP_ADD: case GOP_SUB: case GOP_MUL: case GOP_DIV: case GOP_MOD: case GOP_BAND: case GOP_BOR:
				case GOP_EQ: case GOP_NE: case GOP_LT: case GOP_LE: case GOP_GT: case GOP_GE: case GOP_IN:
				case GOP_INDEX: case GOP_POP: eff = -1; break;
				case GOP_NEG: case GOP_NOT: break;
				case GOP_SETINDEX: eff = -3; break;
				case GOP_JMP: len = 5; stop = 1; branch = 1; break;
				case GOP_JZ: case GOP_JNZ: len = 5; eff = -1; branch = 1; break;
				case GOP_JZK: case GOP_JNZK: len = 5; eff = -1; branch = 1; keep = 1; break;
				case GOP_CALL: len = 4; eff = 1 - (int)s_code[pc + 3]; break;
				case GOP_NATIVE: len = 3; eff = 1 - (int)s_code[pc + 2]; break;
				case GOP_ARRAY: len = 2; eff = 1 - (int)s_code[pc + 1]; break;
				case GOP_RET: case GOP_RETNIL: stop = 1; break;
				default: return -1;
			}
			if (pc + len > end) {
				return -1;
			}
			if (branch) {
				const unsigned to = ru32(s_code + pc + 1);
				const int td = keep ? d : d + eff;
				if (op == GOP_JMP) {
					pc = to;
					continue;
				}
				if (wn >= 48) {
					return -1;
				}
				wpc[wn] = to;
				wd[wn++] = td < 0 ? 0 : td;
			}
			if (stop) {
				break;
			}
			d += eff;
			if (d < 0) {
				d = 0;
			}
			if (d > maxd) {
				maxd = d;
			}
			pc += len;
		}
	}
	return maxd;
}

/* each function's need: its locals, its operands and a little room (a native
 * or a call's result is written over its arguments, the arguments stay) */
static void plan_stacks(void)
{
	for (unsigned i = 0; i < s_fn_n; i++) {
		const unsigned start = s_fn[i].code;
		unsigned end = s_code_n;
		for (unsigned j = 0; j < s_fn_n; j++) {
			if (s_fn[j].code > start && s_fn[j].code < end) {
				end = s_fn[j].code;
			}
		}
		const int d = operand_depth(start, end);
		s_fn[i].need = d < 0 ? 0xffff : (unsigned short)(s_fn[i].locals + d + 2);
	}
}

/* ------------------------------------------------------------ interpreter */
/* The loop keeps the stack pointer, the code and the op count in registers
 * and has inline paths only for the common cases (locals, members, consts,
 * int and float arithmetic and compares, bool jumps); everything else is a
 * call out of line, so the loop stays small (the PS1 has a 4 KiB I-cache).
 * The stack top is written back before any of those calls (array GC scans
 * the stack). */
#define VM_NOINLINE __attribute__((noinline))

static const Val k_vtrue = { T_BOOL, { 1 } };

/* a value copy that moves the vector words only for vectors: on the PS1
 * every word is a trip to main RAM */
typedef unsigned __attribute__((may_alias)) vword;
static inline void vcopy(Val *d, const Val *s)
{
	if (sizeof(Val) != 4 * sizeof(vword)) {
		/* a 64-bit CPU (the PS3's PPU): the string pointer makes a Val
		 * 24 bytes, past the four words below */
		*d = *s;
		return;
	}
	const vword *sw = (const vword *)s;
	vword *dw = (vword *)d;
	/* the type byte as a byte: in word 0 it is the low byte only on a
	 * little-endian CPU (the N64 is big-endian) */
	const unsigned t = s->t;
	const unsigned w0 = sw[0], w1 = sw[1];
	dw[0] = w0;
	dw[1] = w1;
	if ((unsigned)(t - T_VEC2) <= (unsigned)(T_VEC3 - T_VEC2)) {
		const unsigned w2 = sw[2], w3 = sw[3];
		dw[2] = w2;
		dw[3] = w3;
	}
}
static const Val k_vfalse = { T_BOOL, { 0 } };

/* the helpers return s_err[0], for gdir_aot.inc's checks */
static VM_NOINLINE int op_arith(int op, Val *a)
{
	*a = arith(op, a[0], a[1]);
	return s_err[0];
}

static VM_NOINLINE int op_compare(int op, Val *a)
{
	*a = vbool(compare(op, a[0], a[1]));
	return s_err[0];
}

static VM_NOINLINE int op_truthy(const Val *v)
{
	return truthy(*v);
}

static VM_NOINLINE int op_getp(Val *o, int p)
{
	*o = get_prop(*o, p);
	return s_err[0];
}

/* [value obj] -> [obj'] */
static VM_NOINLINE int op_setp(Val *v, int p)
{
	*v = set_prop(v[1], p, v[0]);
	return s_err[0];
}

static VM_NOINLINE void op_mstore(unsigned m, const Val *v)
{
	s_mem[m] = persist(*v, s_memstr[m], GDIR_STR);
}

static VM_NOINLINE void op_noderef(Val *o, unsigned inst, unsigned r)
{
	int nd = -1;
	if (r < s_cls[s_inst[inst].cls].noderefs) {
		const unsigned v = ru16(s_inst[inst].refs + r * 2);
		nd = v == 0xffff ? -1 : (int)v;
	}
	*o = vnode(nd);
}

static VM_NOINLINE void op_neg(Val *v)
{
	if (v->t == T_INT) {
		v->i = -v->i;
	} else if (v->t == T_FLOAT) {
		v->f = -v->f;
	} else if (v->t == T_VEC2 || v->t == T_VEC3) {
		v->v[0] = -v->v[0];
		v->v[1] = -v->v[1];
		v->v[2] = -v->v[2];
	}
}

/* [value container] -> [bool] */
static VM_NOINLINE int op_in(Val *a, unsigned inst)
{
	Val pair[2] = { a[1], a[0] };
	*a = native(inst, GN_ARR_HAS, pair, 2);
	return s_err[0];
}

/* natives read their arguments in place (none writes to them); the result
 * goes over the first */
static VM_NOINLINE int op_native(Val *a, unsigned inst, int id, int argc)
{
#if defined(GDIR_STATS) && defined(GDIR_CLOCK)
	const unsigned t0 = GDIR_CLOCK();
	const Val r = native(inst, id, a, argc);
	if (id >= 0 && id < GN_COUNT) {
		s_nat_time[id] += (GDIR_CLOCK() - t0) & 0xffff;
		s_nat_calls[id]++;
	}
	*a = r;
#else
	*a = native(inst, id, a, argc);
#endif
	return s_err[0];
}

/* [elements...] -> [array] (s_sp still above the elements: they stay marked) */
static VM_NOINLINE int op_array(Val *a, int cnt)
{
	const int ai = arr_new();
	Val v = vnil();
	v.t = T_ARR;
	v.i = ai;
	if (ai >= 0) {
		for (int i = 0; i < cnt && i < GDIR_ARR_LEN; i++) {
			arr_set(ai, i, a[i]);
		}
		s_arr_n[ai] = (unsigned char)(cnt < GDIR_ARR_LEN ? cnt : GDIR_ARR_LEN);
	}
	*a = v;
	return s_err[0];
}

/* [base index] -> [value] */
static VM_NOINLINE int op_index(Val *a)
{
	const Val b = a[0];
	Val r = vnil();
	int i = inum(a[1]);
	if (is_arr(b)) {
		const int ln = alen(b);
		if (i < 0) {
			i += ln;
		}
		if (i >= 0 && i < ln) {
			r = aget(b, i);
		} else {
			fail("array index out of range");
		}
	} else if (b.t == T_STR) {
		const int l = (int)strlen(b.s);
		if (i < 0) {
			i += l;
		}
		char *o = ring_str();
		if (i >= 0 && i < l) {
			o[0] = b.s[i];
			o[1] = 0;
		}
		r = vstr(o);
	} else if (b.t == T_VEC2 || b.t == T_VEC3) {
		r = vfloat((i >= 0 && i < 3) ? b.v[i] : 0.0f);
	}
	*a = r;
	return s_err[0];
}

/* [value base index] -> [] */
static VM_NOINLINE int op_setindex(const Val *a)
{
	const Val v = a[0], b = a[1], idx = a[2];
	if (b.t == T_KARR) {
		fail("const arrays are read-only");
	} else if (b.t == T_ARR && b.i >= 0) {
		int i = inum(idx);
		if (i < 0) {
			i += s_arr_n[b.i];
		}
		if (i >= 0 && i < s_arr_n[b.i]) {
			arr_set(b.i, i, v);
		} else {
			fail("array index out of range");
		}
	}
	return s_err[0];
}

/* ------------------------------------------------------------ ahead of time */
/* GDIR_AOT (the PS1 build, when its export had SCRP00.BIN): gdir_aot.inc has
 * the program's hottest functions as C (internal/platforms/ps1/gdiraot.go).
 * Each does what the interpreter does, on the same frame: L the locals, S
 * the operands, each slot at a depth the compiler worked out, so there is no
 * stack pointer to move, no code to fetch and no dispatch, and a constant,
 * local or member is read where it is used rather than pushed. Ints with a
 * constant are inline; the rest are the helpers below (the PS1's floats are
 * calls anyway), so the code stays small. The stack top is written back
 * (A_SYNC) before what can run a script or make an array (a call, a native,
 * an array literal, `in`): nothing else reads it. */
#if defined(GDIR_AOT) && defined(__has_include)
#if __has_include("gdir_aot.inc")
#define GDIR_AOT_ON 1
typedef Val (*AotFn)(unsigned inst, Val *L, Val *stk, unsigned *spv, Val *mb);
struct AotEntry {
	unsigned short fn;
	AotFn f;
};
static AotFn s_aot[GDIR_MAX_FUNCS];
/* compiled for size: straight-line code runs once a call, from RAM more
 * than the I-cache, and -Os measured as fast as -O2 at four fifths the size */
#define AOT_ATTR __attribute__((optimize("Os")))
static unsigned s_aot_n;

/* + - * / % & |, reading the operands where they are (a local, a member, a
 * constant or the frame) and writing the result to d */
static VM_NOINLINE int aot_bin(int op, Val *d, const Val *a, const Val *b)
{
	if (a->t == T_FLOAT && b->t == T_FLOAT && op <= GOP_MUL) {
		/* the common case, read in place */
		const float x = a->f, y = b->f;
		d->f = op == GOP_ADD ? x + y : (op == GOP_SUB ? x - y : x * y);
		d->t = T_FLOAT;
		return 0;
	}
	Val p[2];
	vcopy(&p[0], a);
	vcopy(&p[1], b);
	if (op == GOP_ADD || op == GOP_SUB || op == GOP_MUL) {
		const unsigned ta = p[0].t, tb = p[1].t;
		if (ta == T_INT && tb == T_INT) {
			const int x = p[0].i, y = p[1].i;
			d->i = op == GOP_ADD ? x + y : (op == GOP_SUB ? x - y : x * y);
			d->t = T_INT;
			return 0;
		}
		if ((ta == T_FLOAT || ta == T_INT) && (tb == T_FLOAT || tb == T_INT)) {
			const float x = ta == T_FLOAT ? p[0].f : (float)p[0].i;
			const float y = tb == T_FLOAT ? p[1].f : (float)p[1].i;
			d->f = op == GOP_ADD ? x + y : (op == GOP_SUB ? x - y : x * y);
			d->t = T_FLOAT;
			return 0;
		}
	}
	const int e = op_arith(op, p);
	*d = p[0];
	return e;
}

/* a compare's truth (-1 on an error), its bool to d if there is one */
static VM_NOINLINE int aot_rel(int op, Val *d, const Val *a, const Val *b)
{
	if (a->t == T_FLOAT && b->t == T_FLOAT && op >= GOP_LT) {
		const int c = fcmp(a->f, b->f);
		const int r = op == GOP_LT ? c < 0 : (op == GOP_LE ? c <= 0 : (op == GOP_GT ? c > 0 : c >= 0));
		if (d) {
			d->t = T_BOOL;
			d->i = r;
		}
		return r;
	}
	Val p[2];
	vcopy(&p[0], a);
	vcopy(&p[1], b);
	const unsigned ta = p[0].t, tb = p[1].t;
	int r;
	if (op >= GOP_LT && (ta == T_FLOAT || ta == T_INT) && (tb == T_FLOAT || tb == T_INT)) {
		int c;
		if (ta == T_INT && tb == T_INT) {
			c = (p[0].i > p[1].i) - (p[0].i < p[1].i);
		} else {
			const float x = ta == T_FLOAT ? p[0].f : (float)p[0].i;
			const float y = tb == T_FLOAT ? p[1].f : (float)p[1].i;
			c = fcmp(x, y);
		}
		r = op == GOP_LT ? c < 0 : (op == GOP_LE ? c <= 0 : (op == GOP_GT ? c > 0 : c >= 0));
	} else if (op < GOP_LT && ta == tb && (ta == T_INT || ta == T_BOOL || ta == T_NODE)) {
		r = (p[0].i == p[1].i) == (op == GOP_EQ);
	} else {
		if (op_compare(op, p)) {
			return -1;
		}
		r = p[0].t == T_BOOL && p[0].i;
	}
	if (d) {
		d->t = T_BOOL;
		d->i = r;
	}
	return r;
}

/* [base index] -> d */
static VM_NOINLINE int aot_index(Val *d, const Val *b, const Val *i)
{
	if (b->t == T_ARR && i->t == T_INT && b->i >= 0 && (unsigned)i->i < s_arr_n[b->i]) {
		vcopy(d, &s_arr[b->i][i->i]);
		return 0;
	}
	Val p[2];
	vcopy(&p[0], b);
	vcopy(&p[1], i);
	const int e = op_index(p);
	*d = p[0];
	return e;
}

/* o's property p -> d */
static VM_NOINLINE int aot_getp(Val *d, const Val *o, int p)
{
	vcopy(d, o);
	return op_getp(d, p);
}

/* the commonest natives (as native() has them), from their arguments where
 * they are */
static VM_NOINLINE void aot_float(Val *d, const Val *a)
{
	const float f = a->t == T_STR ? (float)atof(a->s) : num(*a);
	d->f = f;
	d->t = T_FLOAT;
}

static VM_NOINLINE void aot_int(Val *d, const Val *a)
{
	const int i = a->t == T_STR ? atoi(a->s) : inum(*a);
	d->i = i;
	d->t = T_INT;
}

static VM_NOINLINE void aot_abs(Val *d, const Val *a)
{
	if (a->t == T_INT) {
		const int i = a->i < 0 ? -a->i : a->i;
		d->i = i;
		d->t = T_INT;
	} else {
		const float f = fabsf(num(*a));
		d->f = f;
		d->t = T_FLOAT;
	}
}

static VM_NOINLINE void aot_len(Val *d, const Val *a)
{
	const int n = a->t == T_STR ? (int)strlen(a->s) : (is_arr(*a) ? alen(*a) : 0);
	d->i = n;
	d->t = T_INT;
}

static VM_NOINLINE void aot_vec3(Val *d, const Val *a, const Val *b, const Val *c)
{
	const float x = num(*a), y = num(*b), z = num(*c);
	d->v[0] = x;
	d->v[1] = y;
	d->v[2] = z;
	d->t = T_VEC3;
}

#define AOT_PROLOGUE(nloc) \
	Val *const S = L + (nloc); \
	Val ret; \
	ret.t = T_NIL; \
	ret.v[0] = ret.v[1] = ret.v[2] = 0.0f; \
	const unsigned sb = (unsigned)(S - stk); \
	(void)sb; \
	(void)spv; \
	(void)mb
#define AOT_EPILOGUE() \
	done: \
	return ret
#define A_SYNC(n) (*spv = sb + (n))
static const Val k_vnil = { T_NIL, { 0 } };
#define A_ZERO(v) \
	do { \
		vword *w_ = (vword *)&(v); \
		if (sizeof(Val) != 4 * sizeof(vword)) { \
			memset(w_, 0, sizeof(Val)); \
			break; \
		} \
		w_[0] = 0; \
		w_[1] = 0; \
		w_[2] = 0; \
		w_[3] = 0; \
	} while (0)
#define A_CHECK() \
	if (s_err[0]) \
	goto done
#define A_MSTORE(d, m) \
	do { \
		if (S[d].t == T_STR) { \
			op_mstore((unsigned)(mb + (m) - s_mem), &S[d]); \
		} else { \
			vcopy(&mb[m], &S[d]); \
		} \
	} while (0)
#define A_SELF(d) \
	do { \
		S[d].t = T_NODE; \
		S[d].i = s_inst[inst].node; \
	} while (0)
#define A_NODEREF(d, r) op_noderef(&S[d], inst, (r))
#define A_SETP(d, p) \
	do { \
		if (op_setp(&S[d], (p))) \
			goto done; \
	} while (0)
#define A_ARITH(d, op) \
	do { \
		if (op_arith((op), &S[d])) \
			goto done; \
	} while (0)
#define A_NEG(d) op_neg(&S[d])
#define A_IN(d) \
	do { \
		A_SYNC((d) + 1); \
		if (op_in(&S[d], inst)) \
			goto done; \
	} while (0)
#define A_BACK() \
	do { \
		if (++s_ops > GDIR_OPS_PER_FRAME) { \
			fail("script ran too long in one frame (infinite loop?)"); \
			goto done; \
		} \
	} while (0)
#define A_DUP(d) vcopy(&S[d], &S[(d) - 1])
#define A_CALL(d, fn, argc) \
	do { \
		if (++s_ops > GDIR_OPS_PER_FRAME) { \
			fail("script ran too long in one frame (infinite loop?)"); \
			goto done; \
		} \
		A_SYNC((d) + (argc)); \
		const Val r_ = call(inst, s_cls[s_inst[inst].cls].first_func + (fn), &S[d], (argc)); \
		S[d] = r_; \
		A_CHECK(); \
	} while (0)
#define A_NATIVE(d, id, argc) \
	do { \
		A_SYNC((d) + (argc)); \
		if (op_native(&S[d], inst, (id), (argc))) \
			goto done; \
	} while (0)
#define A_ARRAY(d, cnt) \
	do { \
		A_SYNC((d) + (cnt)); \
		if (op_array(&S[d], (cnt))) \
			goto done; \
	} while (0)
#define A_SETINDEX(d) \
	do { \
		if (op_setindex(&S[d])) \
			goto done; \
	} while (0)

#include "gdir_aot.inc"

/* the compiled functions, if the program is the one they were compiled from:
 * the FNV-1a of its constants, classes and code ([p, e), as gdiraot.go's) */
static void aot_bind(const unsigned char *p, const unsigned char *e)
{
	memset(s_aot, 0, sizeof(s_aot));
	s_aot_n = 0;
	unsigned h = 2166136261u;
	for (; p < e; p++) {
		h = (h ^ *p) * 16777619u;
	}
	if (h != GDIR_AOT_HASH || s_fn_n != GDIR_AOT_FUNCS) {
		printf("GDIR: compiled scripts are for another program; interpreting\n");
		return;
	}
	for (unsigned i = 0; i < sizeof(k_aot) / sizeof(k_aot[0]); i++) {
		if (k_aot[i].fn < s_fn_n && s_fn[k_aot[i].fn].need != 0xffff) {
			s_aot[k_aot[i].fn] = k_aot[i].f;
			s_aot_n++;
		}
	}
	printf("GDIR: %u function(s) compiled to C\n", s_aot_n);
}
#endif
#endif

static Val call(unsigned inst, unsigned func, const Val *args, int argc)
{
	if (func >= s_fn_n || s_err[0]) {
		return vnil();
	}
	if (s_depth >= GDIR_DEPTH) {
		fail("call depth exceeded");
		return vnil();
	}
	const Func *f = &s_fn[func];
	Val *stk = s_stack;
	unsigned *spv = &s_sp;
#ifdef GDIR_FAST_STACK_AT
	if (f->need != 0xffff && s_fsp + f->need <= GDIR_FAST_STACK) {
		stk = s_fast;
		spv = &s_fsp;
	} else
#endif
	{
		const unsigned need = f->need != 0xffff ? f->need : f->locals + 64u;
		if (s_sp + need >= GDIR_STACK) {
			fail("stack overflow");
			return vnil();
		}
	}
	const unsigned base = *spv;
	if (f->code >= s_code_n) {
		fail("pc out of range");
		return vnil();
	}
	Val *const lb = stk + base; /* locals */
	for (unsigned i = 0; i < f->locals; i++) {
		lb[i] = (int)i < argc ? args[i] : vnil();
	}
	Val *sp = lb + f->locals;
	s_depth++;
#ifdef GDIR_STATS
	const unsigned ops0 = s_ops;
#ifdef GDIR_CLOCK
	const unsigned child0 = s_child_ticks;
	s_child_ticks = 0;
	const unsigned t0 = GDIR_CLOCK();
#endif
#endif
	Val *const mb = s_mem + s_inst_mem[inst]; /* members */
	const unsigned char *const code = s_code;
	const unsigned code_n = s_code_n;
	unsigned pc = f->code;
	/* s_ops counts each op for the stats; otherwise only jumps back and
	 * calls (what a runaway script repeats), which keeps the loop lean */
	unsigned nops = 0; /* counted since s_ops was last brought up to date */
	Val ret = vnil();
#ifdef GDIR_AOT_ON
	if (s_aot[func]) {
		(void)sp;
		(void)code;
		(void)code_n;
		(void)pc;
		ret = s_aot[func](inst, lb, stk, spv, mb);
		goto done;
	}
#endif
#define VM_SYNC() (*spv = (unsigned)(sp - stk))
#define VM_CHECK() \
	if (s_err[0]) \
	goto done
	for (;;) {
		const unsigned op = code[pc++];
#if defined(GDIR_STATS) || defined(GS_FRAMELOG)
		nops++;
#define VM_COUNT_BACK()
#else
#define VM_COUNT_BACK() nops++
#endif
		switch (op) {
			case GOP_NOP:
				break;
			case GOP_CONST: {
				const unsigned k = ru16(code + pc);
				pc += 2;
				if (k < s_k_n) {
					vcopy(sp++, &s_k[k]);
				} else {
					*sp++ = vnil();
				}
			} break;
			case GOP_NIL:
				*sp++ = vnil();
				break;
			case GOP_TRUE:
				*sp++ = k_vtrue;
				break;
			case GOP_FALSE:
				*sp++ = k_vfalse;
				break;
			case GOP_LLOAD:
				vcopy(sp++, &lb[code[pc++]]);
				break;
			case GOP_LSTORE:
				--sp;
				vcopy(&lb[code[pc++]], sp);
				break;
			case GOP_MLOAD:
				vcopy(sp++, &mb[code[pc++]]);
				break;
			case GOP_MSTORE: {
				Val *m = mb + code[pc++];
				--sp;
				if (sp->t == T_STR) {
					VM_SYNC();
					op_mstore((unsigned)(m - s_mem), sp);
				} else {
					vcopy(m, sp);
				}
			} break;
			case GOP_SELF:
				*sp++ = vnode(s_inst[inst].node);
				break;
			case GOP_NODEREF:
				op_noderef(sp++, inst, code[pc++]);
				break;
			case GOP_GETP: {
				const int p = code[pc++];
				Val *o = sp - 1;
				if (o->t == T_VEC3 || o->t == T_VEC2) {
					/* .x .y .z of a vector */
					const int k = (p == GP_X || p == GP_R) ? 0 : ((p == GP_Y || p == GP_G) ? 1 : ((p == GP_Z || p == GP_B) ? 2 : 3));
					if (k < 2 || (k == 2 && o->t == T_VEC3)) {
						o->f = o->v[k];
						o->t = T_FLOAT;
						break;
					}
				}
				VM_SYNC();
				op_getp(o, p);
				VM_CHECK();
			} break;
			case GOP_SETP:
				sp -= 2;
				VM_SYNC();
				op_setp(sp++, code[pc++]);
				VM_CHECK();
				break;
			case GOP_ADD:
			case GOP_SUB:
			case GOP_MUL: {
				Val *a = sp - 2;
				const unsigned ta = a[0].t, tb = a[1].t;
				if (ta == T_INT && tb == T_INT) {
					const int x = a[0].i, y = a[1].i;
					a->i = op == GOP_ADD ? x + y : (op == GOP_SUB ? x - y : x * y);
					sp--;
					break;
				}
				if ((ta == T_FLOAT || ta == T_INT) && (tb == T_FLOAT || tb == T_INT)) {
					const float x = ta == T_FLOAT ? a[0].f : (float)a[0].i;
					const float y = tb == T_FLOAT ? a[1].f : (float)a[1].i;
					a->f = op == GOP_ADD ? x + y : (op == GOP_SUB ? x - y : x * y);
					a->t = T_FLOAT;
					sp--;
					break;
				}
				sp--;
				VM_SYNC();
				op_arith((int)op, a);
				VM_CHECK();
			} break;
			case GOP_DIV:
			case GOP_MOD:
			case GOP_BAND:
			case GOP_BOR:
				sp--;
				VM_SYNC();
				op_arith((int)op, sp - 1);
				VM_CHECK();
				break;
			case GOP_NEG:
				op_neg(sp - 1);
				break;
			case GOP_LT:
			case GOP_LE:
			case GOP_GT:
			case GOP_GE: {
				Val *a = sp - 2;
				const unsigned ta = a[0].t, tb = a[1].t;
				int c;
				if (ta == T_INT && tb == T_INT) {
					c = (a[0].i > a[1].i) - (a[0].i < a[1].i);
				} else if ((ta == T_FLOAT || ta == T_INT) && (tb == T_FLOAT || tb == T_INT)) {
					const float x = ta == T_FLOAT ? a[0].f : (float)a[0].i;
					const float y = tb == T_FLOAT ? a[1].f : (float)a[1].i;
					c = fcmp(x, y);
				} else {
					sp--;
					VM_SYNC();
					op_compare((int)op, a);
					VM_CHECK();
					break;
				}
				a->t = T_BOOL;
				a->i = op == GOP_LT ? c < 0 : (op == GOP_LE ? c <= 0 : (op == GOP_GT ? c > 0 : c >= 0));
				sp--;
			} break;
			case GOP_EQ:
			case GOP_NE: {
				Val *a = sp - 2;
				const unsigned ta = a[0].t;
				if (ta == a[1].t && (ta == T_INT || ta == T_BOOL || ta == T_NODE)) {
					const int e = a[0].i == a[1].i;
					a->t = T_BOOL;
					a->i = op == GOP_EQ ? e : !e;
					sp--;
					break;
				}
				sp--;
				VM_SYNC();
				op_compare((int)op, a);
				VM_CHECK();
			} break;
			case GOP_IN:
				sp--;
				VM_SYNC();
				op_in(sp - 1, inst);
				VM_CHECK();
				break;
			case GOP_NOT: {
				Val *v = sp - 1;
				const int t = v->t == T_BOOL ? v->i != 0 : op_truthy(v);
				v->t = T_BOOL;
				v->i = !t;
			} break;
			case GOP_JMP:
			case GOP_JZ:
			case GOP_JNZ:
			case GOP_JZK:
			case GOP_JNZK: {
				const unsigned to = ru32(code + pc);
				pc += 4;
				if (op != GOP_JMP) {
					Val *v = sp - 1;
					const int t = v->t == T_BOOL ? v->i != 0 : op_truthy(v);
					/* JZ/JZK jump when falsy, JNZ/JNZK when truthy; the K
					 * forms keep the value when they jump */
					const int jump = (op == GOP_JZ || op == GOP_JZK) ? !t : t;
					if (!jump || op == GOP_JZ || op == GOP_JNZ) {
						sp--;
					}
					if (!jump) {
						break;
					}
				}
				if (to >= code_n) {
					fail("pc out of range");
					goto done;
				}
				if (to < pc) {
					/* a loop: the per-frame budget catches a runaway one */
					VM_COUNT_BACK();
					if (s_ops + nops > GDIR_OPS_PER_FRAME) {
						fail("script ran too long in one frame (infinite loop?)");
						goto done;
					}
				}
				pc = to;
			} break;
			case GOP_POP:
				sp--;
				break;
			case GOP_DUP:
				vcopy(sp, sp - 1);
				sp++;
				break;
			case GOP_CALL: {
				const unsigned fn = ru16(code + pc);
				const int argc2 = code[pc + 2];
				pc += 3;
				VM_COUNT_BACK();
				s_ops += nops;
				nops = 0;
				if (s_ops > GDIR_OPS_PER_FRAME) {
					fail("script ran too long in one frame (infinite loop?)");
					goto done;
				}
				/* the arguments stay on the stack (marked) under the callee's frame */
				VM_SYNC();
				const Val r = call(inst, s_cls[s_inst[inst].cls].first_func + fn, sp - argc2, argc2);
				sp -= argc2;
				*sp++ = r;
				VM_CHECK();
			} break;
			case GOP_NATIVE: {
				const int id = code[pc];
				const int argc2 = code[pc + 1];
				pc += 2;
				s_ops += nops;
				nops = 0;
				VM_SYNC();
				sp -= argc2;
				op_native(sp, inst, id, argc2);
				sp++;
				VM_CHECK();
			} break;
			case GOP_RET:
				ret = *--sp;
				goto done;
			case GOP_RETNIL:
				goto done;
			case GOP_ARRAY: {
				const int cnt = code[pc++];
				VM_SYNC();
				sp -= cnt;
				op_array(sp, cnt);
				sp++;
				VM_CHECK();
			} break;
			case GOP_INDEX: {
				Val *a = sp - 2;
				sp--;
				if (a[0].t == T_ARR && a[1].t == T_INT && a[0].i >= 0) {
					const unsigned i = (unsigned)a[1].i;
					if (i < s_arr_n[a[0].i]) {
						vcopy(a, &s_arr[a[0].i][i]);
						break;
					}
				}
				VM_SYNC();
				op_index(a);
				VM_CHECK();
			} break;
			case GOP_SETINDEX:
				sp -= 3;
				VM_SYNC();
				op_setindex(sp);
				VM_CHECK();
				break;
			default:
				fail("bad opcode");
				goto done;
		}
	}
done:
#undef VM_SYNC
#undef VM_CHECK
#undef VM_COUNT_BACK
	s_ops += nops;
#ifdef GDIR_STATS
	s_fn_ops[func] += s_ops - ops0;
#ifdef GDIR_CLOCK
	{
		const unsigned dt = (GDIR_CLOCK() - t0) & 0xffff;
		s_fn_self[func] += dt > s_child_ticks ? dt - s_child_ticks : 0;
		s_child_ticks = child0 + dt;
	}
#endif
#endif
	*spv = base;
	s_depth--;
	return ret;
}

/* ------------------------------------------------------------ packs */
static void enter_pack(int pack)
{
	s_entered = pack;
	s_cur = -1;
	s_node_n = 0;
	s_inst_n = 0;
	s_mem_n = 0;
	s_conn_n = 0;
	memset(s_arr_used, 0, sizeof(s_arr_used));
	gs_draw_nodes_reset();
	s_view_n = 0;
	gs_draw_set_views(0);
	if (pack < 0 || pack >= GDIR_MAX_PACKS || !s_pack[pack].present) {
		gs_draw_camera_lock(0);
		return;
	}
	s_cur = pack;
	const Pack *pk = &s_pack[pack];
	s_node_n = pk->node_n < GDIR_MAX_NODES ? pk->node_n : GDIR_MAX_NODES;
	const unsigned char *p = pk->nodes;
	/* node records from PNODnn.BIN (a guest short of RAM keeps them on the
	 * disc), a few at a time through a small buffer */
	FILE *nf = 0;
	unsigned char nbuf[88 * 8];
	unsigned nbuf_n = 0, nbuf_i = 0;
	if (s_gflags & GDIR_FLAG_PNOD) {
		char host[32], iso[48];
		snprintf(host, sizeof(host), "host:PNOD%02d.bin", pack);
		snprintf(iso, sizeof(iso), "cdrom0:\\PNOD%02d.BIN;1", pack);
		nf = iso_fopen(iso, "rb");
		if (!nf) {
			nf = fopen(host, "rb");
		}
		if (!nf) {
			fail("pack node records (PNOD) missing");
			s_node_n = 0;
		}
	}
	for (unsigned n = 0; n < s_node_n; n++) {
		PackNode *pn = &s_nodes[n];
		if (nf) {
			if (nbuf_i >= nbuf_n) {
				const unsigned want = s_node_n - n < 8 ? s_node_n - n : 8;
				if (fread(nbuf, 88, want, nf) != want) {
					fail("pack node records (PNOD) cut short");
					s_node_n = n;
					break;
				}
				nbuf_n = want;
				nbuf_i = 0;
			}
			p = nbuf + 88 * nbuf_i++;
		}
		pn->parent = (short)ru16(p);
		pn->flags = p[2];
		pn->hud = (signed char)p[3];
		for (int k = 0; k < 3; k++) {
			pn->lpos[k] = rf32(p + 4 + k * 4);
			pn->lrot[k] = rf32(p + 16 + k * 4);
			pn->lscale[k] = rf32(p + 28 + k * 4);
		}
		for (int k = 0; k < 12; k++) {
			pn->w0[k] = rf32(p + 40 + k * 4);
		}
		p += 88;
		if (pn->flags & 2) {
			mat_inv34(pn->w0, s_w0inv[n]);
		}
	}
	if (nf) {
		fclose(nf);
	}
	/* child lists for get_child / get_child_count, in node order */
	for (unsigned n = 0; n < s_node_n; n++) {
		const int pa = s_nodes[n].parent;
		s_par[n] = (short)((pa >= 0 && pa < (int)n) ? pa : -1);
		s_child0[n] = -1;
		s_sib[n] = -1;
		s_nchild[n] = 0;
	}
	for (int n = (int)s_node_n - 1; n >= 0; n--) {
		const int pa = s_par[n];
		if (pa >= 0) {
			s_sib[n] = s_child0[pa];
			s_child0[pa] = (short)n;
			s_nchild[pa]++;
		}
	}
	/* subtree sizes (children have higher indices), then whether each
	 * subtree is the run of indices after its node */
	for (unsigned n = 0; n < s_node_n; n++) {
		s_subend[n] = 1;
	}
	for (int n = (int)s_node_n - 1; n >= 0; n--) {
		if (s_par[n] >= 0) {
			s_subend[s_par[n]] = (unsigned short)(s_subend[s_par[n]] + s_subend[n]);
		}
	}
	for (unsigned n = 0; n < s_node_n; n++) {
		s_subend[n] = (unsigned short)(s_subend[n] + n);
	}
	s_preorder = 1;
	for (unsigned n = 0; n < s_node_n; n++) {
		const int pa = s_par[n];
		if (pa >= 0 && s_subend[n] > s_subend[pa]) {
			s_preorder = 0;
		}
	}
	s_mark_n = 0;
	gs_draw_nodes_begin();
	if (pk->has_clear) {
		gs_draw_set_clear(pk->clear[0], pk->clear[1], pk->clear[2]);
	}
	gs_draw_camera_lock(node_ok(pk->camera) ? 1 : 0);
	/* instances + members */
	const unsigned char *q = pk->insts;
	for (unsigned i = 0; i < pk->inst_n && s_inst_n < GDIR_MAX_INST; i++) {
		PackInst *in = &s_inst[s_inst_n];
		in->node = (unsigned short)ru16(q);
		in->cls = (unsigned short)ru16(q + 2);
		in->refs = q + 4;
		const unsigned nrefs = in->cls < s_cls_n ? s_cls[in->cls].noderefs : 0;
		q += 4 + nrefs * 2;
		in->ov_n = q[0];
		in->ov = q + 1;
		q += 1 + in->ov_n * 3u;
		if (in->cls >= s_cls_n) {
			continue;
		}
		s_inst_mem[s_inst_n] = s_mem_n;
		for (unsigned m = 0; m < s_cls[in->cls].members && s_mem_n < GDIR_MAX_MEMBERS; m++) {
			s_mem[s_mem_n++] = vnil();
		}
		s_inst_n++;
	}
	s_mem_n_gc = s_mem_n;
	const unsigned char *c = pk->conns;
	for (unsigned i = 0; i < pk->conn_n && s_conn_n < 64; i++) {
		s_conn[s_conn_n].node = (unsigned short)ru16(c);
		s_conn[s_conn_n].signal = c[2];
		s_conn[s_conn_n].inst = (unsigned short)ru16(c + 3);
		s_conn[s_conn_n].func = (unsigned short)ru16(c + 5);
		s_conn_n++;
		c += 7;
	}
	s_dirty = 1;
	s_all_dirty = 1;
	s_pass_full = 1;
	recompute_nodes();
	/* member initialisers in tree order, then _ready children first */
	for (unsigned i = 0; i < s_inst_n; i++) {
		const Class *k = &s_cls[s_inst[i].cls];
		if (k->fn_init >= 0) {
			call(i, k->first_func + (unsigned)k->fn_init, 0, 0);
		}
		for (unsigned o = 0; o < s_inst[i].ov_n; o++) {
			const unsigned m = s_inst[i].ov[o * 3];
			const unsigned kk = ru16(s_inst[i].ov + o * 3 + 1);
			if (m < k->members && kk < s_k_n) {
				s_mem[s_inst_mem[i] + m] = s_k[kk];
			}
		}
	}
	for (int i = (int)s_inst_n - 1; i >= 0; i--) {
		const Class *k = &s_cls[s_inst[i].cls];
		if (k->fn_ready >= 0) {
			call((unsigned)i, k->first_func + (unsigned)k->fn_ready, 0, 0);
		}
	}
	recompute_nodes();
}

/* a program past one of this build's tables (the PS1's are smaller) */
static void init_limit(const char *what, unsigned n, int max)
{
	char m[96];
	snprintf(m, sizeof(m), "GDIR program has %u %s; this runtime holds %d", n, what, max);
	fail(m);
}

int gdir_vm_init(const unsigned char *b, unsigned sz)
{
	s_mem_gc = s_mem;
	s_stack_gc = s_stack;
	s_sp_live = &s_sp;
	s_active = 0;
	s_err[0] = 0;
	if (!b || sz < 12 || b[0] != 'G' || b[1] != 'D' || b[2] != 'I' || b[3] != 'R' || ru16(b + 4) != GDIR_VERSION) {
		fail("not a GDIR program (header or version)");
		return 0;
	}
	const unsigned total = ru32(b + 8);
	if (total > sz) {
		fail("GDIR program is cut short");
		return 0;
	}
	s_gflags = ru16(b + 6);
	const unsigned char *p = b + 12;
	const unsigned char *end = b + total;
	s_k_n = ru32(p);
	p += 4;
	s_karr_n = 0;
	if (s_k_n > GDIR_MAX_CONSTS) {
		init_limit("constants", s_k_n, GDIR_MAX_CONSTS);
		return 0;
	}
	for (unsigned i = 0; i < s_k_n && p < end; i++) {
		const int t = *p++;
		Val v = vnil();
		switch (t) {
			case GDIR_K_NIL:
				break;
			case GDIR_K_BOOL:
				v = vbool(*p++);
				break;
			case GDIR_K_INT:
				v = vint((int)ru32(p));
				p += 4;
				break;
			case GDIR_K_FLOAT:
				v = vfloat(rf32(p));
				p += 4;
				break;
			case GDIR_K_STR: {
				const unsigned n = ru16(p);
				v = vstr((const char *)(p + 2));
				p += 2 + n + 1;
			} break;
			case GDIR_K_VEC2:
				v = vvec(T_VEC2, rf32(p), rf32(p + 4), 0.0f);
				p += 8;
				break;
			case GDIR_K_VEC3:
				v = vvec(T_VEC3, rf32(p), rf32(p + 4), rf32(p + 8));
				p += 12;
				break;
			case GDIR_K_ARRAY: {
				const unsigned n = ru16(p);
				if (s_karr_n >= GDIR_MAX_KARR) {
					init_limit("constant arrays", s_karr_n + 1, GDIR_MAX_KARR);
					return 0;
				}
				s_karr[s_karr_n] = p;
				v.t = T_KARR;
				v.i = (int)s_karr_n++;
				p += 2 + n * 2;
			} break;
			default:
				fail("GDIR program has an unknown constant type");
				return 0;
		}
		s_k[i] = v;
	}
	s_cls_n = ru16(p);
	p += 2;
	s_fn_n = 0;
	if (s_cls_n > GDIR_MAX_CLASSES) {
		init_limit("classes", s_cls_n, GDIR_MAX_CLASSES);
		return 0;
	}
	for (unsigned c = 0; c < s_cls_n; c++) {
		Class *k = &s_cls[c];
		k->members = (unsigned short)ru16(p);
		k->noderefs = (unsigned short)ru16(p + 2);
		k->funcs = (unsigned short)ru16(p + 4);
		k->fn_init = (short)ru16(p + 6);
		k->fn_ready = (short)ru16(p + 8);
		k->fn_process = (short)ru16(p + 10);
		k->fn_physics = (short)ru16(p + 12);
		p += 14;
		k->first_func = (unsigned short)s_fn_n;
		for (unsigned f = 0; f < k->funcs; f++) {
			if (s_fn_n >= GDIR_MAX_FUNCS) {
				init_limit("functions", s_fn_n + 1, GDIR_MAX_FUNCS);
				return 0;
			}
			s_fn[s_fn_n].argc = p[0];
			s_fn[s_fn_n].locals = p[1];
			s_fn[s_fn_n].code = ru32(p + 2);
			s_fn[s_fn_n].name = (unsigned short)ru16(p + 6);
			s_fn_n++;
			p += 8; // argc, locals, code, name const u16
		}
	}
	s_code_n = ru32(p);
	p += 4;
	s_code = p;
	p += s_code_n;
	plan_stacks();
#ifdef GDIR_AOT_ON
	aot_bind(b + 12, s_code + s_code_n);
#endif
	const unsigned npk = ru16(p);
	p += 2;
	memset(s_pack, 0, sizeof(s_pack));
	for (unsigned i = 0; i < npk && p < end; i++) {
		const int idx = p[0];
		Pack tmp;
		memset(&tmp, 0, sizeof(tmp));
		tmp.present = 1;
		tmp.clear[0] = p[1];
		tmp.clear[1] = p[2];
		tmp.clear[2] = p[3];
		tmp.has_clear = p[4];
		tmp.camera = (short)ru16(p + 5);
		tmp.fov = rf32(p + 7);
		tmp.node_n = (unsigned short)ru16(p + 11);
		tmp.path_k = (unsigned short)ru16(p + 13);
		p += 15;
		tmp.nodes = p;
		if (!(s_gflags & GDIR_FLAG_PNOD)) {
			p += tmp.node_n * 88u;
		}
		tmp.inst_n = (unsigned short)ru16(p);
		p += 2;
		tmp.insts = p;
		/* skip instance records: need class noderef counts */
		for (unsigned k = 0; k < tmp.inst_n; k++) {
			const unsigned cls = ru16(p + 2);
			const unsigned nr = cls < s_cls_n ? s_cls[cls].noderefs : 0;
			p += 4 + nr * 2;
			p += 1 + p[0] * 3u;
		}
		tmp.conn_n = (unsigned short)ru16(p);
		p += 2;
		tmp.conns = p;
		p += tmp.conn_n * 7u;
		if (idx >= 0 && idx < GDIR_MAX_PACKS) {
			s_pack[idx] = tmp;
		}
	}
	s_active = 1;
	s_cur = -1;
	s_pending = -1;
	enter_pack(pack_io_current());
	printf("GDIR: %u classes, %u functions, %u bytes of code, %u packs\n", s_cls_n, s_fn_n, s_code_n, npk);
	return 1;
}

int gdir_vm_active(void)
{
	return s_active;
}

const char *gdir_vm_error(void)
{
	return s_err;
}

void gdir_vm_process(float delta)
{
	if (!s_active) {
		return;
	}
#ifdef GDIR_FREEZE_AT
	/* dev: stop the scripts GDIR_FREEZE_AT frames into pack
	 * GDIR_FREEZE_PACK, so a screenshot shows the same frame run to run */
	{
		static int fpack = -9;
		static unsigned fn;
		if (s_cur != fpack) {
			fpack = s_cur;
			fn = 0;
		}
		if (s_cur == GDIR_FREEZE_PACK && ++fn > GDIR_FREEZE_AT) {
			return;
		}
	}
#endif
	s_delta = delta;
	s_ticks_ms += (unsigned)(delta * 1000.0f);
	s_ops = 0;
	s_mem_n_gc = s_mem_n;
	s_sp_gc = 0;
	s_fsp_gc = 0;
	arr_gc();
	if (pack_io_current() != s_entered) {
		enter_pack(pack_io_current());
	}
	sys_io_tick(delta);
	/* Button focus + Cross: pressed connections first, then the HUD action word. */
	if (sys_io_hud_just_accept()) {
		const int slot = sys_io_hud_focus();
		int handled = 0;
		for (unsigned i = 0; i < s_conn_n && slot >= 0; i++) {
			if (s_conn[i].signal == 0 && hud_of(s_conn[i].node) == slot && s_conn[i].inst < s_inst_n) {
				call(s_conn[i].inst, s_cls[s_inst[s_conn[i].inst].cls].first_func + s_conn[i].func, 0, 0);
				handled = 1;
			}
		}
		(void)handled;
	}
	if (s_cur >= 0) {
		s_mem_n_gc = s_mem_n;
	}
	if (s_cur >= 0 && !sys_io_paused() && !s_err[0]) {
		for (unsigned i = 0; i < s_inst_n && s_pending < 0; i++) {
			const Class *k = &s_cls[s_inst[i].cls];
#if defined(GDIR_STATS) && defined(GDIR_CLOCK)
			const unsigned ts0 = GDIR_CLOCK();
#endif
			if (k->fn_process >= 0) {
				const Val d = vfloat(delta);
				call(i, k->first_func + (unsigned)k->fn_process, &d, 1);
			}
			if (k->fn_physics >= 0) {
				const Val d = vfloat(delta);
				call(i, k->first_func + (unsigned)k->fn_physics, &d, 1);
			}
#if defined(GDIR_STATS) && defined(GDIR_CLOCK)
			s_script_ticks += (GDIR_CLOCK() - ts0) & 0xffff;
#endif
		}
	}
#if defined(GDIR_STATS) && defined(GDIR_CLOCK)
	const unsigned tr0 = GDIR_CLOCK();
#endif
	if (s_dirty) {
		recompute_nodes();
	}
#if defined(GDIR_STATS) && defined(GDIR_CLOCK)
	s_recompute_ticks += (GDIR_CLOCK() - tr0) & 0xffff;
#endif
	s_ops_last = s_ops;
#ifdef GDIR_TRACE
	/* a hash of the members every 30 frames of a pack, to compare two
	 * builds (the interpreter and the compiled scripts) run for run: give
	 * both a fixed delta */
	{
		static int pack = -9;
		static unsigned n;
		if (s_cur != pack) {
			pack = s_cur;
			n = 0;
		}
		if (++n % 30 == 0) {
			unsigned h = 2166136261u;
			for (unsigned i = 0; i < s_mem_n; i++) {
				const Val &v = s_mem[i];
				unsigned w[4] = { v.t, 0, 0, 0 };
				if (v.t == T_STR) {
					for (const char *c = v.s; c && *c; c++) {
						h = (h ^ (unsigned char)*c) * 16777619u;
					}
				} else if (v.t == T_ARR && v.i >= 0 && v.i < GDIR_ARRAYS) {
					for (unsigned e = 0; e < s_arr_n[v.i]; e++) {
						unsigned x;
						memcpy(&x, &s_arr[v.i][e].i, 4);
						h = (h ^ s_arr[v.i][e].t) * 16777619u;
						h = (h ^ (s_arr[v.i][e].t == T_STR ? 0 : x)) * 16777619u;
					}
				} else if (v.t != T_NIL) {
					memcpy(&w[1], &v.i, 4);
					if (v.t == T_VEC2 || v.t == T_VEC3) {
						memcpy(&w[2], &v.v[1], 8);
					}
				}
				for (int k = 0; k < 4; k++) {
					h = (h ^ w[k]) * 16777619u;
				}
			}
			printf("GDIR trace %d/%u: %08x\n", s_cur, n, h);
		}
	}
#endif
#ifdef GDIR_STATS
	{
		static unsigned peak, frames;
		static int spack = -9;
		if (s_cur != spack) {
			/* each pack's reports from its first frame (a run's reports
			 * then line up with another's) */
			spack = s_cur;
			frames = 0;
			peak = 0;
			memset(s_fn_ops, 0, sizeof(s_fn_ops));
#ifdef GDIR_CLOCK
			memset(s_fn_self, 0, sizeof(s_fn_self));
			memset(s_nat_time, 0, sizeof(s_nat_time));
			memset(s_nat_calls, 0, sizeof(s_nat_calls));
			s_st_computed = s_st_sent = 0;
			s_recompute_ticks = 0;
			s_script_ticks = 0;
#endif
		}
		if (s_ops > peak) {
			peak = s_ops;
		}
		if (++frames % 300 == 0) {
			printf("GDIR stats: pack %d peak %u ops/frame (last %u)\n", s_cur, peak, s_ops);
			peak = 0;
			/* the six heaviest functions over the last 300 frames, per frame */
			for (int top = 0; top < 6; top++) {
				unsigned best = 0, bi = 0;
				for (unsigned f = 0; f < s_fn_n; f++) {
					if (s_fn_ops[f] > best) {
						best = s_fn_ops[f];
						bi = f;
					}
				}
				if (!best) {
					break;
				}
				const Val nm = s_fn[bi].name < s_k_n ? s_k[s_fn[bi].name] : vnil();
				printf("  %-20s %u ops/frame\n", nm.t == T_STR ? nm.s : "?", best / 300);
				s_fn_ops[bi] = 0;
			}
			memset(s_fn_ops, 0, sizeof(s_fn_ops));
#ifdef GDIR_CLOCK
			/* where the time goes: the 16 heaviest by their own ticks */
			for (int top = 0; top < 16; top++) {
				unsigned best = 0, bi = 0;
				for (unsigned f = 0; f < s_fn_n; f++) {
					if (s_fn_self[f] > best) {
						best = s_fn_self[f];
						bi = f;
					}
				}
				if (!best) {
					break;
				}
				const Val nm = s_fn[bi].name < s_k_n ? s_k[s_fn[bi].name] : vnil();
				printf("  self %3u %-16s %u ticks/frame\n", bi, nm.t == T_STR ? nm.s : "?", best / 300);
				s_fn_self[bi] = 0;
			}
			memset(s_fn_self, 0, sizeof(s_fn_self));
			printf("  node pass          %u ticks/frame (%u worlds computed, %u sent per frame)\n", s_recompute_ticks / 300,
					s_st_computed / 300, s_st_sent / 300);
			printf("  scripts            %u ticks/frame\n", s_script_ticks / 300);
			s_st_computed = s_st_sent = 0;
			s_recompute_ticks = 0;
			s_script_ticks = 0;
			/* the six natives that took the most time, ticks per frame */
			for (int top = 0; top < 6; top++) {
				unsigned best = 0, bi = 0;
				for (unsigned k = 0; k < GN_COUNT; k++) {
					if (s_nat_time[k] > best) {
						best = s_nat_time[k];
						bi = k;
					}
				}
				if (!best) {
					break;
				}
				printf("  native %3u        %u ticks/frame, %u calls/frame\n", bi, best / 300, s_nat_calls[bi] / 300);
				s_nat_time[bi] = 0;
			}
			memset(s_nat_time, 0, sizeof(s_nat_time));
			memset(s_nat_calls, 0, sizeof(s_nat_calls));
#endif
		}
	}
#endif
	if (s_pending >= 0) {
		const int pk = s_pending;
		s_pending = -1;
		if (s_pending_fade > 0.0f) {
			sys_io_set_fade(1.0f, 0.0f, 0.0f, 0.0f);
			sys_io_scene_fade(s_pending_fade);
		}
		if (pack_io_swap(pk)) {
			enter_pack(pack_io_current());
		}
	}
	sfx_io_tick(delta);
}

unsigned gdir_vm_last_ops(void)
{
	return s_ops_last;
}
