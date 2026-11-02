// MIT. The PS2 guest's sound API (sfx_io.h) on the Wii U's AX (48 kHz):
// the export's SFXnn.BIN ("SFXB" bank or one "SFX " clip) and MUSICnn.BIN
// ("MUS "), IMA ADPCM, decoded to 16-bit PCM when they load, and mixed here
// in software (as the N64, PS3 and Wii guests do): one music voice
// (looping) and eight for the sounds, round robin so they overlap instead
// of cutting, resampled with linear interpolation. The mix goes to one AX
// voice looping over a ring of 16-bit samples, to the TV's and the
// GamePad's left and right; AX's frame callback (every 3 ms) mixes the ring
// up to a little ahead of where the voice is playing, so a long frame or a
// scene loading does not starve it.

#include "sfx_io.h"

#include "iso_path.h"

#include <coreinit/cache.h>
#include <coreinit/spinlock.h>
#include <sndcore2/core.h>
#include <sndcore2/device.h>
#include <sndcore2/voice.h>
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

/* the voices are the mixer's: AX's callback reads them, the game changes
 * them under this lock (a spin lock, the callback cannot sleep) */
static OSSpinLock s_lock;
static void lock(void)
{
	OSUninterruptibleSpinLock_Acquire(&s_lock);
}

static void unlock(void)
{
	OSUninterruptibleSpinLock_Release(&s_lock);
}

#define RING 4096  /* mono samples the voice loops over: about 85 ms */
#define AHEAD 1536 /* how far ahead of the voice the mix runs: 32 ms */
static int16_t *s_ring;
static AXVoice *s_ax;
static uint32_t s_wr; /* the next sample to mix, as a ring offset */

/* n mono samples at the ring's offset at */
static void fill(uint32_t at, uint32_t n)
{
	static int32_t mix[RING];
	memset(mix, 0, n * sizeof(int32_t));
	for (int v = 0; v <= VOICES; v++) {
		volatile Voice *vc = &s_voice[v];
		const Clip *c = vc->clip;
		if (!c || !c->pcm || !c->n) {
			continue;
		}
		uint32_t pos = vc->pos;
		const uint32_t step = c->step, end = c->n << 16;
		const int vol = vc->vol;
		for (uint32_t i = 0; i < n; i++) {
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
	for (uint32_t i = 0; i < n; i++) {
		int32_t s = mix[i];
		s = s > 32767 ? 32767 : (s < -32768 ? -32768 : s);
		s_ring[(at + i) % RING] = (int16_t)s;
	}
}

/* AX's frame: mix from where the last one stopped to AHEAD past the voice */
static void on_frame(void)
{
	if (!s_ax) {
		return;
	}
	AXVoiceOffsets o;
	AXGetVoiceOffsets(s_ax, &o);
	const uint32_t play = o.currentOffset % RING;
	uint32_t ahead = (s_wr + RING - play) % RING;
	if (ahead > AHEAD + 512) {
		/* fell behind (the voice passed it): start again at the voice */
		s_wr = (play + 64) % RING;
		ahead = 64;
	}
	if (ahead >= AHEAD) {
		return;
	}
	uint32_t n = AHEAD - ahead;
	lock();
	while (n) {
		const uint32_t run = n < RING - s_wr ? n : RING - s_wr;
		fill(s_wr, run);
		DCFlushRange(s_ring + s_wr, run * 2);
		s_wr = (s_wr + run) % RING;
		n -= run;
	}
	unlock();
}

static int audio_start(void)
{
	OSInitSpinLock(&s_lock);
	s_ring = (int16_t *)memalign(64, RING * 2);
	if (!s_ring) {
		return 0;
	}
	memset(s_ring, 0, RING * 2);
	DCFlushRange(s_ring, RING * 2);
	if (!AXIsInit()) {
		AXInitParams init;
		memset(&init, 0, sizeof(init));
		init.renderer = AX_INIT_RENDERER_48KHZ;
		init.pipeline = AX_INIT_PIPELINE_SINGLE;
		AXInitWithParams(&init);
	}
	s_ax = AXAcquireVoice(31, NULL, NULL);
	if (!s_ax) {
		printf("Wii U sound: no AX voice\n");
		return 0;
	}
	AXVoiceBegin(s_ax);
	AXSetVoiceType(s_ax, 0);
	AXVoiceOffsets o;
	memset(&o, 0, sizeof(o));
	o.dataType = AX_VOICE_FORMAT_LPCM16;
	o.loopingEnabled = AX_VOICE_LOOP_ENABLED;
	o.loopOffset = 0;
	o.endOffset = RING - 1;
	o.currentOffset = 0;
	o.data = s_ring;
	AXSetVoiceOffsets(s_ax, &o);
	AXVoiceVeData ve = { 0x8000, 0 };
	AXSetVoiceVe(s_ax, &ve);
	AXSetVoiceSrcType(s_ax, AX_VOICE_SRC_TYPE_NONE);
	AXSetVoiceSrcRatio(s_ax, 1.0f);
	/* left and right, bus 0, on the TV and the GamePad */
	AXVoiceDeviceMixData mix[6];
	memset(mix, 0, sizeof(mix));
	mix[0].bus[0].volume = 0x8000;
	mix[1].bus[0].volume = 0x8000;
	AXSetVoiceDeviceMix(s_ax, AX_DEVICE_TYPE_TV, 0, mix);
	AXSetVoiceDeviceMix(s_ax, AX_DEVICE_TYPE_DRC, 0, mix);
	s_wr = 64;
	AXSetVoiceState(s_ax, AX_VOICE_STATE_PLAYING);
	AXVoiceEnd(s_ax);
	AXRegisterAppFrameCallback(on_frame);
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
	printf("Wii U sound: pack %d, %d clips\n", pack, s_clip_n);
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

/* main, leaving: the voice and AX go back to the system */
extern "C" void wiiu_sfx_shutdown(void)
{
	if (!s_ax) {
		return;
	}
	AXDeregisterAppFrameCallback(on_frame);
	AXVoiceBegin(s_ax);
	AXSetVoiceState(s_ax, AX_VOICE_STATE_STOPPED);
	AXVoiceEnd(s_ax);
	AXFreeVoice(s_ax);
	s_ax = 0;
	AXQuit();
}
