// MIT. The PS2 guest's sound API (sfx_io.h) on the PS1 SPU. `ps1 iso`
// rewrites the export's SFXnn.BIN and MUSICnn.BIN (IMA ADPCM) as the SPU's
// own ADPCM ("SFXV", "MUSV": internal/platforms/ps1/sfxvag.go), which is
// only copied to SPU RAM here. An older disc's IMA ("SFXB", "SFX ", "MUS ")
// is still encoded here, one clip at a time (slower: seconds at boot).
//
// SPU RAM: 0x1000 a silent looping block (parked voices), then the bank,
// then the music (looped by its own block flags). Voice 0 is the music,
// voices 1..8 the sounds, round robin so they overlap instead of cutting.

#include "sfx_io.h"

#include "iso_path.h"

#include <hwregs_c.h>
#include <psxspu.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPU_SILENCE 0x1000 /* 16 bytes: a looping silent block */
#define SPU_BANK 0x1010
#define SPU_END 0x80000 /* SPU RAM: 512 KiB */
#define MAX_CLIPS 64
#define VOICE_MUSIC 0
#define VOICE_FIRST 1
#define VOICE_COUNT 8
#define CHUNK 4096 /* VAG bytes written to the SPU at a time (a multiple of 64) */

static const int k_ima_step[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
	19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
	50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
	130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
	337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
	876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
	2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
	5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
	15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};
