// MIT. Open host: then cdrom0: for STREAM / PACK01 / MESH%02d / GTEX%02d / NODE%02d / DISP.
// user:// stub is host:SAVE.BIN. Pack 0 stays ELF-embedded.

#ifndef BLAZIUM_PS2_PACK_IO_H
#define BLAZIUM_PS2_PACK_IO_H

#ifdef __cplusplus
extern "C" {
#endif

int pack_io_init(void);
int pack_io_loaded(void);

void pack_io_set_pack0(const unsigned char *mesh, unsigned mesh_sz,
		const unsigned char *gtex, unsigned gtex_sz,
		const unsigned char *node, unsigned node_sz);

// 0 = embed, n>0 = MESH%02d/GTEX%02d/NODE%02d from host: or ISO. Re-inits gs_draw.
int pack_io_swap(int pack);
int pack_io_current(void);
int pack_io_has_pack1(void);
int pack_io_max(void);
int pack_io_has_stream(void);
int pack_io_find_path(const char *path);
int pack_io_instantiate(int pack);
int pack_io_load_scene(int pack);
int pack_io_unload(int pack);
int pack_io_is_loaded(int pack);
int pack_io_loaded_count(void);
int pack_io_can_fit(int pack);
int pack_io_prefetch(int pack);
unsigned pack_io_ee_used(void);
unsigned pack_io_ee_free(void);
unsigned pack_io_ee_limit(void);
unsigned pack_io_gs_used(void);
unsigned pack_io_gs_free(void);
unsigned pack_io_gs_limit(void);
unsigned pack_io_cost_ee(int pack);
unsigned pack_io_cost_gs(int pack);
void pack_io_current_node(const unsigned char **out, unsigned *sz);

// user:// → host:SAVE.BIN then mc0:SAVE.BIN. Errors if neither is writable.
int pack_io_user_present(void);
int pack_io_user_ready(void);
int pack_io_user_save(const void *data, unsigned n);
int pack_io_user_load(void *data, unsigned maxn);
int pack_io_user_save_slot(int slot, const void *data, unsigned n);
int pack_io_user_load_slot(int slot, void *data, unsigned maxn);
int pack_io_user_format(void);
const char *pack_io_user_error(void);
int pack_io_poke(unsigned off, unsigned char v);
int pack_io_peek(unsigned off);
unsigned pack_io_poke_size(void);
const unsigned char *pack_io_poke_data(void);

#ifdef __cplusplus
}
#endif

#endif
