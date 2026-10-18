// MIT. The bits of a C library the shared Blazium runtime needs and
// PSn00bSDK leaves out: read-only FILE over the CD-ROM, snprintf with floats,
// atof/atoi, and single-precision maths (the PS1 has no FPU).

#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include "shim/stdio.h"
#include "shim/stdlib.h"
#include "shim/math.h"

#include <psxcd.h>

#undef printf
#undef snprintf
#undef sprintf
#undef vsnprintf

/* ------------------------------------------------------------ files */
struct P1File {
	uint32_t lba;
	uint32_t size;
	uint32_t pos;
	int32_t cached; /* sector index in buf, or -1 */
	uint32_t buf[512];
};

static int read_sectors(uint32_t lba, int n, void *dst)
{
	CdlLOC loc;
	for (int attempt = 0; attempt < 3; attempt++) {
		CdIntToPos((int)lba, &loc);
		if (!CdControl(CdlSetloc, &loc, 0)) {
			continue;
		}
		if (!CdRead(n, (uint32_t *)dst, CdlModeSpeed)) {
			continue;
		}
		if (CdReadSync(0, 0) >= 0) {
			return 1;
		}
	}
	return 0;
}

/* The disc's file list, read once: the root, DATA and DATA's folders (each
 * pack's). Looking a name up in memory saves the seeks CdSearchFile makes
 * for every miss, and the runtime tries several names for every file. */
struct DirEntry {
	char path[28];
	uint32_t lba, size;
};
#define DIR_MAX 256
static struct DirEntry s_dir[DIR_MAX];
static int s_dir_n = -1;

static void dir_scan(const char *path, int depth)
{
	CdlDIR *d = CdOpenDir(path);
	if (!d) {
		return;
	}
	CdlFILE f;
	char subs[24][12];
	int nsub = 0;
	while (CdReadDir(d, &f) && s_dir_n < DIR_MAX) {
		if (!f.name[0] || f.name[0] == '.' || (unsigned char)f.name[0] < 32) {
			continue;
		}
		if (!strchr(f.name, ';')) {
			/* a folder */
			if (depth < 2 && nsub < 24 && strlen(f.name) < sizeof(subs[0])) {
				strcpy(subs[nsub++], f.name);
			}
			continue;
		}
		struct DirEntry *e = &s_dir[s_dir_n];
		const size_t pl = strlen(path);
		if (pl + 1 + strlen(f.name) + 1 > sizeof(e->path)) {
			continue;
		}
		strcpy(e->path, path);
		if (pl > 1) {
			strcat(e->path, "\\");
		}
		strcat(e->path, f.name);
		e->lba = (uint32_t)CdPosToInt(&f.pos);
		e->size = (uint32_t)f.size;
		s_dir_n++;
	}
	CdCloseDir(d);
	for (int i = 0; i < nsub; i++) {
		char sub[32];
		if (strlen(path) + strlen(subs[i]) + 2 > sizeof(sub)) {
			continue;
		}
		strcpy(sub, path);
		if (strlen(path) > 1) {
			strcat(sub, "\\");
		}
		strcat(sub, subs[i]);
		dir_scan(sub, depth + 1);
	}
}

static int dir_lookup(const char *iso, uint32_t *lba, uint32_t *size)
{
	if (s_dir_n < 0) {
		s_dir_n = 0;
		dir_scan("\\", 0);
	}
	for (int i = 0; i < s_dir_n; i++) {
		if (strcmp(s_dir[i].path, iso) == 0) {
			*lba = s_dir[i].lba;
			*size = s_dir[i].size;
			return 1;
		}
	}
	/* not in the list (a deeper folder): ask the drive */
	CdlFILE cf;
	if (s_dir_n == 0 && CdSearchFile(&cf, iso)) {
		*lba = (uint32_t)CdPosToInt(&cf.pos);
		*size = (uint32_t)cf.size;
		return 1;
	}
	return 0;
}

