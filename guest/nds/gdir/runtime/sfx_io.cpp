// MIT. The PS2 guest's sound API (sfx_io.h) on the DS's sound channels: the
// export's SFXnn.BIN ("SFXB" bank or one "SFX " clip) and MUSICnn.BIN
// ("MUS "), IMA ADPCM, which the DS plays as it is. Each clip gets the
// 4-byte header the hardware starts from (sample 0, step index 0: how the
// export encodes) and the ARM7 mixes: the music on one channel (looping),
// each sound on a free one, so they overlap instead of cutting.

#include "sfx_io.h"

#include "iso_path.h"

#include <malloc.h>
#include <nds.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CLIPS 64
#define SFX_CHANS 8 /* the sounds playing that sfx_io_stop stops */

struct Clip {
	uint8_t *data; /* header + ADPCM, word aligned and padded */
	uint32_t size; /* bytes, a multiple of 4 */
	uint16_t rate;
};

static int s_ok;
static int s_ready;
static int s_pack = -1;
static Clip s_clips[MAX_CLIPS];
static int s_clip_n;
static Clip s_music;
static int s_chan[SFX_CHANS];
static int s_next;
static int s_audible;
static int s_mus_ready, s_mus_playing, s_mus_chan = -1;
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

/* a clip as the hardware takes it: the header (sample 0, index 0), the data */
static int make_clip(const uint8_t *ima, unsigned nbytes, int rate, Clip *c)
{
	c->data = 0;
	c->size = 0;
	c->rate = (uint16_t)(rate > 0 && rate < 65536 ? rate : 22050);
	if (!nbytes) {
		return 0;
	}
	const uint32_t size = (4 + nbytes + 3) & ~3u;
	uint8_t *d = (uint8_t *)memalign(32, size);
	if (!d) {
		printf("DS sound: no memory for a clip of %u bytes\n", nbytes);
		return 0;
	}
	memset(d, 0, 4);
	memcpy(d + 4, ima, nbytes);
	memset(d + 4 + nbytes, 0, size - 4 - nbytes);
	DC_FlushRange(d, size); /* the ARM7 reads it */
	c->data = d;
	c->size = size;
	return 1;
}

static void free_clip(Clip *c)
{
	free(c->data);
	c->data = 0;
	c->size = 0;
}

int sfx_io_init(void)
{
	soundEnable();
	for (int i = 0; i < SFX_CHANS; i++) {
		s_chan[i] = -1;
	}
	s_ok = 1;
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
			make_clip(b + 12, nb, (int)ru16(b + 6), &s_clips[0]);
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
			make_clip(b + off, nb, rate, &s_clips[s_clip_n]);
			s_clip_n++;
			off += nb;
		}
	}
	free(b);
	s_pack = pack;
	s_ready = s_clip_n > 0;
	printf("DS sound: pack %d, %d clips\n", pack, s_clip_n);
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
	if (!s_ready || clip < 0 || clip >= s_clip_n || !s_clips[clip].data) {
		return;
	}
	const Clip *c = &s_clips[clip];
	const int ch = soundPlaySample(c->data, SoundFormat_ADPCM, c->size, c->rate, 127, 64, false, 0);
	if (ch >= 0) {
		/* a sound still in this place was older than SFX_CHANS others: it
		 * plays on (the hardware frees its channel when it ends) */
		s_chan[s_next] = ch;
		s_next = (s_next + 1) % SFX_CHANS;
		s_audible = 1;
	}
}

int sfx_io_clip_count(void)
{
	return s_clip_n;
}

void sfx_io_stop(void)
{
	for (int i = 0; i < SFX_CHANS; i++) {
		if (s_chan[i] >= 0 && s_chan[i] != s_mus_chan) {
			soundKill(s_chan[i]);
		}
		s_chan[i] = -1;
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
			/* the file becomes the clip where it is (the RAM holds one
			 * copy): the header over the file's, the data after it */
			s_music.rate = (uint16_t)(ru16(b + 6) ? ru16(b + 6) : 22050);
			memmove(b + 4, b + 12, nb);
			memset(b, 0, 4);
			s_music.size = (4 + nb + 3) & ~3u;
			memset(b + 4 + nb, 0, s_music.size - 4 - nb);
			DC_FlushRange(b, s_music.size);
			s_music.data = b;
			b = 0;
			ok = nb > 0;
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
	free_clip(&s_music);
	s_mus_ready = 0;
}

static int vol127(float v)
{
	v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
	return (int)(v * 127.0f);
}

void sfx_io_music_play(void)
{
	if (!s_mus_ready) {
		return;
	}
	if (s_mus_chan >= 0) {
		soundKill(s_mus_chan);
	}
	/* the loop starts after the header (a word in), where it decodes from */
	s_mus_chan = soundPlaySample(s_music.data, SoundFormat_ADPCM, s_music.size, s_music.rate, (u8)vol127(s_mus_vol), 64, true, 1);
	s_mus_playing = s_mus_chan >= 0;
}

void sfx_io_music_stop(void)
{
	s_fade_left = 0.0f;
	if (s_mus_chan >= 0) {
		soundKill(s_mus_chan);
	}
	s_mus_chan = -1;
	s_mus_playing = 0;
}

static void apply_music_vol(void)
{
	if (s_mus_playing && s_mus_chan >= 0) {
		soundSetVolume(s_mus_chan, (u8)vol127(s_mus_vol));
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
