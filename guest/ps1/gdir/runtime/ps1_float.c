// MIT. Single-precision float for the PS1 (R3000A, no FPU): the libgcc
// soft-float entry points the compiler calls, written for speed. They take
// the place of libgcc's (the linker uses these first).
//
// IEEE 754 binary32 with round to nearest even, except that subnormal
// inputs and results are flushed to zero (signed), as the PS2's FPU does.
// Infinities and NaNs propagate; a NaN result is the default quiet NaN.

#include <stdint.h>

typedef union {
	float f;
	uint32_t u;
	int32_t i;
} FU;

#define SIGN 0x80000000u
#define EXP_MASK 0x7f800000u
#define MANT_MASK 0x007fffffu
#define QNAN 0x7fc00000u

static inline uint32_t fbits(float f)
{
	FU x;
	x.f = f;
	return x.u;
}

static inline float bitsf(uint32_t u)
{
	FU x;
	x.u = u;
	return x.f;
}

static inline int is_nan(uint32_t a)
{
	return (a & 0x7fffffffu) > EXP_MASK;
}

/* count leading zeros of a nonzero 32-bit value (the R3000 has no CLZ) */
static inline int clz32(uint32_t x)
{
	int n = 0;
	if (!(x & 0xffff0000u)) {
		n += 16;
		x <<= 16;
	}
	if (!(x & 0xff000000u)) {
		n += 8;
		x <<= 8;
	}
	if (!(x & 0xf0000000u)) {
		n += 4;
		x <<= 4;
	}
	if (!(x & 0xc0000000u)) {
		n += 2;
		x <<= 2;
	}
	if (!(x & 0x80000000u)) {
		n += 1;
	}
	return n;
}

/* sign, a biased exponent and a mantissa with its leading 1 at bit 30 and
 * guard/round/sticky below bit 7: round to nearest even and pack */
static inline uint32_t pack(uint32_t sign, int32_t e, uint32_t m)
{
	/* m: 1.xxx at bit 30; 23 mantissa bits are 29..7, bits 6..0 round */
	const uint32_t low = m & 0x7f;
	m >>= 7;
	if (low > 0x40 || (low == 0x40 && (m & 1))) {
		m++;
		if (m & 0x01000000u) { /* rounded up to 2.0 */
			m >>= 1;
			e++;
		}
	}
	if (e >= 0xff) {
		return sign | EXP_MASK;
	}
	if (e <= 0) {
		return sign; /* flush to zero */
	}
	return sign | ((uint32_t)e << 23) | (m & MANT_MASK);
}

static inline __attribute__((always_inline)) uint32_t add_bits(uint32_t a, uint32_t b)
{
	uint32_t ea = (a >> 23) & 0xff, eb = (b >> 23) & 0xff;
	if (ea == 0xff || eb == 0xff) {
		if (is_nan(a) || is_nan(b)) {
			return QNAN;
		}
		if (ea == 0xff && eb == 0xff && ((a ^ b) & SIGN)) {
			return QNAN; /* inf - inf */
		}
		return ea == 0xff ? a : b;
	}
	if (ea == 0) {
		return eb == 0 ? (a & b & SIGN) : b; /* zero (or flushed) + b */
	}
	if (eb == 0) {
		return a;
	}
	/* a the larger in magnitude */
	if ((a & 0x7fffffffu) < (b & 0x7fffffffu)) {
		const uint32_t t = a;
		a = b;
		b = t;
		const uint32_t te = ea;
		ea = eb;
		eb = te;
	}
	const uint32_t sign = a & SIGN;
	uint32_t ma = ((a & MANT_MASK) | 0x00800000u) << 6; /* leading 1 at bit 29 */
	uint32_t mb = ((b & MANT_MASK) | 0x00800000u) << 6;
	const uint32_t d = ea - eb;
	if (d) {
		if (d >= 31) {
			mb = 1; /* sticky only */
		} else {
			const uint32_t lost = mb & ((1u << d) - 1);
			mb = (mb >> d) | (lost != 0);
		}
	}
	int32_t e = (int32_t)ea;
	uint32_t m;
	if ((a ^ b) & SIGN) {
		m = ma - mb;
		if (!m) {
			return 0; /* x - x = +0 */
		}
		/* renormalise: leading 1 back to bit 29 */
		const int s = clz32(m) - 2;
		if (s > 0) {
			m <<= s;
			e -= s;
		}
	} else {
		m = ma + mb;
		if (m & 0x40000000u) { /* carried to bit 30 */
			m = (m >> 1) | (m & 1);
			e++;
		}
	}
	/* leading 1 at bit 29: pack wants it at 30 */
	return pack(sign, e, m << 1);
}

