// MIT. Stream extra packs from host: (ELF dir HostFs) then cdrom0:. SAVE.BIN is user:// stub.

#include "pack_io.h"
#include "iso_path.h"

#include "gs_draw.h"
#include "sys_io.h"
#include "vu1_draw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PACK_IO_HEAP_REPORT
extern "C" void PACK_IO_HEAP_REPORT(void); /* the guest's heap, on "no memory" */
#endif

#define PS2_CATALOG 99
#define PS2_LAYERS 16
#ifndef PS2_EE_LIMIT
#define PS2_EE_LIMIT (24u * 1024u * 1024u)
#endif
#ifndef PS2_GS_LIMIT
#define PS2_GS_LIMIT (4u * 1024u * 1024u)
#endif
#ifndef PS2_EXTRA_EE_CAP
#define PS2_EXTRA_EE_CAP (2u * 1024u * 1024u)
#endif
#ifndef PS2_POKE_N
#define PS2_POKE_N (24u * 1024u)
#endif

static unsigned char s_poke[PS2_POKE_N];

static int s_toc_ok;
static int s_current;
static int s_has_pack1;
static int s_has_stream;
static int s_max_pack;
static char s_paths[100][128];
static unsigned s_cost_ee[100];
static unsigned s_cost_gs[100];

typedef struct {
	int pack;
	unsigned char *mesh;
	unsigned char *gtex;
	unsigned char *node;
	unsigned mesh_sz;
	unsigned gtex_sz;
	unsigned node_sz;
	unsigned ee;
	unsigned gs;
} PackLayer;

static PackLayer s_ly[PS2_LAYERS];
static int s_ly_n;
static unsigned s_p0_ee;
static unsigned s_p0_gs;

static const unsigned char *s_p0_mesh;
static const unsigned char *s_p0_gtex;
static const unsigned char *s_p0_node;
static unsigned s_p0_mesh_sz;
static unsigned s_p0_gtex_sz;
static unsigned s_p0_node_sz;

static unsigned char *s_px_mesh;
static unsigned char *s_px_gtex;
static unsigned char *s_px_node;
static unsigned s_px_mesh_sz;
static unsigned s_px_gtex_sz;
static unsigned s_px_node_sz;
static int s_px_id = -1;
static int s_user_present;
static int s_user_ready;
static char s_user_err[96];

