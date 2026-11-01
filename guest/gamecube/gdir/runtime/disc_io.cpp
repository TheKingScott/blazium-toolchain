// MIT. The game's disc: the cooked files are files of the GameCube disc
// image (gamecube build writes it: DATA/ in its file system table, FST).
// disc_init reads the disc header and the FST through the drive, and
// disc_fopen opens a file as a stdio FILE (fopencookie) that reads straight
// from the disc: the shared runtime's fopen/fread/fseek then work as on the
// PS2's CD.
//
// The reads talk to the disc interface (DI) here, not through libogc's DVD:
// the drive is ready (the disc just booted), so a read is the one DMA
// command, waited for by polling. Nintendont, which plays GameCube games
// on a Wii, has no drive for the game: as the game loads it points the code
// that reaches the DI ("lis rX, 0xCC00" then "addi rY, rX, 0x6000", as
// Nintendo's SDK has it) at its own DI, which reads the ISO from the SD
// card or USB drive, but that DI only answers reads (and only raises an
// interrupt when asked). libogc's DVD resets the drive, asks it what it is,
// patches its firmware and waits for its interrupts, so under Nintendont
// it waited for good (a black screen). di_regs() builds the address the
// SDK's way, and nothing else is asked of the drive.

#define _GNU_SOURCE 1
#include "disc_io.h"

#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>

static int s_ok;
static unsigned char *s_fst;
static unsigned s_fst_n;  /* entries */
static const char *s_names;
/* reads go through this: the drive wants 32-byte aligned buffers and
 * lengths, and offsets that are a multiple of 4 */
#define BOUNCE (32 * 1024)
static unsigned char *s_bounce;

static unsigned be32(const unsigned char *p)
{
	return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
}

/* the DI's registers, 0xCC006000, built as Nintendont looks for them (one
 * asm block, so the two instructions stay together; "b": not r0, which
 * addi takes as 0) */
static inline volatile unsigned *di_regs(void)
{
	volatile unsigned *p;
	__asm__ volatile("lis %0,0xCC00\n\taddi %0,%0,0x6000" : "=b"(p));
	return p;
}

enum { DISR = 0, DICVR = 1, DICMD0 = 2, DICMD1 = 3, DICMD2 = 4, DIMAR = 5, DILENGTH = 6, DICR = 7 };
#define DISR_CLEAR 0x54u /* the three interrupt bits written back, their masks off */
#define DISR_DEINT 0x04u /* the drive reported an error */

/* one DMA command (cmd0 0xA8000000 a read, 0xA8000040 the disc ID): len
 * (a multiple of 32) bytes at disc offset off (a
 * multiple of 4) into dst (32-byte aligned). 0 on a drive error or after
 * ten seconds with no answer. */
static int di_command(unsigned cmd0, void *dst, unsigned len, unsigned off)
{
	volatile unsigned *di = di_regs();
	DCInvalidateRange(dst, len);
	di[DISR] = DISR_CLEAR;
	di[DICMD0] = cmd0;
	di[DICMD1] = off >> 2;
	di[DICMD2] = len;
	di[DIMAR] = (unsigned)(uintptr_t)dst & 0x1FFFFFFFu;
	di[DILENGTH] = len;
	di[DICR] = 3; /* DMA, start */
	const u64 t0 = gettime();
	while (di[DICR] & 1) {
		if (diff_sec(t0, gettime()) > 10) {
			printf("GC disc: no answer to a read at %u\n", off);
			return 0;
		}
	}
	const unsigned sr = di[DISR];
	di[DISR] = DISR_CLEAR;
	DCInvalidateRange(dst, len);
	if (sr & DISR_DEINT) {
		printf("GC disc: the drive failed a read at %u\n", off);
		return 0;
	}
	return 1;
}

static int di_read(void *dst, unsigned len, unsigned off)
{
	return di_command(0xA8000000u, dst, len, off);
}

/* n bytes at disc offset off into dst */
static int disc_read(void *dst, unsigned n, unsigned off)
{
	unsigned char *d = (unsigned char *)dst;
	while (n) {
		const unsigned a = off & ~3u;
		const unsigned skip = off - a;
		unsigned want = skip + n;
		if (want > BOUNCE) {
			want = BOUNCE;
		}
		const unsigned len = (want + 31) & ~31u;
		if (!di_read(s_bounce, len, a)) {
			return 0;
		}
		const unsigned got = want - skip;
		memcpy(d, s_bounce + skip, got);
		d += got;
		off += got;
		n -= got;
	}
	return 1;
}