float __addsf3(float a, float b)
{
	return bitsf(add_bits(fbits(a), fbits(b)));
}

float __subsf3(float a, float b)
{
	return bitsf(add_bits(fbits(a), fbits(b) ^ SIGN));
}

float __mulsf3(float a, float b)
{
	const uint32_t ua = fbits(a), ub = fbits(b);
	const uint32_t sign = (ua ^ ub) & SIGN;
	const uint32_t ea = (ua >> 23) & 0xff, eb = (ub >> 23) & 0xff;
	if (ea == 0xff || eb == 0xff) {
		if (is_nan(ua) || is_nan(ub)) {
			return bitsf(QNAN);
		}
		if ((ea == 0xff && eb == 0) || (eb == 0xff && ea == 0)) {
			return bitsf(QNAN); /* inf * 0 */
		}
		return bitsf(sign | EXP_MASK);
	}
	if (ea == 0 || eb == 0) {
		return bitsf(sign);
	}
	const uint32_t ma = (ua & MANT_MASK) | 0x00800000u;
	const uint32_t mb = (ub & MANT_MASK) | 0x00800000u;
	/* 24 x 24 bits: a 48-bit product, leading 1 at bit 46 or 47 */
	const uint64_t p = (uint64_t)ma * mb;
	int32_t e = (int32_t)ea + (int32_t)eb - 127;
	uint32_t m;
	if (p & ((uint64_t)1 << 47)) {
		m = (uint32_t)(p >> 17) | (((uint32_t)p & 0x1ffff) != 0);
		e++;
	} else {
		m = (uint32_t)(p >> 16) | (((uint32_t)p & 0xffff) != 0);
	}
	return bitsf(pack(sign, e, m));
}

float __divsf3(float a, float b)
{
	const uint32_t ua = fbits(a), ub = fbits(b);
	const uint32_t sign = (ua ^ ub) & SIGN;
	const uint32_t ea = (ua >> 23) & 0xff, eb = (ub >> 23) & 0xff;
	if (ea == 0xff || eb == 0xff) {
		if (is_nan(ua) || is_nan(ub) || (ea == 0xff && eb == 0xff)) {
			return bitsf(QNAN);
		}
		return bitsf(ea == 0xff ? (sign | EXP_MASK) : sign);
	}
	if (eb == 0) {
		return bitsf(ea == 0 ? QNAN : (sign | EXP_MASK));
	}
	if (ea == 0) {
		return bitsf(sign);
	}
	const uint32_t ma = (ua & MANT_MASK) | 0x00800000u;
	const uint32_t mb = (ub & MANT_MASK) | 0x00800000u;
	int32_t e = (int32_t)ea - (int32_t)eb + 127;
	/* q = ma << 31 / mb (31 or 32 significant bits), by long division in
	 * 8-bit steps with the R3000's 32-bit divide (a 64-bit one is a libgcc
	 * loop): the remainder stays under mb < 2^24, so r << 8 fits */
	uint32_t n = ma << 8;
	uint32_t q = n / mb;
	uint32_t r = n % mb;
	n = r << 8;
	q = (q << 8) | (n / mb);
	r = n % mb;
	n = r << 8;
	q = (q << 8) | (n / mb);
	r = n % mb;
	n = r << 7;
	q = (q << 7) | (n / mb);
	r = n % mb;
	uint32_t m;
	if (q & 0x80000000u) {
		m = (q >> 1) | (q & 1) | (r != 0);
	} else {
		m = q | (r != 0);
		e--;
	}
	return bitsf(pack(sign, e, m));
}

/* comparisons, libgcc's contract: __ltsf2/__lesf2 <0 / <=0 when a < b / a <= b
 * (and >0 if unordered), __gtsf2/__gesf2 >0 / >=0 when a > b / a >= b (and <0
 * if unordered), __eqsf2/__nesf2 0 when equal (nonzero if unordered) */
static inline int cmp_bits(uint32_t a, uint32_t b)
{
	/* zeros (and flushed subnormals) compare equal whatever their sign */
	if (!(a & EXP_MASK)) {
		a = 0;
	}
	if (!(b & EXP_MASK)) {
		b = 0;
	}
	/* sign-magnitude to a two's complement order */
	const int32_t sa = (a & SIGN) ? -(int32_t)(a & 0x7fffffffu) : (int32_t)a;
	const int32_t sb = (b & SIGN) ? -(int32_t)(b & 0x7fffffffu) : (int32_t)b;
	return (sa > sb) - (sa < sb);
}