static const int k_ima_index[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
/* the SPU's ADPCM prediction filters (x64) */
static const int k_f0[5] = { 0, 60, 115, 98, 122 };
static const int k_f1[5] = { 0, 0, -52, -55, -60 };

static int s_ok;
static int s_ready;
static int s_pack = -1;
static int s_clip_n;
static uint32_t s_clip_addr[MAX_CLIPS];
static int s_clip_pitch[MAX_CLIPS];
static uint32_t s_bank_end = SPU_BANK;
static int s_next;
static int s_audible;
static int s_mus_ready, s_mus_playing, s_mus_pitch;
static uint32_t s_mus_addr;
static float s_mus_vol = 1.0f, s_fade_from, s_fade_to, s_fade_left, s_fade_total;
static uint8_t s_chunk[CHUNK] __attribute__((aligned(4)));

static unsigned ru16(const uint8_t *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned ru32(const uint8_t *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static uint8_t *load_blob(const char *stem, int pack, unsigned *sz)
{
	char path[48];
	snprintf(path, sizeof(path), "cdrom0:\\%s%02d.BIN;1", stem, pack);
	FILE *f = iso_fopen(path, "rb");
	*sz = 0;
	if (!f) {
		return 0;
	}
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *b = n >= 12 ? (uint8_t *)malloc((size_t)n) : 0;
	if (b && fread(b, 1, (size_t)n, f) == (size_t)n) {
		*sz = (unsigned)n;
	} else {
		free(b);
		b = 0;
	}
	fclose(f);
	return b;
}

/* ---- IMA ADPCM in, SPU ADPCM out, streamed to SPU RAM ---- */

struct Ima {
	const uint8_t *p;
	unsigned n, i; /* bytes, nibbles read */
	int pred, idx;
};

static int ima_next(Ima *s)
{
	if (s->i >= s->n * 2) {
		return 0;
	}
	const unsigned b = s->p[s->i >> 1];
	const unsigned nib = (s->i & 1) ? (b >> 4) : (b & 15);
	s->i++;
	const int step = k_ima_step[s->idx];
	int d = step >> 3;
	if (nib & 4) {
		d += step;
	}
	if (nib & 2) {
		d += step >> 1;
	}
	if (nib & 1) {
		d += step >> 2;
	}
	s->pred += (nib & 8) ? -d : d;
	s->pred = s->pred > 32767 ? 32767 : (s->pred < -32768 ? -32768 : s->pred);
	s->idx += k_ima_index[nib];
	s->idx = s->idx < 0 ? 0 : (s->idx > 88 ? 88 : s->idx);
	return s->pred;
}

/* one 28-sample block with filter f and shift sh from history h1, h2:
 * the squared error, and (out) the nibbles and the new history */
static uint32_t vag_try(const int *x, int f, int sh, int h1, int h2, uint8_t *nib, int *o1, int *o2)
{
	uint32_t err = 0; /* sum of squared errors / 256: fits 32 bits (no libgcc 64-bit multiply) */
	for (int i = 0; i < 28; i++) {
		const int pred = (h1 * k_f0[f] + h2 * k_f1[f] + 32) >> 6;
		int r = x[i] - pred;
		/* nearest 4-bit step at this shift */
		int q = (r + ((1 << (12 - sh)) >> 1)) >> (12 - sh); /* the nearest step (floor of r/step + 1/2) */
		q = q > 7 ? 7 : (q < -8 ? -8 : q);
		int y = (q << (12 - sh)) + pred;
		y = y > 32767 ? 32767 : (y < -32768 ? -32768 : y);
		const uint32_t e = (uint32_t)(x[i] > y ? x[i] - y : y - x[i]);
		err += (e * e) >> 8;
		nib[i] = (uint8_t)(q & 15);
		h2 = h1;
		h1 = y;
	}
	*o1 = h1;
	*o2 = h2;
	return err;
}

/* A clip (IMA bytes) to SPU RAM at addr as VAG: the bytes written (a
 * multiple of 16), or 0. loop: the last block loops to the first (music);
 * otherwise a silent end block (the voice parks on its loop at the end). */
static uint32_t upload_clip(const uint8_t *ima, unsigned nbytes, uint32_t addr, int loop)
{
	const unsigned samples = nbytes * 2;
	const unsigned blocks = (samples + 27) / 28 + (loop ? 0 : 1);
	const uint32_t size = blocks * 16;
	if (!samples || addr + size > SPU_END) {
		return 0;
	}
	Ima s = { ima, nbytes, 0, 0, 0 };
	int h1 = 0, h2 = 0;
	unsigned fill = 0;
	uint32_t at = addr;
	for (unsigned b = 0; b < blocks; b++) {
		uint8_t *blk = s_chunk + fill;
		memset(blk, 0, 16);
		const int data_block = loop || b + 1 < blocks;
		if (data_block) {
			int x[28];
			for (int i = 0; i < 28; i++) {
				x[i] = ima_next(&s);
			}
			/* the filter and shift that fit best (shift: the smallest step
			 * that holds the block's peak residual, and its neighbours) */
			uint32_t best = 0xffffffffu;
			uint8_t nib[28], bn[28];
			int bf = 0, bs = 0, b1 = 0, b2 = 0;
			for (int f = 0; f < 5; f++) {
				int peak = 0, t1 = h1, t2 = h2;
				for (int i = 0; i < 28; i++) {
					const int pred = (t1 * k_f0[f] + t2 * k_f1[f] + 32) >> 6;
					const int r = x[i] - pred;
					peak = (r < 0 ? -r : r) > peak ? (r < 0 ? -r : r) : peak;
					t2 = t1;
					t1 = x[i];
				}
				int sh = 12;
				while (sh > 0 && (peak >> (12 - sh)) > 7) {
					sh--;
				}
				for (int k = -1; k < 2; k++) {
					const int shk = sh - k;
					if (shk < 0 || shk > 12) {
						continue;
					}
					int o1, o2;
					const uint32_t e = vag_try(x, f, shk, h1, h2, nib, &o1, &o2);
					if (e < best) {
						best = e;
						bf = f;
						bs = shk;
						b1 = o1;
						b2 = o2;
						memcpy(bn, nib, 28);
					}
				}
			}
			h1 = b1;
			h2 = b2;
			blk[0] = (uint8_t)((bf << 4) | bs);
			for (int i = 0; i < 14; i++) {
				blk[2 + i] = (uint8_t)(bn[i * 2] | (bn[i * 2 + 1] << 4));
			}
		}
		/* flags: loop start 4, loop (repeat) 2, loop end 1 */
		if (loop) {
			blk[1] = (uint8_t)((b == 0 ? 4 : 0) | (b + 1 == blocks ? 3 : 0));
		} else if (b + 1 == blocks) {
			blk[1] = 7; /* the silent end block loops on itself */
		}
		fill += 16;
		if (fill == CHUNK || b + 1 == blocks) {
			const unsigned n = (fill + 63) & ~63u;
			memset(s_chunk + fill, 0, n - fill);
			SpuSetTransferStartAddr(at);
			SpuWrite((const uint32_t *)s_chunk, n);
			SpuIsTransferCompleted(SPU_TRANSFER_WAIT);
			at += fill;
			fill = 0;
		}
	}
	return size;
}

/* VAG blocks as they are to SPU RAM at addr, through s_chunk (SpuWrite
 * wants word-aligned data in multiples of 64) */
static uint32_t upload_vag(const uint8_t *vag, unsigned n, uint32_t addr)
{
	n &= ~15u;
	if (!n || addr + n > SPU_END) {
		return 0;
	}
	for (unsigned done = 0; done < n;) {
		const unsigned part = n - done < CHUNK ? n - done : CHUNK;
		memcpy(s_chunk, vag + done, part);
		const unsigned w = (part + 63) & ~63u;
		memset(s_chunk + part, 0, w - part);
		SpuSetTransferStartAddr(addr + done);
		SpuWrite((const uint32_t *)s_chunk, w);
		SpuIsTransferCompleted(SPU_TRANSFER_WAIT);
		done += part;
	}
	return n;
}

static int pitch_of(int rate)
{
	return rate > 0 ? (rate * 4096) / 44100 : 4096;
}

static void voice_start(int v, uint32_t addr, int pitch, int vol)
{
	SPU_KEY_OFF1 = (uint16_t)(1 << v);
	SPU_CH_VOL_L(v) = (uint16_t)vol;
	SPU_CH_VOL_R(v) = (uint16_t)vol;
	SPU_CH_FREQ(v) = (uint16_t)(pitch > 0x3fff ? 0x3fff : pitch);
	SPU_CH_ADDR(v) = (uint16_t)getSPUAddr(addr);
	/* instant attack, hold at full, a short release */
	SPU_CH_ADSR1(v) = 0x000f;
	SPU_CH_ADSR2(v) = 0x1fc0 | 0x0a;
	SPU_KEY_ON1 = (uint16_t)(1 << v);
}

int sfx_io_init(void)
{
	SpuInit();
	/* a silent looping block for voices with nothing to play */
	memset(s_chunk, 0, 64);
	s_chunk[1] = 7;
	SpuSetTransferStartAddr(SPU_SILENCE);
	SpuWrite((const uint32_t *)s_chunk, 64);
	SpuIsTransferCompleted(SPU_TRANSFER_WAIT);
	for (int v = 0; v < 24; v++) {
		SPU_CH_VOL_L(v) = 0;
		SPU_CH_VOL_R(v) = 0;
		SPU_CH_ADDR(v) = (uint16_t)getSPUAddr(SPU_SILENCE);
	}
	SpuSetCommonMasterVolume(0x3fff, 0x3fff);
	s_ok = 1;
	const int ok = sfx_io_load(0);
	(void)sfx_io_music_load(0);
	return ok;
}

int sfx_io_load(int pack)
{
	if (!s_ok) {
		return 0;
	}
	if (pack < 0) {
		pack = 0;
	}
	if (pack == s_pack && s_ready) {
		return 1;
	}
	unsigned sz = 0;
	uint8_t *b = load_blob("SFX", pack, &sz);
	if (!b) {
		/* a pack without its own bank keeps pack 0's */
		if (s_pack == 0 && s_ready) {
			return 1;
		}
		pack = 0;
		b = load_blob("SFX", 0, &sz);
	}
	if (!b) {
		return 0;
	}
	if (sz < 8 || memcmp(b, "SFX", 3) != 0 || ru16(b + 4) != 1) {
		free(b);
		return 0;
	}
	if (b[3] == 'V') {
		sfx_io_stop();
		s_clip_n = 0;
		uint32_t at = SPU_BANK;
		const unsigned count = ru16(b + 6);
		unsigned off = 8;
		for (unsigned i = 0; i < count && s_clip_n < MAX_CLIPS && off + 6 <= sz; i++) {
			const int rate = (int)ru16(b + off);
			const unsigned nb = ru32(b + off + 2);
			off += 6;
			if (off + nb > sz) {
				break;
			}
			const uint32_t w = upload_vag(b + off, nb, at);
			s_clip_addr[s_clip_n] = w ? at : 0;
			s_clip_pitch[s_clip_n] = pitch_of(rate);
			s_clip_n++;
			at += w;
			off += nb;
		}
		free(b);
		s_bank_end = at;
		s_pack = pack;
		s_ready = s_clip_n > 0;
		printf("P1 sound: pack %d, %d clips, %u bytes of SPU RAM\n", pack, s_clip_n, (unsigned)(at - SPU_BANK));
		if (s_mus_ready && s_mus_addr < s_bank_end) {
			s_mus_ready = 0;
			sfx_io_music_load(pack);
		}
		return s_ready;
	}
	/* an older disc: IMA ADPCM, encoded here */
	sfx_io_stop();
	s_clip_n = 0;
	s_ready = 0;
	uint32_t at = SPU_BANK;
	if (b[3] == ' ' && sz >= 12) {
		const unsigned nb = ru32(b + 8);
		if (12 + nb <= sz) {
			const uint32_t w = upload_clip(b + 12, nb, at, 0);
			s_clip_addr[0] = w ? at : 0;
			s_clip_pitch[0] = pitch_of((int)ru16(b + 6));
			s_clip_n = 1;
			at += w;
		}
	} else if (b[3] == 'B') {
		const unsigned count = ru16(b + 6);
		unsigned off = 8;
		for (unsigned i = 0; i < count && s_clip_n < MAX_CLIPS && off + 6 <= sz; i++) {
			const int rate = (int)ru16(b + off);
			const unsigned nb = ru32(b + off + 2);
			off += 6;
			if (off + nb > sz) {
				break;
			}
			/* an empty or failed clip keeps its index (silent) */
			const uint32_t w = upload_clip(b + off, nb, at, 0);
			s_clip_addr[s_clip_n] = w ? at : 0;
			s_clip_pitch[s_clip_n] = pitch_of(rate);
			s_clip_n++;
			at += w;
			off += nb;
		}
	}
	free(b);
	s_bank_end = at;
	s_pack = pack;
	s_ready = s_clip_n > 0;
	printf("P1 sound: pack %d, %d clips, %u bytes of SPU RAM\n", pack, s_clip_n, (unsigned)(at - SPU_BANK));
	/* the music sat after the old bank: load it again after this one */
	if (s_mus_ready && s_mus_addr < s_bank_end) {
		s_mus_ready = 0;
		sfx_io_music_load(pack);
	}
	return s_ready;
}

void sfx_io_unload(void)
{
	/* the bank stays in SPU RAM until another replaces it */
	sfx_io_stop();
}

void sfx_io_play(void)
{
	sfx_io_play_clip(0);
}

void sfx_io_play_clip(int clip)
{
	if (!s_ready || clip < 0 || clip >= s_clip_n || !s_clip_addr[clip]) {
		return;
	}
	const int v = VOICE_FIRST + s_next;
	s_next = (s_next + 1) % VOICE_COUNT;
	voice_start(v, s_clip_addr[clip], s_clip_pitch[clip], 0x3fff);
	s_audible = 1;
}

int sfx_io_clip_count(void)
{
	return s_clip_n;
}

void sfx_io_stop(void)
{
	if (s_ok) {
		SPU_KEY_OFF1 = (uint16_t)(((1 << VOICE_COUNT) - 1) << VOICE_FIRST);
	}
}

int sfx_io_ready(void)
{
	return s_ready;
}

int sfx_io_audible(void)
{
	return s_audible;
}

/* ---- music: one looping clip on voice 0 ---- */

int sfx_io_music_loaded(void)
{
	return s_mus_ready;
}

int sfx_io_music_load(int pack)
{
	if (!s_ok) {
		return 0;
	}
	if (pack < 0) {
		pack = 0;
	}
	unsigned sz = 0;
	uint8_t *b = load_blob("MUSIC", pack, &sz);
	if (!b && pack != 0) {
		b = load_blob("MUSIC", 0, &sz);
	}
	if (!b) {
		return 0;
	}
	int ok = 0;
	if (sz >= 12 && memcmp(b, "MUSV", 4) == 0 && ru16(b + 4) == 1) {
		const unsigned nb = ru32(b + 8);
		if (12 + nb <= sz) {
			const int was = s_mus_playing;
			sfx_io_music_stop();
			s_mus_addr = (s_bank_end + 15) & ~15u;
			ok = upload_vag(b + 12, nb, s_mus_addr) != 0;
			s_mus_pitch = pitch_of((int)ru16(b + 6));
			s_mus_ready = ok;
			if (ok && was) {
				sfx_io_music_play();
			}
		}
	} else if (sz >= 12 && memcmp(b, "MUS ", 4) == 0 && ru16(b + 4) == 1) {
		const unsigned nb = ru32(b + 8);
		if (12 + nb <= sz) {
			const int was = s_mus_playing;
			sfx_io_music_stop();
			s_mus_addr = (s_bank_end + 15) & ~15u;
			ok = upload_clip(b + 12, nb, s_mus_addr, 1) != 0;
			s_mus_pitch = pitch_of((int)ru16(b + 6));
			s_mus_ready = ok;
			if (ok && was) {
				sfx_io_music_play();
			}
		}
	}
	free(b);
	return ok;
}

void sfx_io_music_unload(void)
{
	sfx_io_music_stop();
	s_mus_ready = 0;
}

static int vol_spu(float v)
{
	v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
	return (int)(v * 16383.0f);
}

void sfx_io_music_play(void)
{
	if (!s_mus_ready) {
		return;
	}
	voice_start(VOICE_MUSIC, s_mus_addr, s_mus_pitch, vol_spu(s_mus_vol));
	s_mus_playing = 1;
}

void sfx_io_music_stop(void)
{
	s_fade_left = 0.0f;
	if (s_ok && s_mus_playing) {
		SPU_KEY_OFF1 = (uint16_t)(1 << VOICE_MUSIC);
	}
	s_mus_playing = 0;
}

static void apply_music_vol(void)
{
	if (s_mus_playing) {
		const int v = vol_spu(s_mus_vol);
		SPU_CH_VOL_L(VOICE_MUSIC) = (uint16_t)v;
		SPU_CH_VOL_R(VOICE_MUSIC) = (uint16_t)v;
	}
}

void sfx_io_music_set_vol(float vol)
{
	s_mus_vol = vol < 0.0f ? 0.0f : (vol > 1.0f ? 1.0f : vol);
	s_fade_left = 0.0f;
	apply_music_vol();
}

void sfx_io_music_fade(float to_vol, float ms)
{
	to_vol = to_vol < 0.0f ? 0.0f : (to_vol > 1.0f ? 1.0f : to_vol);
	if (ms <= 0.0f) {
		sfx_io_music_set_vol(to_vol);
		return;
	}
	s_fade_from = s_mus_vol;
	s_fade_to = to_vol;
	s_fade_left = ms;
	s_fade_total = ms;
}

void sfx_io_tick(float delta)
{
	if (s_fade_left <= 0.0f || s_fade_total <= 0.0f) {
		return;
	}
	s_fade_left -= delta * 1000.0f;
	float t = 1.0f - s_fade_left / s_fade_total;
	if (t >= 1.0f || s_fade_left <= 0.0f) {
		t = 1.0f;
		s_fade_left = 0.0f;
	}
	s_mus_vol = s_fade_from + (s_fade_to - s_fade_from) * t;
	apply_music_vol();
}
