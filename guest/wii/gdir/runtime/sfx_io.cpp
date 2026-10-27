// MIT. The PS2 guest's sound API (sfx_io.h) on the Wii's audio
// interface (AI DMA, 48 kHz stereo 16-bit): the export's SFXnn.BIN ("SFXB"
// bank or one "SFX " clip) and MUSICnn.BIN ("MUS "), IMA ADPCM, decoded to
// 16-bit PCM when they load, and mixed here in software (as the N64 and PS3
// guests do): one music voice (looping) and eight for the sounds, round
// robin so they overlap instead of cutting, resampled with linear
// interpolation. The DMA's interrupt starts the next buffer and mixes the
// one after, so a long frame or a scene loading does not starve it.

#include "sfx_io.h"

#include "iso_path.h"

#include <gccore.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUT_RATE 48000
#define MAX_CLIPS 64
#define VOICES 8

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

struct Clip {
	int16_t *pcm;
	uint32_t n;    /* samples */
	uint32_t step; /* 16.16 source samples per output sample */
};

struct Voice {
	const Clip *clip; /* NULL: silent */
	uint32_t pos;     /* 16.16 */
	int vol;          /* 0..256 */
	int loop;
};

static int s_ok;
static int s_ready;
static int s_pack = -1;
static Clip s_clips[MAX_CLIPS];
static int s_clip_n;
static Clip s_music;
static volatile Voice s_voice[VOICES + 1]; /* [VOICES] is the music */
static int s_next;
static int s_audible;
static int s_mus_ready, s_mus_playing;
static float s_mus_vol = 1.0f, s_fade_from, s_fade_to, s_fade_left, s_fade_total;

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

/* IMA ADPCM (low nibble first, from 0 and index 0) to PCM */
static int decode_clip(const uint8_t *ima, unsigned nbytes, int rate, Clip *c)
{
	c->pcm = 0;
	c->n = 0;
	c->step = (uint32_t)(((uint64_t)(rate > 0 ? rate : OUT_RATE) << 16) / OUT_RATE);
	if (!nbytes) {
		return 0;
	}
	int16_t *out = (int16_t *)malloc(nbytes * 4);
	if (!out) {
		return 0;
	}
	int pred = 0, idx = 0;
	for (unsigned i = 0; i < nbytes * 2; i++) {
		const unsigned b = ima[i >> 1];
		const unsigned nib = (i & 1) ? (b >> 4) : (b & 15);
		const int step = k_ima_step[idx];
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
		pred += (nib & 8) ? -d : d;
		pred = pred > 32767 ? 32767 : (pred < -32768 ? -32768 : pred);
		idx += k_ima_index[nib];
		idx = idx < 0 ? 0 : (idx > 88 ? 88 : idx);
		out[i] = (int16_t)pred;
	}
	c->pcm = out;
	c->n = nbytes * 2;
	return 1;
}

static void free_clip(Clip *c)
{
	free(c->pcm);
	c->pcm = 0;
	c->n = 0;
}

/* the voices are the interrupt's: the game changes them with it held off */
static u32 s_irq;
static void lock(void)
{
	s_irq = IRQ_Disable();
}

static void unlock(void)
{
	IRQ_Restore(s_irq);
}

#define BLOCK 512 /* stereo samples a buffer: about 10.7 ms */
static s16 *s_buf[2];
static int s_cur;

/* one buffer of BLOCK stereo samples */
static void fill_block(s16 *buf)
{
	static int32_t mix[BLOCK];
	memset(mix, 0, sizeof(mix));
	for (int v = 0; v <= VOICES; v++) {
		volatile Voice *vc = &s_voice[v];
		const Clip *c = vc->clip;
		if (!c || !c->pcm || !c->n) {
			continue;
		}
		uint32_t pos = vc->pos;
		const uint32_t step = c->step, end = c->n << 16;
		const int vol = vc->vol;
		for (int i = 0; i < BLOCK; i++) {
			if (pos >= end) {
				if (!vc->loop) {
					vc->clip = 0;
					break;
				}
				pos -= end;
			}
			const uint32_t k = pos >> 16;
			const int a = c->pcm[k];
			const int b = k + 1 < c->n ? c->pcm[k + 1] : (vc->loop ? c->pcm[0] : a);
			const int s = a + (((b - a) * (int)((pos >> 4) & 0xfff)) >> 12);
			mix[i] += (s * vol) >> 8;
			pos += step;
		}
		vc->pos = pos;
	}
	for (int i = 0; i < BLOCK; i++) {
		int32_t s = mix[i];
		s = s > 32767 ? 32767 : (s < -32768 ? -32768 : s);
		buf[i * 2] = (s16)s;
		buf[i * 2 + 1] = (s16)s;
	}
	DCFlushRange(buf, BLOCK * 4);
}