int __ltsf2(float a, float b)
{
	const uint32_t ua = fbits(a), ub = fbits(b);
	if (is_nan(ua) || is_nan(ub)) {
		return 1;
	}
	return cmp_bits(ua, ub);
}

int __lesf2(float a, float b)
{
	const uint32_t ua = fbits(a), ub = fbits(b);
	if (is_nan(ua) || is_nan(ub)) {
		return 1;
	}
	return cmp_bits(ua, ub);
}

int __gtsf2(float a, float b)
{
	const uint32_t ua = fbits(a), ub = fbits(b);
	if (is_nan(ua) || is_nan(ub)) {
		return -1;
	}
	return cmp_bits(ua, ub);
}

int __gesf2(float a, float b)
{
	const uint32_t ua = fbits(a), ub = fbits(b);
	if (is_nan(ua) || is_nan(ub)) {
		return -1;
	}
	return cmp_bits(ua, ub);
}

int __eqsf2(float a, float b)
{
	const uint32_t ua = fbits(a), ub = fbits(b);
	if (is_nan(ua) || is_nan(ub)) {
		return 1;
	}
	return cmp_bits(ua, ub) != 0;
}

int __nesf2(float a, float b)
{
	return __eqsf2(a, b);
}

int __unordsf2(float a, float b)
{
	return is_nan(fbits(a)) || is_nan(fbits(b));
}

int __fixsfsi(float a)
{
	const uint32_t u = fbits(a);
	const int32_t e = (int32_t)((u >> 23) & 0xff) - 127;
	if (e < 0) {
		return 0;
	}
	if (e >= 31) {
		if (is_nan(u)) {
			return 0;
		}
		return (u & SIGN) ? (int32_t)0x80000000 : 0x7fffffff;
	}
	const uint32_t m = (u & MANT_MASK) | 0x00800000u;
	const uint32_t v = e >= 23 ? m << (e - 23) : m >> (23 - e);
	return (u & SIGN) ? -(int32_t)v : (int32_t)v;
}

unsigned __fixunssfsi(float a)
{
	const uint32_t u = fbits(a);
	if (u & SIGN) {
		return 0;
	}
	const int32_t e = (int32_t)((u >> 23) & 0xff) - 127;
	if (e < 0) {
		return 0;
	}
	if (e >= 32) {
		return is_nan(u) ? 0 : 0xffffffffu;
	}
	const uint32_t m = (u & MANT_MASK) | 0x00800000u;
	return e >= 23 ? m << (e - 23) : m >> (23 - e);
}

static float from_u32(uint32_t v, uint32_t sign)
{
	if (!v) {
		return bitsf(0);
	}
	const int z = clz32(v);
	/* leading 1 to bit 31, then down to bit 30 with a sticky bit */
	const uint32_t m = v << z;
	return bitsf(pack(sign, 158 - z, (m >> 1) | (m & 1)));
}

float __floatsisf(int a)
{
	if (a < 0) {
		return from_u32(0u - (uint32_t)a, SIGN);
	}
	return from_u32((uint32_t)a, 0);
}

float __floatunsisf(unsigned a)
{
	return from_u32(a, 0);
}

float __negsf2(float a)
{
	return bitsf(fbits(a) ^ SIGN);
}

/* sqrtf, correctly rounded (subnormals flushed): the root's 24 bits one at
 * a time from the mantissa, the classic restoring square root */
float sqrtf(float x)
{
	const uint32_t u = fbits(x);
	int32_t e = (int32_t)((u >> 23) & 0xff);
	if (e == 0) {
		return bitsf(u & SIGN); /* +-0 (or flushed) */
	}
	if (u & SIGN) {
		return bitsf(QNAN);
	}
	if (e == 0xff) {
		return x; /* inf or NaN */
	}
	uint32_t m = (u & MANT_MASK) | 0x00800000u;
	e -= 127;
	if (e & 1) { /* an even exponent: the odd bit goes into the mantissa */
		m <<= 1;
	}
	e >>= 1; /* floor */
	/* m in [2^23, 2^25): root bits from 2^24 down, 25 of them (one to round) */
	m <<= 1;
	uint32_t q = 0, s = 0, bit = 0x01000000u;
	while (bit) {
		const uint32_t t = s + bit;
		if (t <= m) {
			s = t + bit;
			m -= t;
			q += bit;
		}
		m <<= 1;
		bit >>= 1;
	}
	/* q: 25 bits, the lowest the round bit; m != 0: inexact (sticky) */
	if (m) {
		q += q & 1;
	}
	return bitsf(((uint32_t)(e + 126) << 23) + (q >> 1));
}