/* "cdrom0:\DATA\P04\MESH04.BIN;1" (or "cdrom:", or no device) -> the ISO
 * path CdSearchFile wants. host: is the PS2 dev host: not on a PS1. */
FILE *p1_fopen(const char *path, const char *mode)
{
	if (!path || (mode && (mode[0] == 'w' || mode[0] == 'a'))) {
		return 0;
	}
	if (strncmp(path, "host:", 5) == 0 || strncmp(path, "mc0:", 4) == 0) {
		return 0;
	}
	const char *p = path;
	if (strncmp(p, "cdrom0:", 7) == 0) {
		p += 7;
	} else if (strncmp(p, "cdrom:", 6) == 0) {
		p += 6;
	}
	char iso[96];
	unsigned n = 0;
	if (*p != '\\' && *p != '/') {
		iso[n++] = '\\';
	}
	for (; *p && n + 3 < sizeof(iso); p++) {
		char c = *p == '/' ? '\\' : *p;
		if (c >= 'a' && c <= 'z') {
			c = (char)(c - 'a' + 'A');
		}
		iso[n++] = c;
	}
	iso[n] = 0;
	if (!strchr(iso, ';')) {
		iso[n++] = ';';
		iso[n++] = '1';
		iso[n] = 0;
	}
	uint32_t lba = 0, size = 0;
	if (!dir_lookup(iso, &lba, &size)) {
		return 0;
	}
	FILE *f = (FILE *)malloc(sizeof(FILE));
	if (!f) {
		return 0;
	}
	f->lba = lba;
	f->size = size;
	f->pos = 0;
	f->cached = -1;
	return f;
}

size_t p1_fread(void *buf, size_t size, size_t count, FILE *f)
{
	if (!f || !buf || size == 0) {
		return 0;
	}
	uint32_t want = (uint32_t)(size * count);
	if (f->pos >= f->size) {
		return 0;
	}
	if (want > f->size - f->pos) {
		want = f->size - f->pos;
	}
	uint8_t *dst = (uint8_t *)buf;
	uint32_t left = want;
	while (left > 0) {
		const uint32_t sec = f->pos / 2048;
		const uint32_t in = f->pos % 2048;
		/* whole sectors straight into the caller's buffer */
		if (in == 0 && left >= 2048 && ((uintptr_t)dst & 3) == 0) {
			const int n = (int)(left / 2048);
			if (!read_sectors(f->lba + sec, n, dst)) {
				break;
			}
			dst += (uint32_t)n * 2048;
			f->pos += (uint32_t)n * 2048;
			left -= (uint32_t)n * 2048;
			continue;
		}
		if (f->cached != (int32_t)sec) {
			if (!read_sectors(f->lba + sec, 1, f->buf)) {
				break;
			}
			f->cached = (int32_t)sec;
		}
		uint32_t take = 2048 - in;
		if (take > left) {
			take = left;
		}
		memcpy(dst, (const uint8_t *)f->buf + in, take);
		dst += take;
		f->pos += take;
		left -= take;
	}
	return (size_t)((want - left) / size);
}

size_t p1_fwrite(const void *buf, size_t size, size_t n, FILE *f)
{
	(void)buf;
	(void)size;
	(void)n;
	(void)f;
	return 0;
}

int p1_fseek(FILE *f, long off, int whence)
{
	if (!f) {
		return -1;
	}
	long base = 0;
	if (whence == SEEK_CUR) {
		base = (long)f->pos;
	} else if (whence == SEEK_END) {
		base = (long)f->size;
	}
	const long to = base + off;
	if (to < 0) {
		return -1;
	}
	f->pos = (uint32_t)to;
	return 0;
}

void p1_rewind(FILE *f)
{
	p1_fseek(f, 0, SEEK_SET);
}

long p1_ftell(FILE *f)
{
	return f ? (long)f->pos : -1;
}