/* the AI's interrupt: the buffer just started plays next; mix the other */
static void on_dma(void)
{
	s_cur ^= 1;
	AUDIO_InitDMA((u32)s_buf[s_cur], BLOCK * 4);
	fill_block(s_buf[s_cur ^ 1]);
}

static int audio_start(void)
{
	for (int i = 0; i < 2; i++) {
		s_buf[i] = (s16 *)memalign(32, BLOCK * 4);
		if (!s_buf[i]) {
			return 0;
		}
		memset(s_buf[i], 0, BLOCK * 4);
		DCFlushRange(s_buf[i], BLOCK * 4);
	}
	AUDIO_Init(NULL);
	AUDIO_SetDSPSampleRate(AI_SAMPLERATE_48KHZ);
	AUDIO_RegisterDMACallback(on_dma);
	s_cur = 0;
	AUDIO_InitDMA((u32)s_buf[0], BLOCK * 4);
	AUDIO_StartDMA();
	return 1;
}

static void voice_start(int v, const Clip *c, int vol, int loop)
{
	lock();
	s_voice[v].clip = 0;
	s_voice[v].pos = 0;
	s_voice[v].vol = vol;
	s_voice[v].loop = loop;
	s_voice[v].clip = c;
	unlock();
}

static void voice_stop(int v)
{
	lock();
	s_voice[v].clip = 0;
	unlock();
}

int sfx_io_init(void)
{
	s_ok = audio_start();
	if (!s_ok) {
		return 0;
	}
	const int ok = sfx_io_load(0);
	(void)sfx_io_music_load(0);
	return ok;
}

static void free_bank(void)
{
	sfx_io_stop();
	for (int i = 0; i < s_clip_n; i++) {
		free_clip(&s_clips[i]);
	}
	s_clip_n = 0;
	s_ready = 0;
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
	free_bank();
	if (b[3] == ' ' && sz >= 12) {
		const unsigned nb = ru32(b + 8);
		if (12 + nb <= sz) {
			decode_clip(b + 12, nb, (int)ru16(b + 6), &s_clips[0]);
			s_clip_n = 1;
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
			decode_clip(b + off, nb, rate, &s_clips[s_clip_n]);
			s_clip_n++;
			off += nb;
		}
	}
	free(b);
	s_pack = pack;
	s_ready = s_clip_n > 0;
	printf("Wii sound: pack %d, %d clips\n", pack, s_clip_n);
	return s_ready;
}

void sfx_io_unload(void)
{
	sfx_io_stop();
}

void sfx_io_play(void)
{
	sfx_io_play_clip(0);
}

void sfx_io_play_clip(int clip)
{
	if (!s_ready || clip < 0 || clip >= s_clip_n || !s_clips[clip].pcm) {
		return;
	}
	const int v = s_next;
	s_next = (s_next + 1) % VOICES;
	voice_start(v, &s_clips[clip], 256, 0);
	s_audible = 1;
}

int sfx_io_clip_count(void)
{
	return s_clip_n;
}

void sfx_io_stop(void)
{
	for (int v = 0; v < VOICES; v++) {
		voice_stop(v);
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

/* ---- music: one looping clip ---- */

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
	if (sz >= 12 && memcmp(b, "MUS ", 4) == 0 && ru16(b + 4) == 1) {
		const unsigned nb = ru32(b + 8);
		if (12 + nb <= sz) {
			const int was = s_mus_playing;
			sfx_io_music_stop();
			free_clip(&s_music);
			ok = decode_clip(b + 12, nb, (int)ru16(b + 6), &s_music);
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

static int vol256(float v)
{
	v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
	return (int)(v * 256.0f);
}

void sfx_io_music_play(void)
{
	if (!s_mus_ready) {
		return;
	}
	voice_start(VOICES, &s_music, vol256(s_mus_vol), 1);
	s_mus_playing = 1;
}

void sfx_io_music_stop(void)
{
	s_fade_left = 0.0f;
	if (s_ok && s_mus_playing) {
		voice_stop(VOICES);
	}
	s_mus_playing = 0;
}

static void apply_music_vol(void)
{
	if (s_mus_playing) {
		lock();
		s_voice[VOICES].vol = vol256(s_mus_vol);
		unlock();
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
