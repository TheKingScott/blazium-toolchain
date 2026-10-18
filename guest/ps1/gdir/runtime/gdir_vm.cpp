// MIT. GDIR VM for the Blazium PS2 guest: runs the cooked GDScript subset.
// See gdir_ops.h for the bytecode and blazium/platform/ps2/export/cook/
// gdir_compile.cpp for the compiler. Not the Godot VM: no Variant/ClassDB.

#include "gdir_vm.h"
#include "mc_io.h"

#include "gdir_ops.h"
#include "gs_draw.h"
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
static float s_local[GDIR_MAX_NODES][12];
static unsigned char s_all_dirty = 1;

static void mark_node(int n)
{
	s_dirty = 1;
	if (n >= 0 && n < GDIR_MAX_NODES) {
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

static void fail(const char *msg)
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
	memset(&v, 0, sizeof(v));
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
	for (int i = 0; i < GDIR_ARRAYS; i++) {
		s_arr_used[i] = mark[i];
	}
}

static unsigned *s_sp_live;
static int arr_new(void)
{
	if (s_sp_live) {
		s_sp_gc = *s_sp_live;
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
		for (int c = 0; c < 4; c++) {
			t[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] + a[r * 4 + 2] * b[2 * 4 + c] + (c == 3 ? a[r * 4 + 3] : 0.0f);
		}
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
	const int p = s_nodes[n].parent;
	return (p >= 0 && p < n) ? p : -1;
}

static int node_stale(int n)
{
	const int p = node_parent(n);
	return s_ndirty[n] || (p >= 0 && s_pver[n] != s_wver[p]);
}

/* n's world from its parent's (already current) and its own transform */
static void compute_one(int n)
{
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
		}
		if (p >= 0) {
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

static void recompute_nodes(void)
{
	/* Parents come before children, so one pass carries a change down: a
	 * node's world is redone when it or an ancestor moved. */
	const int all = s_all_dirty;
	memset(s_keep, 0, s_node_n);
	if (s_cur >= 0 && s_cur < GDIR_MAX_PACKS) {
		mark_keep(s_pack[s_cur].camera);
	}
	for (int v = 0; v < s_view_n; v++) {
		mark_keep(s_view_cam[v]);
	}
	for (unsigned n = 0; n < s_node_n; n++) {
		PackNode *pn = &s_nodes[n];
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
	s_all_dirty = 0;
	for (unsigned n = 0; n < s_node_n; n++) {
		const PackNode *pn = &s_nodes[n];
		gs_draw_node_hidden((int)n, !s_vis[n]);
		if (pn->hud >= 0) {
			sys_io_hud_set_hidden(pn->hud, !s_vis[n]);
		}
		if (s_sent[n] == s_wver[n] && !all) {
			continue; /* the renderer keeps the matrix it has */
		}
		s_sent[n] = s_wver[n];
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
		case GP_VISIBLE:
			if (truthy(v)) {
				pn->flags |= 1;
			} else {
				pn->flags &= (unsigned char)~1;
			}
			s_dirty = 1;
			break;
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
		c = x < y ? -1 : (x > y ? 1 : 0);
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
#ifdef GDIR_CLOCK
extern "C" unsigned GDIR_CLOCK(void);
/* GDIR_CLOCK(): a 16-bit tick counter the guest provides; time in natives */
static unsigned s_nat_time[GN_COUNT], s_nat_calls[GN_COUNT];
static unsigned s_recompute_ticks; /* the frame-end node pass */
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
		case GN_NODE_CHILD_COUNT: {
			int c = 0;
			if (a[0].t == T_NODE) {
				for (unsigned i = 0; i < s_node_n; i++) {
					c += s_nodes[i].parent == a[0].i;
				}
			}
			return vint(c);
		}
		case GN_NODE_GET_CHILD: {
			int want = inum(a[1]);
			if (a[0].t == T_NODE) {
				for (unsigned i = 0; i < s_node_n; i++) {
					if (s_nodes[i].parent == a[0].i && want-- == 0) {
						return vnode((int)i);
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

/* ------------------------------------------------------------ interpreter */
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
	const unsigned base = s_sp;
	if (base + f->locals + 64 >= GDIR_STACK) {
		fail("stack overflow");
		return vnil();
	}
	for (unsigned i = 0; i < f->locals; i++) {
		s_stack[base + i] = (int)i < argc ? args[i] : vnil();
	}
	s_sp = base + f->locals;
	s_depth++;
#ifdef GDIR_STATS
	const unsigned ops0 = s_ops;
#endif
	const unsigned mem = s_inst_mem[inst];
	const unsigned cls = s_inst[inst].cls;
	unsigned pc = f->code;
	Val ret = vnil();
	char lbuf[GDIR_STR];
	(void)lbuf;
	for (;;) {
		if (++s_ops > GDIR_OPS_PER_FRAME) {
			fail("script ran too long in one frame (infinite loop?)");
			break;
		}
		if (pc >= s_code_n) {
			fail("pc out of range");
			break;
		}
		const unsigned char op = s_code[pc++];
		switch (op) {
			case GOP_NOP:
				break;
			case GOP_CONST: {
				const unsigned k = ru16(s_code + pc);
				pc += 2;
				s_stack[s_sp++] = k < s_k_n ? s_k[k] : vnil();
			} break;
			case GOP_NIL:
				s_stack[s_sp++] = vnil();
				break;
			case GOP_TRUE:
				s_stack[s_sp++] = vbool(1);
				break;
			case GOP_FALSE:
				s_stack[s_sp++] = vbool(0);
				break;
			case GOP_LLOAD:
				s_stack[s_sp] = s_stack[base + s_code[pc++]];
				s_sp++;
				break;
			case GOP_LSTORE:
				s_stack[base + s_code[pc++]] = s_stack[--s_sp];
				break;
			case GOP_MLOAD:
				s_stack[s_sp++] = s_mem[mem + s_code[pc++]];
				break;
			case GOP_MSTORE: {
				const unsigned m = mem + s_code[pc++];
				s_mem[m] = persist(s_stack[--s_sp], s_memstr[m], GDIR_STR);
			} break;
			case GOP_SELF:
				s_stack[s_sp++] = vnode(s_inst[inst].node);
				break;
			case GOP_NODEREF: {
				const unsigned r = s_code[pc++];
				int nd = -1;
				if (r < s_cls[cls].noderefs) {
					const unsigned v = ru16(s_inst[inst].refs + r * 2);
					nd = v == 0xffff ? -1 : (int)v;
				}
				s_stack[s_sp++] = vnode(nd);
			} break;
			case GOP_GETP: {
				const int p = s_code[pc++];
				s_stack[s_sp - 1] = get_prop(s_stack[s_sp - 1], p);
			} break;
			case GOP_SETP: {
				const int p = s_code[pc++];
				const Val o = s_stack[--s_sp];
				const Val v = s_stack[--s_sp];
				s_stack[s_sp++] = set_prop(o, p, v);
			} break;
			case GOP_ADD:
			case GOP_SUB:
			case GOP_MUL:
			case GOP_DIV:
			case GOP_MOD:
			case GOP_BAND:
			case GOP_BOR: {
				const Val b = s_stack[--s_sp];
				const Val a = s_stack[--s_sp];
				s_stack[s_sp++] = arith(op, a, b);
			} break;
			case GOP_NEG: {
				Val v = s_stack[s_sp - 1];
				if (v.t == T_INT) {
					v.i = -v.i;
				} else if (v.t == T_FLOAT) {
					v.f = -v.f;
				} else if (v.t == T_VEC2 || v.t == T_VEC3) {
					v.v[0] = -v.v[0];
					v.v[1] = -v.v[1];
					v.v[2] = -v.v[2];
				}
				s_stack[s_sp - 1] = v;
			} break;
			case GOP_EQ:
			case GOP_NE:
			case GOP_LT:
			case GOP_LE:
			case GOP_GT:
			case GOP_GE: {
				const Val b = s_stack[--s_sp];
				const Val a = s_stack[--s_sp];
				s_stack[s_sp++] = vbool(compare(op, a, b));
			} break;
			case GOP_IN: {
				const Val c = s_stack[--s_sp];
				const Val v = s_stack[--s_sp];
				Val pair[2] = { c, v };
				s_stack[s_sp++] = native(inst, GN_ARR_HAS, pair, 2);
			} break;
			case GOP_NOT:
				s_stack[s_sp - 1] = vbool(!truthy(s_stack[s_sp - 1]));
				break;
			case GOP_JMP:
				pc = ru32(s_code + pc);
				break;
			case GOP_JZ: {
				const unsigned to = ru32(s_code + pc);
				pc += 4;
				if (!truthy(s_stack[--s_sp])) {
					pc = to;
				}
			} break;
			case GOP_JNZ: {
				const unsigned to = ru32(s_code + pc);
				pc += 4;
				if (truthy(s_stack[--s_sp])) {
					pc = to;
				}
			} break;
			case GOP_JZK: {
				const unsigned to = ru32(s_code + pc);
				pc += 4;
				if (!truthy(s_stack[s_sp - 1])) {
					pc = to;
				} else {
					s_sp--;
				}
			} break;
			case GOP_JNZK: {
				const unsigned to = ru32(s_code + pc);
				pc += 4;
				if (truthy(s_stack[s_sp - 1])) {
					pc = to;
				} else {
					s_sp--;
				}
			} break;
			case GOP_POP:
				s_sp--;
				break;
			case GOP_DUP:
				s_stack[s_sp] = s_stack[s_sp - 1];
				s_sp++;
				break;
			case GOP_CALL: {
				const unsigned fn = ru16(s_code + pc);
				const int argc2 = s_code[pc + 2];
				pc += 3;
				Val a[16];
				for (int i = argc2 - 1; i >= 0; i--) {
					a[i] = s_stack[--s_sp];
				}
				const Val r = call(inst, s_cls[cls].first_func + fn, a, argc2);
				s_stack[s_sp++] = r;
			} break;
			case GOP_NATIVE: {
				const int id = s_code[pc];
				const int argc2 = s_code[pc + 1];
				pc += 2;
				Val a[16];
				for (int i = argc2 - 1; i >= 0; i--) {
					a[i] = s_stack[--s_sp];
				}
#if defined(GDIR_STATS) && defined(GDIR_CLOCK)
				const unsigned t0 = GDIR_CLOCK();
				s_stack[s_sp++] = native(inst, id, a, argc2);
				if (id >= 0 && id < GN_COUNT) {
					s_nat_time[id] += (GDIR_CLOCK() - t0) & 0xffff;
					s_nat_calls[id]++;
				}
#else
				s_stack[s_sp++] = native(inst, id, a, argc2);
#endif
			} break;
			case GOP_RET:
				ret = s_stack[--s_sp];
				goto done;
			case GOP_RETNIL:
				goto done;
			case GOP_ARRAY: {
				const int cnt = s_code[pc++];
				const int ai = arr_new(); /* before popping: elements stay marked */
				Val v = vnil();
				v.t = T_ARR;
				v.i = ai;
				s_sp -= (unsigned)cnt;
				if (ai >= 0) {
					for (int i = 0; i < cnt && i < GDIR_ARR_LEN; i++) {
						arr_set(ai, i, s_stack[s_sp + i]);
					}
					s_arr_n[ai] = (unsigned char)(cnt < GDIR_ARR_LEN ? cnt : GDIR_ARR_LEN);
				}
				s_stack[s_sp++] = v;
			} break;
			case GOP_INDEX: {
				const Val idx = s_stack[--s_sp];
				const Val b = s_stack[--s_sp];
				Val r = vnil();
				int i = inum(idx);
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
				s_stack[s_sp++] = r;
			} break;
			case GOP_SETINDEX: {
				const Val idx = s_stack[--s_sp];
				const Val b = s_stack[--s_sp];
				const Val v = s_stack[--s_sp];
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
			} break;
			default:
				fail("bad opcode");
				goto done;
		}
		if (s_err[0]) {
			break;
		}
	}
done:
#ifdef GDIR_STATS
	s_fn_ops[func] += s_ops - ops0;
#endif
	s_sp = base;
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
	for (unsigned n = 0; n < s_node_n; n++) {
		PackNode *pn = &s_nodes[n];
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
		p += tmp.node_n * 88u;
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
	s_delta = delta;
	s_ticks_ms += (unsigned)(delta * 1000.0f);
	s_ops = 0;
	s_mem_n_gc = s_mem_n;
	s_sp_gc = 0;
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
			if (k->fn_process >= 0) {
				const Val d = vfloat(delta);
				call(i, k->first_func + (unsigned)k->fn_process, &d, 1);
			}
			if (k->fn_physics >= 0) {
				const Val d = vfloat(delta);
				call(i, k->first_func + (unsigned)k->fn_physics, &d, 1);
			}
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
#ifdef GDIR_STATS
	{
		static unsigned peak, frames;
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
			printf("  node pass          %u ticks/frame\n", s_recompute_ticks / 300);
			s_recompute_ticks = 0;
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
