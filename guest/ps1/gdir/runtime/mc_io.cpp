// MIT. Memory card saves are not on the PS1 runtime yet: no card, so the
// shared runtime says "no memory card" and carries on.

#include "mc_io.h"

int mc_io_boot(void)
{
	return 0;
}

int mc_io_present(void)
{
	return 0;
}

int mc_io_save(int slot, const unsigned char *data, unsigned n)
{
	(void)slot;
	(void)data;
	(void)n;
	return 0;
}

int mc_io_load(int slot, unsigned char *data, unsigned maxn)
{
	(void)slot;
	(void)data;
	(void)maxn;
	return -2;
}
