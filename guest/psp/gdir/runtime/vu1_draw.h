// MIT. No VU1 on the PSP (the GE draws everything): pack_io calls this and gets "not used".
#ifndef BLAZIUM_PSP_VU1_DRAW_H
#define BLAZIUM_PSP_VU1_DRAW_H

static inline int vu1_draw_init(const unsigned char *mesh, unsigned mesh_sz)
{
	(void)mesh;
	(void)mesh_sz;
	return 0;
}

#endif