int disc_init(void)
{
	s_bounce = (unsigned char *)memalign(32, BOUNCE);
	if (!s_bounce) {
		return 0;
	}
	/* the drive is as the boot left it, the disc read; no reset (under
	 * Nintendont there is no drive to reset) */
	unsigned char hdr[0x440];
	int ok = disc_read(hdr, sizeof(hdr), 0);
	if (!ok) {
		/* a drive that has not read the disc's ID yet (started from a
		 * loader that left it so, or Dolphin booting the DOL in the Wii's
		 * mode) fails every read until it has: read it, then again */
		ok = di_command(0xA8000040u, s_bounce, 32, 0) && disc_read(hdr, sizeof(hdr), 0);
	}
	if (!ok || be32(hdr + 0x1c) != 0xC2339F3Du) {
		printf("GC disc: no GameCube disc header\n");
		return 0;
	}
	const unsigned fst_off = be32(hdr + 0x424), fst_size = be32(hdr + 0x428);
	if (fst_size < 12 || fst_size > 4u * 1024u * 1024u) {
		printf("GC disc: no FST\n");
		return 0;
	}
	s_fst = (unsigned char *)malloc(fst_size + 1);
	if (!s_fst || !disc_read(s_fst, fst_size, fst_off)) {
		printf("GC disc: FST unreadable\n");
		return 0;
	}
	s_fst[fst_size] = 0;
	s_fst_n = be32(s_fst + 8);
	if (s_fst_n * 12 > fst_size) {
		return 0;
	}
	s_names = (const char *)s_fst + s_fst_n * 12;
	s_ok = 1;
	printf("GC disc: %u files\n", s_fst_n);
	return 1;
}

/* the entry for "DATA/NAME" or "NAME" (case aside), or 0 */
static unsigned lookup(const char *path)
{
	unsigned dir = 0;      /* the directory searched: its entry */
	unsigned end = s_fst_n;
	const char *p = path;
	while (*p == '/') {
		p++;
	}
	while (*p) {
		const char *slash = strchr(p, '/');
		const size_t len = slash ? (size_t)(slash - p) : strlen(p);
		unsigned found = 0;
		for (unsigned i = dir + 1; i < end;) {
			const unsigned char *e = s_fst + i * 12;
			const char *name = s_names + (be32(e) & 0xffffff);
			if (strlen(name) == len && strncasecmp(name, p, len) == 0) {
				found = i;
				break;
			}
			i = (e[0] & 1) ? be32(e + 8) : i + 1; /* past a directory's contents */
		}
		if (!found) {
			return 0;
		}
		if (!slash) {
			return (s_fst[found * 12] & 1) ? 0 : found;
		}
		if (!(s_fst[found * 12] & 1)) {
			return 0;
		}
		dir = found;
		end = be32(s_fst + found * 12 + 8);
		p = slash + 1;
	}
	return 0;
}

struct DiscFile {
	unsigned off, size, pos;
};

static ssize_t f_read(void *cookie, char *buf, size_t n)
{
	DiscFile *f = (DiscFile *)cookie;
	if (f->pos >= f->size) {
		return 0;
	}
	if (n > f->size - f->pos) {
		n = f->size - f->pos;
	}
	if (!disc_read(buf, (unsigned)n, f->off + f->pos)) {
		return -1;
	}
	f->pos += (unsigned)n;
	return (ssize_t)n;
}

static int f_seek(void *cookie, _off64_t *where, int whence)
{
	DiscFile *f = (DiscFile *)cookie;
	long long p = *where;
	if (whence == SEEK_CUR) {
		p += f->pos;
	} else if (whence == SEEK_END) {
		p += f->size;
	}
	if (p < 0) {
		return -1;
	}
	f->pos = (unsigned)p;
	*where = p;
	return 0;
}

static int f_close(void *cookie)
{
	free(cookie);
	return 0;
}

FILE *disc_fopen(const char *path)
{
	if (!s_ok || !path) {
		return 0;
	}
	const unsigned i = lookup(path);
	if (!i) {
		return 0;
	}
	DiscFile *f = (DiscFile *)malloc(sizeof(DiscFile));
	if (!f) {
		return 0;
	}
	f->off = be32(s_fst + i * 12 + 4);
	f->size = be32(s_fst + i * 12 + 8);
	f->pos = 0;
	cookie_io_functions_t io;
	memset(&io, 0, sizeof(io));
	io.read = f_read;
	io.seek = f_seek;
	io.close = f_close;
	FILE *fp = fopencookie(f, "rb", io);
	if (!fp) {
		free(f);
	}
	return fp;
}