static unsigned ru16(const unsigned char *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned ru32(const unsigned char *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static int parse_pack(const unsigned char *blob, unsigned sz)
{
	if (!blob || sz < 8) {
		return 0;
	}
	if (blob[0] != 'P' || blob[1] != 'A' || blob[2] != 'C' || blob[3] != 'K') {
		return 0;
	}
	if (ru16(blob + 4) != 1) {
		return 0;
	}
	return ru16(blob + 6) > 0;
}

static int parse_stream(const unsigned char *blob, unsigned sz)
{
	if (!blob || sz < 8) {
		return 0;
	}
	if (blob[0] != 'S' || blob[1] != 'T' || blob[2] != 'R' || blob[3] != 'M') {
		return 0;
	}
	if (ru16(blob + 4) != 1) {
		return 0;
	}
	const unsigned count = ru16(blob + 6);
	if (count == 0 || 8 + count * 2 > sz) {
		return 0;
	}
	unsigned maxp = 0;
	for (unsigned i = 0; i < count; i++) {
		const unsigned id = ru16(blob + 8 + i * 2);
		if (id > maxp) {
			maxp = id;
		}
	}
	s_max_pack = (int)maxp;
	unsigned off = 8 + count * 2;
	for (unsigned i = 0; i < count && off < sz; i++) {
		const unsigned id = ru16(blob + 8 + i * 2);
		const unsigned len = blob[off++];
		if (off + len > sz || id > PS2_CATALOG) {
			break;
		}
		unsigned n = len;
		if (n > 127) {
			n = 127;
		}
		memcpy(s_paths[id], blob + off, n);
		s_paths[id][n] = 0;
		off += len;
		if (off + 8 <= sz) {
			s_cost_ee[id] = ru32(blob + off);
			off += 4;
			s_cost_gs[id] = ru32(blob + off);
			off += 4;
		}
	}
	return maxp > 0;
}

static FILE *open_named(const char *host, const char *iso_bs, const char *iso)
{
	const char *paths[4];
	paths[0] = host;
	paths[1] = iso_bs;
	paths[2] = iso;
	paths[3] = 0;
	for (int i = 0; paths[i]; i++) {
		FILE *f = iso_fopen(paths[i], "rb");
		if (f) {
			return f;
		}
	}
	return NULL;
}

static int read_named(const char *host, const char *iso_bs, const char *iso,
		unsigned char **out, unsigned *sz)
{
	FILE *f = open_named(host, iso_bs, iso);
	if (!f) {
		return 0;
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return 0;
	}
	long n = ftell(f);
	if (n < 8) {
		fclose(f);
		return 0;
	}
	rewind(f);
	unsigned char *buf = (unsigned char *)malloc((size_t)n);
	if (!buf) {
		printf("pack_io: no memory for %s (%ld bytes)\n", iso, n);
#ifdef PACK_IO_HEAP_REPORT
		PACK_IO_HEAP_REPORT();
#endif
		fclose(f);
		return 0;
	}
	if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
		printf("pack_io: %s did not read\n", iso);
		free(buf);
		fclose(f);
		return 0;
	}
	fclose(f);
	*out = buf;
	*sz = (unsigned)n;
	return 1;
}

static void set_user_err(const char *msg)
{
	unsigned i = 0;
	if (!msg) {
		s_user_err[0] = 0;
		return;
	}
	while (msg[i] && i + 1 < sizeof(s_user_err)) {
		s_user_err[i] = msg[i];
		i++;
	}
	s_user_err[i] = 0;
}

static FILE *open_save(const char *mode)
{
	FILE *f = fopen("host:SAVE0.BIN", mode);
	if (f) {
		return f;
	}
	f = fopen("host:SAVE.BIN", mode);
	if (f) {
		return f;
	}
	return fopen("mc0:SAVE.BIN", mode);
}

static void write_save_stub(void)
{
	set_user_err("");
	FILE *f = open_save("wb");
	if (!f) {
		s_user_present = 0;
		s_user_ready = 0;
		set_user_err("user:// save failed: no host:SAVE.BIN or mc0: (HostFs/memcard missing)");
		return;
	}
	const char mag[] = { 'S', 'A', 'V', 'E', 1, 0 };
	fwrite(mag, 1, sizeof(mag), f);
	fclose(f);
	s_user_present = 1;
	s_user_ready = 1;
}

static void free_px(void)
{
	free(s_px_mesh);
	free(s_px_gtex);
	free(s_px_node);
	s_px_mesh = 0;
	s_px_gtex = 0;
	s_px_node = 0;
	s_px_mesh_sz = 0;
	s_px_gtex_sz = 0;
	s_px_node_sz = 0;
	s_px_id = -1;
}

static unsigned pack_cost_ee(int pack, unsigned mesh_sz, unsigned gtex_sz, unsigned node_sz)
{
	if (pack > 0 && pack <= PS2_CATALOG && s_cost_ee[pack]) {
		return s_cost_ee[pack];
	}
	return mesh_sz + gtex_sz + node_sz;
}

static unsigned pack_cost_gs(int pack, unsigned gtex_sz)
{
	if (pack > 0 && pack <= PS2_CATALOG && s_cost_gs[pack]) {
		return s_cost_gs[pack];
	}
	return gtex_sz;
}

static unsigned used_ee(void)
{
	unsigned n = s_p0_ee;
	if (s_current > 0) {
		n += pack_cost_ee(s_current, s_px_mesh_sz, s_px_gtex_sz, s_px_node_sz);
	}
	for (int i = 0; i < s_ly_n; i++) {
		n += s_ly[i].ee;
	}
	return n;
}

static unsigned used_gs(void)
{
	unsigned n = s_p0_gs;
	if (s_current > 0) {
		n += pack_cost_gs(s_current, s_px_gtex_sz);
	}
	for (int i = 0; i < s_ly_n; i++) {
		n += s_ly[i].gs;
	}
	return n;
}

static int layer_index(int pack)
{
	for (int i = 0; i < s_ly_n; i++) {
		if (s_ly[i].pack == pack) {
			return i;
		}
	}
	return -1;
}

static void free_layer(PackLayer *ly)
{
	if (!ly) {
		return;
	}
	gs_draw_layer_remove(ly->mesh);
	free(ly->mesh);
	free(ly->gtex);
	free(ly->node);
	memset(ly, 0, sizeof(*ly));
}

static int read_pack_bins(int pack, unsigned char **mesh, unsigned *mesh_sz,
		unsigned char **gtex, unsigned *gtex_sz,
		unsigned char **node, unsigned *node_sz)
{
	char host_m[40];
	char iso_m[48];
	char iso_m2[48];
	char host_g[40];
	char iso_g[48];
	char iso_g2[48];
	char host_n[40];
	char iso_n[48];
	char iso_n2[48];
	sprintf(host_m, "host:MESH%02d.bin", pack);
	sprintf(iso_m, "cdrom0:\\MESH%02d.BIN;1", pack);
	sprintf(iso_m2, "cdrom0:MESH%02d.BIN;1", pack);
	sprintf(host_g, "host:GTEX%02d.bin", pack);
	sprintf(iso_g, "cdrom0:\\GTEX%02d.BIN;1", pack);
	sprintf(iso_g2, "cdrom0:GTEX%02d.BIN;1", pack);
	sprintf(host_n, "host:NODE%02d.bin", pack);
	sprintf(iso_n, "cdrom0:\\NODE%02d.BIN;1", pack);
	sprintf(iso_n2, "cdrom0:NODE%02d.BIN;1", pack);
	if (!read_named(host_m, iso_m, iso_m2, mesh, mesh_sz)) {
		return 0;
	}
	if (!read_named(host_g, iso_g, iso_g2, gtex, gtex_sz)) {
		free(*mesh);
		*mesh = 0;
		return 0;
	}
	if (!read_named(host_n, iso_n, iso_n2, node, node_sz)) {
		free(*mesh);
		free(*gtex);
		*mesh = 0;
		*gtex = 0;
		return 0;
	}
	if (pack == 1) {
		s_has_pack1 = 1;
	}
	if (pack > s_max_pack) {
		s_max_pack = pack;
	}
	return 1;
}

static int load_pack_n(int pack)
{
	if (pack < 1) {
		return 0;
	}
	if (s_px_id == pack && s_px_mesh && s_px_gtex && s_px_node) {
		return 1;
	}
	free_px();
	if (!read_pack_bins(pack, &s_px_mesh, &s_px_mesh_sz, &s_px_gtex, &s_px_gtex_sz, &s_px_node, &s_px_node_sz)) {
		return 0;
	}
	s_px_id = pack;
	return 1;
}

#ifdef PACK_IO_RELOAD_PACK0
/* A guest short of RAM (the PS1) lets pack 0 go while another pack is the
 * scene, and reads it from the disc again when the game comes back. */
static void drop_p0(void)
{
	free((void *)s_p0_mesh);
	free((void *)s_p0_gtex);
	free((void *)s_p0_node);
	s_p0_mesh = 0;
	s_p0_gtex = 0;
	s_p0_node = 0;
	s_p0_mesh_sz = s_p0_gtex_sz = s_p0_node_sz = 0;
	s_p0_ee = 0;
}

static int reload_p0(void)
{
	unsigned char *mesh = 0, *gtex = 0, *node = 0;
	unsigned msz = 0, gsz = 0, nsz = 0;
	if (!read_pack_bins(0, &mesh, &msz, &gtex, &gsz, &node, &nsz)) {
		printf("pack_io: pack 0 did not come back\n");
		return 0;
	}
	s_p0_mesh = mesh;
	s_p0_mesh_sz = msz;
	s_p0_gtex = gtex;
	s_p0_gtex_sz = gsz;
	s_p0_node = node;
	s_p0_node_sz = nsz;
	s_p0_ee = msz + gsz + nsz;
	s_p0_gs = gsz;
	return 1;
}
#endif

void pack_io_set_pack0(const unsigned char *mesh, unsigned mesh_sz,
		const unsigned char *gtex, unsigned gtex_sz,
		const unsigned char *node, unsigned node_sz)
{
	s_p0_mesh = mesh;
	s_p0_mesh_sz = mesh_sz;
	s_p0_gtex = gtex;
	s_p0_gtex_sz = gtex_sz;
	s_p0_node = node;
	s_p0_node_sz = node_sz;
	s_p0_ee = mesh_sz + gtex_sz + node_sz;
	s_p0_gs = gtex_sz;
	s_current = 0;
}

int pack_io_swap(int pack)
{
	if (pack <= 0) {
		free_px();
#ifdef PACK_IO_RELOAD_PACK0
		if (!s_p0_mesh && !reload_p0()) {
			return 0;
		}
#endif
		if (!s_p0_mesh) {
			return 0;
		}
		if (!gs_draw_init(s_p0_mesh, s_p0_mesh_sz, s_p0_gtex, s_p0_gtex_sz, s_p0_node, s_p0_node_sz)) {
			return 0;
		}
#ifdef PACK_IO_RELOAD_PACK0
		/* the textures are in VRAM now */
		free((void *)s_p0_gtex);
		s_p0_gtex = 0;
		s_p0_ee -= s_p0_gtex_sz;
		s_p0_gtex_sz = 0;
#endif
		vu1_draw_init(s_p0_mesh, s_p0_mesh_sz);
		s_current = 0;
		sys_io_load_pack(0);
		return 1;
	}
	if (pack == s_current && s_px_id == pack && s_px_mesh) {
		return 1;
	}
	{
		const int li = layer_index(pack);
		if (li >= 0) {
			free_px();
			if (!gs_draw_init(s_ly[li].mesh, s_ly[li].mesh_sz, s_ly[li].gtex, s_ly[li].gtex_sz, s_ly[li].node, s_ly[li].node_sz)) {
				return 0;
			}
			vu1_draw_init(s_ly[li].mesh, s_ly[li].mesh_sz);
			s_current = pack;
			sys_io_load_pack(pack);
			return 1;
		}
	}
	{
		unsigned ee = used_ee();
		unsigned gs = used_gs();
		if (s_current > 0) {
			ee -= pack_cost_ee(s_current, s_px_mesh_sz, s_px_gtex_sz, s_px_node_sz);
			gs -= pack_cost_gs(s_current, s_px_gtex_sz);
		}
#ifdef PACK_IO_RELOAD_PACK0
		ee -= s_p0_ee; /* pack 0 goes */
#endif
		if (ee + pack_io_cost_ee(pack) > PS2_EE_LIMIT || gs + pack_io_cost_gs(pack) > PS2_GS_LIMIT) {
			printf("pack_io: pack %d does not fit (RAM %u + %u of %u, VRAM %u + %u of %u)\n", pack, ee, pack_io_cost_ee(pack),
					(unsigned)PS2_EE_LIMIT, gs, pack_io_cost_gs(pack), (unsigned)PS2_GS_LIMIT);
			return 0;
		}
	}
#ifdef PACK_IO_RELOAD_PACK0
	drop_p0();
#endif
	if (!load_pack_n(pack)) {
		printf("pack_io: pack %d did not load\n", pack);
#ifdef PACK_IO_RELOAD_PACK0
		pack_io_swap(0); /* back to where the game was */
#endif
		return 0;
	}
	if (!gs_draw_init(s_px_mesh, s_px_mesh_sz, s_px_gtex, s_px_gtex_sz, s_px_node, s_px_node_sz)) {
		printf("pack_io: pack %d did not draw\n", pack);
#ifdef PACK_IO_RELOAD_PACK0
		free_px();
		pack_io_swap(0);
#endif
		return 0;
	}
	vu1_draw_init(s_px_mesh, s_px_mesh_sz);
	s_current = pack;
	sys_io_load_pack(pack);
	return 1;
}

int pack_io_init(void)
{
	s_toc_ok = 0;
	s_has_stream = 0;
	s_max_pack = 0;
	s_ly_n = 0;
	memset(s_paths, 0, sizeof(s_paths));
	memset(s_cost_ee, 0, sizeof(s_cost_ee));
	memset(s_cost_gs, 0, sizeof(s_cost_gs));
	memset(s_ly, 0, sizeof(s_ly));
	memset(s_poke, 0, sizeof(s_poke));
	unsigned char *buf = 0;
	unsigned sz = 0;
	if (read_named("host:STREAM.bin", "cdrom0:\\STREAM.BIN;1", "cdrom0:STREAM.BIN;1", &buf, &sz)) {
		s_has_stream = parse_stream(buf, sz);
		s_toc_ok = s_has_stream;
		free(buf);
		buf = 0;
	}
	if (read_named("host:PACK01.bin", "cdrom0:\\PACK01.BIN;1", "cdrom0:PACK01.BIN;1", &buf, &sz)) {
		if (parse_pack(buf, sz)) {
			s_toc_ok = 1;
			if (s_max_pack < 1) {
				s_max_pack = 1;
			}
		}
		free(buf);
	}
	write_save_stub();
	return s_toc_ok;
}

int pack_io_loaded(void)
{
	return s_toc_ok;
}

int pack_io_current(void)
{
	return s_current;
}

int pack_io_has_pack1(void)
{
	return s_has_pack1;
}

int pack_io_max(void)
{
	return s_max_pack;
}

int pack_io_has_stream(void)
{
	return s_has_stream;
}

int pack_io_find_path(const char *path)
{
	if (!path || !path[0]) {
		return -1;
	}
	for (int i = 0; i <= PS2_CATALOG; i++) {
		if (s_paths[i][0] && strcmp(s_paths[i], path) == 0) {
			return i;
		}
	}
	return -1;
}

int pack_io_can_fit(int pack)
{
	if (pack < 1) {
		return 0;
	}
	if (pack_io_is_loaded(pack)) {
		return 1;
	}
	const unsigned ee = pack_io_cost_ee(pack);
	const unsigned gs = pack_io_cost_gs(pack);
	if (ee > PS2_EXTRA_EE_CAP) {
		return 0;
	}
	if (used_ee() + ee > PS2_EE_LIMIT || used_gs() + gs > PS2_GS_LIMIT) {
		return 0;
	}
	return 1;
}

int pack_io_prefetch(int pack)
{
	if (pack < 1) {
		return 0;
	}
	if (!pack_io_can_fit(pack) && !pack_io_is_loaded(pack)) {
		return 0;
	}
	char host[40];
	char iso_bs[48];
	char iso[48];
	sprintf(host, "host:MESH%02d.bin", pack);
	sprintf(iso_bs, "cdrom0:\\MESH%02d.BIN;1", pack);
	sprintf(iso, "cdrom0:MESH%02d.BIN;1", pack);
	FILE *f = open_named(host, iso_bs, iso);
	if (!f) {
		return 0;
	}
	fclose(f);
	return 1;
}

int pack_io_instantiate(int pack)
{
	if (pack < 1) {
		return 0;
	}
	if (pack == s_current || layer_index(pack) >= 0) {
		return 1;
	}
	if (s_ly_n >= PS2_LAYERS) {
		return 0;
	}
	if (!pack_io_can_fit(pack)) {
		return 0;
	}
	PackLayer ly;
	memset(&ly, 0, sizeof(ly));
	ly.pack = pack;
	if (!read_pack_bins(pack, &ly.mesh, &ly.mesh_sz, &ly.gtex, &ly.gtex_sz, &ly.node, &ly.node_sz)) {
		return 0;
	}
	ly.ee = pack_cost_ee(pack, ly.mesh_sz, ly.gtex_sz, ly.node_sz);
	ly.gs = pack_cost_gs(pack, ly.gtex_sz);
	if (used_ee() + ly.ee > PS2_EE_LIMIT || used_gs() + ly.gs > PS2_GS_LIMIT) {
		free(ly.mesh);
		free(ly.gtex);
		free(ly.node);
		return 0;
	}
	if (!gs_draw_layer_add(ly.mesh, ly.mesh_sz)) {
		free(ly.mesh);
		free(ly.gtex);
		free(ly.node);
		return 0;
	}
	s_ly[s_ly_n++] = ly;
	return 1;
}

int pack_io_load_scene(int pack)
{
	return pack_io_instantiate(pack);
}

int pack_io_unload(int pack)
{
	int found = 0;
	int w = 0;
	for (int i = 0; i < s_ly_n; i++) {
		if (s_ly[i].pack == pack) {
			free_layer(&s_ly[i]);
			found = 1;
			continue;
		}
		s_ly[w++] = s_ly[i];
	}
	s_ly_n = w;
	if (s_current == pack) {
		pack_io_swap(0);
		found = 1;
	}
	return found;
}

int pack_io_is_loaded(int pack)
{
	if (pack == 0 || pack == s_current) {
		return 1;
	}
	return layer_index(pack) >= 0;
}

int pack_io_loaded_count(void)
{
	return 1 + s_ly_n + (s_current > 0 ? 1 : 0);
}

unsigned pack_io_ee_used(void)
{
	return used_ee();
}

unsigned pack_io_ee_limit(void)
{
	return PS2_EE_LIMIT;
}

unsigned pack_io_ee_free(void)
{
	const unsigned u = used_ee();
	return u >= PS2_EE_LIMIT ? 0 : (PS2_EE_LIMIT - u);
}

unsigned pack_io_gs_used(void)
{
	return used_gs();
}

unsigned pack_io_gs_limit(void)
{
	return PS2_GS_LIMIT;
}

unsigned pack_io_gs_free(void)
{
	const unsigned u = used_gs();
	return u >= PS2_GS_LIMIT ? 0 : (PS2_GS_LIMIT - u);
}

unsigned pack_io_cost_ee(int pack)
{
	if (pack <= 0) {
		return s_p0_ee;
	}
	if (pack <= PS2_CATALOG && s_cost_ee[pack]) {
		return s_cost_ee[pack];
	}
	return 0;
}

unsigned pack_io_cost_gs(int pack)
{
	if (pack <= 0) {
		return s_p0_gs;
	}
	if (pack <= PS2_CATALOG && s_cost_gs[pack]) {
		return s_cost_gs[pack];
	}
	return 0;
}

void pack_io_current_node(const unsigned char **out, unsigned *sz)
{
	if (!out || !sz) {
		return;
	}
	if (s_current <= 0) {
		*out = s_p0_node;
		*sz = s_p0_node_sz;
		return;
	}
	if (s_px_id == s_current && s_px_node) {
		*out = s_px_node;
		*sz = s_px_node_sz;
		return;
	}
	{
		const int li = layer_index(s_current);
		if (li >= 0) {
			*out = s_ly[li].node;
			*sz = s_ly[li].node_sz;
			return;
		}
	}
	*out = s_px_node;
	*sz = s_px_node_sz;
}

int pack_io_user_present(void)
{
	return s_user_present;
}

int pack_io_user_ready(void)
{
	return s_user_ready;
}

int pack_io_user_save(const void *data, unsigned n)
{
	FILE *f = open_save("wb");
	if (!f) {
		s_user_present = 0;
		s_user_ready = 0;
		set_user_err("user:// save failed: no host:SAVE.BIN or mc0: (HostFs/memcard missing)");
		return 0;
	}
	const char mag[] = { 'S', 'A', 'V', 'E', 1, 0 };
	fwrite(mag, 1, sizeof(mag), f);
	if (data && n) {
		fwrite(data, 1, n, f);
	}
	fclose(f);
	s_user_present = 1;
	s_user_ready = 1;
	set_user_err("");
	return 1;
}

int pack_io_user_load(void *data, unsigned maxn)
{
	FILE *f = open_save("rb");
	if (!f) {
		set_user_err("user:// load failed: SAVE.BIN not found on host: or mc0:");
		return -1;
	}
	unsigned char mag[6];
	const size_t got = fread(mag, 1, 6, f);
	const int header = (got == 6 && mag[0] == 'S' && mag[1] == 'A' && mag[2] == 'V' && mag[3] == 'E');
	int n = 0;
	if (data && maxn) {
		unsigned char *dst = (unsigned char *)data;
		if (!header) {
			if (got > 0) {
				const unsigned copy = got < maxn ? (unsigned)got : maxn;
				memcpy(dst, mag, copy);
				n = (int)copy;
			}
			if (n < (int)maxn) {
				n += (int)fread(dst + n, 1, maxn - (unsigned)n, f);
			}
		} else {
			n = (int)fread(dst, 1, maxn, f);
		}
	}
	fclose(f);
	if (n > 0 && data) {
		const unsigned copy = (unsigned)n < PS2_POKE_N ? (unsigned)n : PS2_POKE_N;
		memcpy(s_poke, data, copy);
	}
	s_user_present = 1;
	s_user_ready = 1;
	set_user_err("");
	return n;
}

static FILE *open_save_slot(int slot, const char *mode)
{
	char host[24];
	char mc[24];
	if (slot < 0) {
		slot = 0;
	}
	if (slot > 3) {
		slot = 3;
	}
	sprintf(host, "host:SAVE%d.BIN", slot);
	sprintf(mc, "mc0:SAVE%d.BIN", slot);
	FILE *f = fopen(host, mode);
	if (f) {
		return f;
	}
	if (slot == 0) {
		f = fopen("host:SAVE.BIN", mode);
		if (f) {
			return f;
		}
	}
	return fopen(mc, mode);
}

int pack_io_user_save_slot(int slot, const void *data, unsigned n)
{
	FILE *f = open_save_slot(slot, "wb");
	if (!f) {
		s_user_present = 0;
		s_user_ready = 0;
		set_user_err("memcard save_slot failed: no host:SAVE#.BIN or mc0:");
		return 0;
	}
	if (!data || !n) {
		data = s_poke;
		n = PS2_POKE_N;
	}
	const char mag[] = { 'S', 'A', 'V', 'E', 1, 0 };
	fwrite(mag, 1, sizeof(mag), f);
	if (data && n) {
		fwrite(data, 1, n, f);
	}
	fclose(f);
	s_user_present = 1;
	s_user_ready = 1;
	set_user_err("");
	return 1;
}

int pack_io_user_load_slot(int slot, void *data, unsigned maxn)
{
	FILE *f = open_save_slot(slot, "rb");
	if (!f) {
		set_user_err("memcard load_slot failed: SAVE#.BIN not found on host: or mc0:");
		return -1;
	}
	unsigned char mag[6];
	const size_t got = fread(mag, 1, 6, f);
	const int header = (got == 6 && mag[0] == 'S' && mag[1] == 'A' && mag[2] == 'V' && mag[3] == 'E');
	int n = 0;
	if (data && maxn) {
		unsigned char *dst = (unsigned char *)data;
		if (!header) {
			if (got > 0) {
				const unsigned copy = got < maxn ? (unsigned)got : maxn;
				memcpy(dst, mag, copy);
				n = (int)copy;
			}
			if (n < (int)maxn) {
				n += (int)fread(dst + n, 1, maxn - (unsigned)n, f);
			}
		} else {
			n = (int)fread(dst, 1, maxn, f);
		}
	}
	fclose(f);
	if (n > 0 && data) {
		const unsigned copy = (unsigned)n < PS2_POKE_N ? (unsigned)n : PS2_POKE_N;
		memcpy(s_poke, data, copy);
	}
	s_user_present = 1;
	s_user_ready = 1;
	set_user_err("");
	return n;
}

int pack_io_poke(unsigned off, unsigned char v)
{
	if (off >= PS2_POKE_N) {
		return 0;
	}
	s_poke[off] = v;
	return 1;
}

int pack_io_peek(unsigned off)
{
	if (off >= PS2_POKE_N) {
		return 0;
	}
	return (int)s_poke[off];
}

unsigned pack_io_poke_size(void)
{
	return PS2_POKE_N;
}

const unsigned char *pack_io_poke_data(void)
{
	return s_poke;
}

int pack_io_user_format(void)
{
	int ok = 0;
	for (int i = 0; i < 4; i++) {
		if (pack_io_user_save_slot(i, 0, 0)) {
			ok = 1;
		}
	}
	if (!ok) {
		set_user_err("memcard_format failed: no host: or mc0:");
	}
	return ok;
}

const char *pack_io_user_error(void)
{
	return s_user_err;
}