int p1_fclose(FILE *f)
{
	free(f);
	return 0;
}

/* ------------------------------------------------------------ printf */
struct Out {
	char *o;
	size_t n, at;
};

static void put(struct Out *w, char c)
{
	if (w->at + 1 < w->n) {
		w->o[w->at] = c;
	}
	w->at++;
}

static void put_padded(struct Out *w, const char *s, int len, int width, int left, char padc)
{
	int pad = width > len ? width - len : 0;
	if (!left) {
		/* zero padding goes after a sign */
		if (padc == '0' && len > 0 && (s[0] == '-' || s[0] == '+')) {
			put(w, s[0]);
			s++;
			len--;
		}
		while (pad-- > 0) {
			put(w, padc);
		}
	}
	for (int i = 0; i < len; i++) {
		put(w, s[i]);
	}
	if (left) {
		while (pad-- > 0) {
			put(w, ' ');
		}
	}
}

static int fmt_uint(char *b, unsigned long long v, int base, int upper)
{
	char t[24];
	int n = 0;
	do {
		const int d = (int)(v % (unsigned)base);
		t[n++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
		v /= (unsigned)base;
	} while (v);
	for (int i = 0; i < n; i++) {
		b[i] = t[n - 1 - i];
	}
	return n;
}

/* fixed-point text for |v| with prec decimals (prec <= 9) */
static int fmt_fixed(char *b, double v, int prec, int sign_plus, int sign_space)
{
	int n = 0;
	if (v != v) {
		memcpy(b, "nan", 3);
		return 3;
	}
	if (v < 0) {
		b[n++] = '-';
		v = -v;
	} else if (sign_plus) {
		b[n++] = '+';
	} else if (sign_space) {
		b[n++] = ' ';
	}
	if (v > 1.0e18) {
		memcpy(b + n, "inf", 3);
		return n + 3;
	}
	if (prec > 9) {
		prec = 9;
	}
	unsigned long long scale = 1;
	for (int i = 0; i < prec; i++) {
		scale *= 10;
	}
	unsigned long long ip = (unsigned long long)v;
	double frac = v - (double)ip;
	unsigned long long fp = (unsigned long long)(frac * (double)scale + 0.5);
	if (fp >= scale) {
		fp -= scale;
		ip++;
	}
	n += fmt_uint(b + n, ip, 10, 0);
	if (prec > 0) {
		b[n++] = '.';
		char t[12];
		const int k = fmt_uint(t, fp, 10, 0);
		for (int i = k; i < prec; i++) {
			b[n++] = '0';
		}
		memcpy(b + n, t, (size_t)k);
		n += k;
	}
	return n;
}

int p1_vsnprintf(char *o, size_t n, const char *fmt, va_list ap)
{
	struct Out w = { o, n, 0 };
	for (const char *p = fmt; *p; p++) {
		if (*p != '%') {
			put(&w, *p);
			continue;
		}
		p++;
		int left = 0, plus = 0, space = 0, alt = 0;
		char padc = ' ';
		for (;; p++) {
			if (*p == '-') {
				left = 1;
			} else if (*p == '0') {
				padc = '0';
			} else if (*p == '+') {
				plus = 1;
			} else if (*p == ' ') {
				space = 1;
			} else if (*p == '#') {
				alt = 1;
			} else {
				break;
			}
		}
		(void)alt;
		int width = 0;
		if (*p == '*') {
			width = va_arg(ap, int);
			p++;
		}
		while (*p >= '0' && *p <= '9') {
			width = width * 10 + (*p++ - '0');
		}
		int prec = -1;
		if (*p == '.') {
			p++;
			prec = 0;
			if (*p == '*') {
				prec = va_arg(ap, int);
				p++;
			}
			while (*p >= '0' && *p <= '9') {
				prec = prec * 10 + (*p++ - '0');
			}
		}
		int lng = 0;
		while (*p == 'l' || *p == 'h' || *p == 'z') {
			if (*p == 'l') {
				lng++;
			}
			p++;
		}
		char b[48];
		int len = 0;
		switch (*p) {
			case 'd':
			case 'i': {
				long long v = lng >= 2 ? va_arg(ap, long long) : (lng ? (long long)va_arg(ap, long) : (long long)va_arg(ap, int));
				if (v < 0) {
					b[len++] = '-';
					len += fmt_uint(b + len, (unsigned long long)(-v), 10, 0);
				} else {
					if (plus) {
						b[len++] = '+';
					} else if (space) {
						b[len++] = ' ';
					}
					len += fmt_uint(b + len, (unsigned long long)v, 10, 0);
				}
				put_padded(&w, b, len, width, left, padc);
			} break;
			case 'u':
			case 'x':
			case 'X':
			case 'p': {
				unsigned long long v;
				if (*p == 'p') {
					v = (uintptr_t)va_arg(ap, void *);
				} else {
					v = lng >= 2 ? va_arg(ap, unsigned long long) : (lng ? (unsigned long long)va_arg(ap, unsigned long) : (unsigned long long)va_arg(ap, unsigned));
				}
				len = fmt_uint(b, v, *p == 'u' ? 10 : 16, *p == 'X');
				put_padded(&w, b, len, width, left, padc);
			} break;
			case 'c':
				b[0] = (char)va_arg(ap, int);
				put_padded(&w, b, 1, width, left, ' ');
				break;
			case 's': {
				const char *s = va_arg(ap, const char *);
				if (!s) {
					s = "(null)";
				}
				int sl = (int)strlen(s);
				if (prec >= 0 && sl > prec) {
					sl = prec;
				}
				put_padded(&w, s, sl, width, left, ' ');
			} break;
			case 'f':
			case 'F':
				len = fmt_fixed(b, va_arg(ap, double), prec < 0 ? 6 : prec, plus, space);
				put_padded(&w, b, len, width, left, padc);
				break;
			case 'g':
			case 'G':
			case 'e':
			case 'E': {
				/* %g as GDScript's float printing needs it: up to prec
				 * significant digits, trailing zeros dropped */
				double v = va_arg(ap, double);
				int sig = prec < 0 ? 6 : (prec == 0 ? 1 : prec);
				double a = v < 0 ? -v : v;
				int ex = 0;
				if (a >= 1.0) {
					while (a >= 10.0 && ex < 30) {
						a /= 10.0;
						ex++;
					}
				} else if (a > 0.0) {
					while (a < 1.0 && ex > -30) {
						a *= 10.0;
						ex--;
					}
				}
				int dec = sig - 1 - ex;
				if (dec < 0) {
					dec = 0;
				}
				len = fmt_fixed(b, v, dec, plus, space);
				if (memchr(b, '.', (size_t)len)) {
					while (len > 0 && b[len - 1] == '0') {
						len--;
					}
					if (len > 0 && b[len - 1] == '.') {
						len--;
					}
				}
				put_padded(&w, b, len, width, left, padc);
			} break;
			case '%':
				put(&w, '%');
				break;
			case 0:
				p--;
				break;
			default:
				put(&w, '%');
				put(&w, *p);
				break;
		}
	}
	if (n > 0) {
		o[w.at < n ? w.at : n - 1] = 0;
	}
	return (int)w.at;
}

int p1_snprintf(char *o, size_t n, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	const int r = p1_vsnprintf(o, n, fmt, ap);
	va_end(ap);
	return r;
}

int p1_sprintf(char *o, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	const int r = p1_vsnprintf(o, 512, fmt, ap);
	va_end(ap);
	return r;
}

int p1_printf(const char *fmt, ...)
{
	char b[256];
	va_list ap;
	va_start(ap, fmt);
	const int r = p1_vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	printf("%s", b);
	return r;
}

/* ------------------------------------------------------------ numbers */
double atof(const char *s)
{
	if (!s) {
		return 0.0;
	}
	while (*s == ' ' || *s == '\t') {
		s++;
	}
	int neg = 0;
	if (*s == '-' || *s == '+') {
		neg = *s == '-';
		s++;
	}
	double v = 0.0;
	while (*s >= '0' && *s <= '9') {
		v = v * 10.0 + (double)(*s++ - '0');
	}
	if (*s == '.') {
		s++;
		double f = 0.1;
		while (*s >= '0' && *s <= '9') {
			v += f * (double)(*s++ - '0');
			f *= 0.1;
		}
	}
	if (*s == 'e' || *s == 'E') {
		s++;
		int en = 0, eneg = 0;
		if (*s == '-' || *s == '+') {
			eneg = *s == '-';
			s++;
		}
		while (*s >= '0' && *s <= '9') {
			en = en * 10 + (*s++ - '0');
		}
		while (en-- > 0) {
			v = eneg ? v / 10.0 : v * 10.0;
		}
	}
	return neg ? -v : v;
}

int atoi(const char *s)
{
	return (int)strtol(s, 0, 10);
}

/* ------------------------------------------------------------ maths */
#define P1_PI 3.14159265358979f
#define P1_TAU 6.28318530717959f
#define P1_HALF_PI 1.57079632679490f

static inline uint32_t f2u(float f)
{
	uint32_t u;
	memcpy(&u, &f, 4);
	return u;
}

static inline float u2f(uint32_t u)
{
	float f;
	memcpy(&f, &u, 4);
	return f;
}

float fabsf(float x)
{
	return u2f(f2u(x) & 0x7fffffffu);
}

float sqrtf(float x)
{
	if (x <= 0.0f) {
		return 0.0f;
	}
	float y = u2f(0x1fbd1df5u + (f2u(x) >> 1));
	y = 0.5f * (y + x / y);
	y = 0.5f * (y + x / y);
	y = 0.5f * (y + x / y);
	return y;
}

float floorf(float x)
{
	if (x >= 8388608.0f || x <= -8388608.0f) {
		return x;
	}
	const int i = (int)x;
	return (float)((x < 0.0f && (float)i != x) ? i - 1 : i);
}

float ceilf(float x)
{
	return -floorf(-x);
}

float roundf(float x)
{
	return x < 0.0f ? -floorf(-x + 0.5f) : floorf(x + 0.5f);
}

float fmodf(float x, float y)
{
	if (y == 0.0f) {
		return 0.0f;
	}
	const float q = x / y;
	const float t = q < 0.0f ? -floorf(-q) : floorf(q);
	return x - t * y;
}

/* sin on [-pi, pi]: reduce to [-pi/2, pi/2], then an odd polynomial */
float sinf(float x)
{
	if (x > P1_PI || x < -P1_PI) {
		x = fmodf(x, P1_TAU);
		if (x > P1_PI) {
			x -= P1_TAU;
		} else if (x < -P1_PI) {
			x += P1_TAU;
		}
	}
	if (x > P1_HALF_PI) {
		x = P1_PI - x;
	} else if (x < -P1_HALF_PI) {
		x = -P1_PI - x;
	}
	const float x2 = x * x;
	return x * (1.0f + x2 * (-0.16666667f + x2 * (0.0083333310f + x2 * (-0.00019840874f + x2 * 2.7525562e-6f))));
}

float cosf(float x)
{
	return sinf(x + P1_HALF_PI);
}

float tanf(float x)
{
	const float c = cosf(x);
	return c == 0.0f ? 1.0e9f : sinf(x) / c;
}

/* atan on [0, 1] (minimax), extended by atan(x) = pi/2 - atan(1/x) */
static float atan01(float x)
{
	const float x2 = x * x;
	return x * (0.99997726f + x2 * (-0.33262347f + x2 * (0.19354346f + x2 * (-0.11643287f + x2 * (0.05265332f + x2 * -0.01172120f)))));
}

float atanf(float x)
{
	const int neg = x < 0.0f;
	float a = neg ? -x : x;
	const float r = a > 1.0f ? P1_HALF_PI - atan01(1.0f / a) : atan01(a);
	return neg ? -r : r;
}

float atan2f(float y, float x)
{
	if (x == 0.0f) {
		if (y > 0.0f) {
			return P1_HALF_PI;
		}
		if (y < 0.0f) {
			return -P1_HALF_PI;
		}
		return 0.0f;
	}
	const float a = atanf(y / x);
	if (x > 0.0f) {
		return a;
	}
	return y >= 0.0f ? a + P1_PI : a - P1_PI;
}

float asinf(float x)
{
	if (x >= 1.0f) {
		return P1_HALF_PI;
	}
	if (x <= -1.0f) {
		return -P1_HALF_PI;
	}
	return atan2f(x, sqrtf(1.0f - x * x));
}

float acosf(float x)
{
	if (x >= 1.0f) {
		return 0.0f;
	}
	if (x <= -1.0f) {
		return P1_PI;
	}
	return atan2f(sqrtf(1.0f - x * x), x);
}

/* exp via 2^(x / ln2): integer part in the exponent, fraction by polynomial */
float expf(float x)
{
	if (x > 88.0f) {
		return 3.4e38f;
	}
	if (x < -87.0f) {
		return 0.0f;
	}
	const float t = x * 1.44269504f;
	const float fi = floorf(t);
	const float f = t - fi;
	const float p = 1.0f + f * (0.69314718f + f * (0.24022650f + f * (0.05550411f + f * (0.00961813f + f * 0.00133336f))));
	return p * u2f((uint32_t)((int)fi + 127) << 23);
}

float logf(float x)
{
	if (x <= 0.0f) {
		return -3.4e38f;
	}
	const uint32_t u = f2u(x);
	const int e = (int)((u >> 23) & 0xff) - 127;
	float m = u2f((u & 0x007fffffu) | 0x3f800000u); /* 1..2 */
	/* ln(m) with m in [1,2): s = (m-1)/(m+1), ln = 2(s + s^3/3 + s^5/5 + s^7/7) */
	const float s = (m - 1.0f) / (m + 1.0f);
	const float s2 = s * s;
	const float lm = 2.0f * s * (1.0f + s2 * (0.33333333f + s2 * (0.2f + s2 * (0.14285714f + s2 * 0.11111111f))));
	return lm + (float)e * 0.69314718f;
}

float powf(float x, float y)
{
	if (y == 0.0f) {
		return 1.0f;
	}
	if (x == 0.0f) {
		return 0.0f;
	}
	if (x < 0.0f) {
		/* integer powers of negatives only */
		const float yi = floorf(y);
		if (yi != y) {
			return 0.0f;
		}
		const float r = expf(y * logf(-x));
		return ((int)yi & 1) ? -r : r;
	}
	return expf(y * logf(x));
}

/* the heap as PSn00bSDK sees it, for "no memory" reports */
struct P1HeapUsage { /* PSn00bSDK's HeapUsage */
	size_t total, heap, stack, alloc, alloc_max;
};
void GetHeapUsage(struct P1HeapUsage *usage);
void p1_heap_report(void)
{
	struct P1HeapUsage hu;
	GetHeapUsage(&hu);
	printf("heap: %u in use (peak %u), heap %u, total %u\n", (unsigned)hu.alloc, (unsigned)hu.alloc_max, (unsigned)hu.heap,
			(unsigned)hu.total);
}

/* root counter 2 at the system clock / 8 (4.23 MHz), for GDIR_STATS */
unsigned p1_clock(void)
{
	static int on;
	if (!on) {
		*(volatile uint32_t *)0x1f801124 = 0x0200;
		on = 1;
	}
	return *(volatile uint32_t *)0x1f801120 & 0xffff;
}
