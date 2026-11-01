/* MIT. The GameCube disc's apploader, for the discs blazium-toolchain
 * writes. The console's boot ROM (or an emulator's, or Swiss) loads this to
 * 0x81200000 and calls its three functions: main asks for one read of the
 * disc at a time (the disc header, then the DOL's header, then each of the
 * DOL's sections), close clears the DOL's BSS, makes the caches see the new
 * code and gives back the DOL's entry point.
 *
 * Build: powerpc-eabi-gcc -O2 -mcpu=750 -meabi -mhard-float -ffreestanding
 *   -nostdlib -fno-builtin -Wl,-Ttext=0x81200000 -Wl,-e,entry apploader.c,
 *   then objcopy -O binary (scripts/gamecube-guest-prebuilt.sh). */

typedef unsigned int u32;
typedef void (*report_fn)(const char *, ...);

/* scratch for the headers, in the apploader's own megabyte (0x81200000 to
 * 0x81300000): Nintendont, which plays the disc on a Wii, runs its loader
 * (multidol, the one calling these functions) at 0x81300000, and a header
 * read there wrote over it (a black screen) */
#define HDR ((volatile unsigned char *)0x81280000)
#define DOL ((volatile unsigned char *)0x81280800)

static report_fn s_report = 0;
static u32 s_state = 0;
static u32 s_dol = 0;
static u32 s_sec = 0;

static u32 be32(volatile const unsigned char *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static void app_init(report_fn report)
{
	s_report = report;
	s_state = 0;
	s_sec = 0;
}

/* 1 with a read for the boot ROM to make, 0 when the DOL is in */
static int app_main(void **dst, int *size, int *offset)
{
	if (s_state == 0) {
		*dst = (void *)HDR;
		*size = 0x440;
		*offset = 0;
		s_state = 1;
		return 1;
	}
	if (s_state == 1) {
		s_dol = be32(HDR + 0x420);
		*dst = (void *)DOL;
		*size = 0x100;
		*offset = (int)s_dol;
		s_state = 2;
		return 1;
	}
	/* the sections: 7 text, then 11 data; offsets at 0, addresses at 0x48,
	 * sizes at 0x90 */
	while (s_sec < 18) {
		const u32 i = s_sec++;
		const u32 off = be32(DOL + i * 4);
		const u32 addr = be32(DOL + 0x48 + i * 4);
		const u32 sz = be32(DOL + 0x90 + i * 4);
		if (!sz || !addr) {
			continue;
		}
		*dst = (void *)addr;
		*size = (int)((sz + 31) & ~31u);
		*offset = (int)(s_dol + off);
		return 1;
	}
	return 0;
}

static void *app_close(void)
{
	/* the BSS */
	const u32 bss = be32(DOL + 0xd8), bss_n = be32(DOL + 0xdc);
	for (u32 a = bss; a < bss + bss_n; a++) {
		*(volatile unsigned char *)a = 0;
	}
	/* the sections in memory, not the caches */
	for (u32 i = 0; i < 18; i++) {
		const u32 addr = be32(DOL + 0x48 + i * 4), sz = be32(DOL + 0x90 + i * 4);
		for (u32 a = addr & ~31u; sz && a < addr + sz; a += 32) {
			__asm__ volatile("dcbf 0,%0; icbi 0,%0" : : "r"(a) : "memory");
		}
	}
	for (u32 a = bss & ~31u; bss_n && a < bss + bss_n; a += 32) {
		__asm__ volatile("dcbf 0,%0" : : "r"(a) : "memory");
	}
	__asm__ volatile("sync; isync");
	return (void *)be32(DOL + 0xe0);
}

void entry(void (**init)(report_fn), int (**main)(void **, int *, int *), void *(**close)(void))
{
	*init = app_init;
	*main = app_main;
	*close = app_close;
}
