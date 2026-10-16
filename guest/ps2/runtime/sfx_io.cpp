// MIT IMA ADPCM of ABI 1 "SFX ". Load host: then cdrom0:.
// Voice: sdrdrv SIF RPC (rSdInit / sce_SDR_DEV) + freesd. Do not include
// libsd.h/libsdr.h or call sceSdInit/sceSdRemote (this -lsdr lacks those).

#include "sfx_io.h"
#include "iso_path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__has_include)
#if __has_include("irx_flags.h")
#include "irx_flags.h"
#endif
#if __has_include(<loadfile.h>)
#include <loadfile.h>
#define BLAZIUM_PS2_HAS_LOADFILE 1
#endif
#if __has_include(<iopheap.h>)
#include <iopheap.h>
#define BLAZIUM_PS2_HAS_IOPHEAP 1
#endif
#if __has_include(<kernel.h>)
#include <kernel.h>
#endif
#if __has_include(<libsdr-common.h>) && __has_include(<sifrpc.h>)
extern "C" {
#include <libsd-common.h>
#include <libsdr-common.h>
#include <sifrpc.h>
#if __has_include(<sifdma.h>)
#include <sifdma.h>
#endif
}
#define BLAZIUM_PS2_HAS_LIBSDR 1
#endif
#endif

#ifdef BLAZIUM_PS2_HAS_IRX_FREESD
extern "C" {
extern const unsigned char irx_freesd[];
extern const unsigned int size_irx_freesd;
}
#endif
#ifdef BLAZIUM_PS2_HAS_IRX_SDRDRV
extern "C" {
extern const unsigned char irx_sdrdrv[];
extern const unsigned int size_irx_sdrdrv;
}
#endif

static const int s_ima_step[89] = {
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

static const int s_ima_index[16] = {
	-1, -1, -1, -1, 2, 4, 6, 8,
	-1, -1, -1, -1, 2, 4, 6, 8
};

static short *s_pcm;
static int s_pcm_n;
static int s_rate;
static unsigned char *s_vag; /* every clip's VAG, back to back */
static unsigned s_vag_sz;
/* The bank ("SFXB", or one "SFX " clip as a bank of one). It is uploaded to
 * SPU2 RAM once when the pack loads; play only starts a voice. */
#define SFX_MAX_CLIPS 64
#define SFX_SPU_BASE 0x5000
#define SFX_SPU_END 0x10000 /* music starts here */
#define SFX_VOICE_FIRST 2 /* voice 0: unused, 1: music */
#define SFX_VOICE_COUNT 8
static unsigned s_clip_off[SFX_MAX_CLIPS];
static unsigned s_clip_len[SFX_MAX_CLIPS];
static int s_clip_rate[SFX_MAX_CLIPS];
static int s_clip_n;
static int s_bank_on_spu;
static int s_next_voice;
static int s_ready;
static int s_sfx_pack = -1;
static int s_played;
static int s_audible;
static int s_sd_ok;
static short *s_mus_pcm;
static int s_mus_pcm_n;
static int s_mus_rate;
static unsigned char *s_mus_vag;
static unsigned s_mus_vag_sz;
static int s_mus_ready;
static int s_mus_playing;
static int s_mus_vol;
static float s_mus_vol_f;
static float s_fade_from;
static float s_fade_to;
static float s_fade_left;
static float s_fade_total;
#ifdef BLAZIUM_PS2_HAS_LIBSDR
static SifRpcClientData_t s_sd_cd __attribute__((aligned(64)));
static int s_sbuff[16] __attribute__((aligned(64)));
static int s_sd_bound;
#endif

static unsigned ru16(const unsigned char *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned ru32(const unsigned char *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static int load_blob(const char **paths, unsigned char **out, unsigned *sz)
{
	for (int i = 0; paths[i]; i++) {
		FILE *f = iso_fopen(paths[i], "rb");
		if (!f) {
			continue;
		}
		if (fseek(f, 0, SEEK_END) != 0) {
			fclose(f);
			continue;
		}
		long n = ftell(f);
		if (n < 12) {
			fclose(f);
			continue;
		}
		rewind(f);
		unsigned char *buf = (unsigned char *)malloc((size_t)n);
		if (!buf) {
			fclose(f);
			continue;
		}
		if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
			free(buf);
			fclose(f);
			continue;
		}
		fclose(f);
		*out = buf;
		*sz = (unsigned)n;
		return 1;
	}
	return 0;
}

static int decode_ima(const unsigned char *nibbles, unsigned nbytes, short **out_pcm, int *out_n)
{
	int pred = 0;
	int idx = 0;
	const unsigned samples = nbytes * 2;
	short *pcm = (short *)malloc(samples * sizeof(short));
	if (!pcm) {
		return 0;
	}
	*out_pcm = pcm;
	*out_n = (int)samples;
	unsigned o = 0;
	for (unsigned i = 0; i < nbytes; i++) {
		for (int half = 0; half < 2; half++) {
			const unsigned nibble = (half == 0) ? (nibbles[i] & 0x0f) : (nibbles[i] >> 4);
			const int step = s_ima_step[idx];
			int delta = step >> 3;
			if (nibble & 4) {
				delta += step;
			}
			if (nibble & 2) {
				delta += step >> 1;
			}
			if (nibble & 1) {
				delta += step >> 2;
			}
			if (nibble & 8) {
				pred -= delta;
			} else {
				pred += delta;
			}
			if (pred > 32767) {
				pred = 32767;
			}
			if (pred < -32768) {
				pred = -32768;
			}
			idx += s_ima_index[nibble];
			if (idx < 0) {
				idx = 0;
			}
			if (idx > 88) {
				idx = 88;
			}
			pcm[o++] = (short)pred;
		}
	}
	return 1;
}

static int encode_vag(const short *pcm, int n, unsigned char **out_vag, unsigned *out_sz, int loop)
{
	if (!pcm || n < 1) {
		return 0;
	}
	if (n > 11200) {
		n = 11200;
	}
	const int blocks = (n + 27) / 28;
	unsigned sz = (unsigned)blocks * 16;
	unsigned char *vag = (unsigned char *)malloc(sz + 16);
	if (!vag) {
		return 0;
	}
	memset(vag, 0, sz);
	for (int b = 0; b < blocks; b++) {
		unsigned char *blk = vag + b * 16;
		int maxabs = 0;
		short samp[28];
		for (int i = 0; i < 28; i++) {
			const int idx = b * 28 + i;
			const short s = (idx < n) ? pcm[idx] : 0;
			samp[i] = s;
			int a = s < 0 ? -s : s;
			if (a > maxabs) {
				maxabs = a;
			}
		}
		int shift = 12;
		while (shift > 0 && (maxabs >> (12 - shift)) > 7) {
			shift--;
		}
		if (shift < 0) {
			shift = 0;
		}
		blk[0] = (unsigned char)(shift & 0x0f);
		if (loop) {
			/* ADPCM_LOOP_START=4 ADPCM_LOOP=2 ADPCM_LOOP_END=1 */
			if (blocks == 1) {
				blk[1] = 7;
			} else if (b == 0) {
				blk[1] = 6;
			} else if (b == blocks - 1) {
				blk[1] = 3;
			} else {
				blk[1] = 2;
			}
		} else {
			blk[1] = (b == blocks - 1) ? 1 : 0;
		}
		for (int i = 0; i < 14; i++) {
			int n0 = samp[i * 2] >> (12 - shift);
			int n1 = samp[i * 2 + 1] >> (12 - shift);
			if (n0 > 7) {
				n0 = 7;
			}
			if (n0 < -8) {
				n0 = -8;
			}
			if (n1 > 7) {
				n1 = 7;
			}
			if (n1 < -8) {
				n1 = -8;
			}
			blk[2 + i] = (unsigned char)((n0 & 0x0f) | ((n1 & 0x0f) << 4));
		}
	}
	*out_vag = vag;
	*out_sz = sz;
	return 1;
}

static void load_sound_irx(void)
{
#ifdef BLAZIUM_PS2_HAS_LOADFILE
#ifdef BLAZIUM_PS2_HAS_IRX_FREESD
	SifExecModuleBuffer((void *)irx_freesd, (int)size_irx_freesd, 0, NULL, NULL);
#endif
#ifdef BLAZIUM_PS2_HAS_IRX_SDRDRV
	SifExecModuleBuffer((void *)irx_sdrdrv, (int)size_irx_sdrdrv, 0, NULL, NULL);
#endif
#endif
}

#ifdef BLAZIUM_PS2_HAS_LIBSDR
static int sdr_bind(void)
{
	if (s_sd_bound) {
		return 1;
	}
	memset(&s_sd_cd, 0, sizeof(s_sd_cd));
	for (;;) {
		if (sceSifBindRpc(&s_sd_cd, sce_SDR_DEV, 0) < 0) {
			return 0;
		}
		if (s_sd_cd.server != NULL) {
			s_sd_bound = 1;
			return 1;
		}
		nopdelay();
	}
}

static int sdr_cmd(int cmd, int a1, int a2, int a3, int a4, int a5)
{
	if (!sdr_bind()) {
		return -1;
	}
	s_sbuff[0] = (int)(unsigned long)s_sbuff;
	s_sbuff[1] = a1;
	s_sbuff[2] = a2;
	s_sbuff[3] = a3;
	s_sbuff[4] = a4;
	s_sbuff[5] = a5;
	if (sceSifCallRpc(&s_sd_cd, cmd, 0, &s_sbuff[0], sizeof(s_sbuff), &s_sbuff[0], 16, NULL, NULL) < 0) {
		return -1;
	}
	return s_sbuff[0];
}

static int sdr_trans_to_iop(void *ee, void *iop, unsigned size)
{
	SifDmaTransfer_t dmat;
	dmat.src = ee;
	dmat.dest = iop;
	dmat.size = size;
	dmat.attr = 0;
	int id = 0;
	while (!(id = sceSifSetDma(&dmat, 1))) {
	}
	while (sceSifDmaStat(id) >= 0) {
	}
	return 0;
}
#endif

static int init_sdr(void)
{
#ifdef BLAZIUM_PS2_HAS_LIBSDR
	if (!sdr_bind()) {
		return 0;
	}
	sdr_cmd(rSdInit, 0, 0, 0, 0, 0);
	/* Both cores: core 0's voices feed core 1's input, and core 1 is what
	 * reaches the outputs, so both need their master volume up and their
	 * mixers open (voices dry, sound-in, memory-in; no reverb). */
	for (int core = 0; core < 2; core++) {
		sdr_cmd(rSdSetParam, SD_PARAM_MVOLL | core, 0x3fff, 0, 0, 0);
		sdr_cmd(rSdSetParam, SD_PARAM_MVOLR | core, 0x3fff, 0, 0, 0);
		sdr_cmd(rSdSetParam, SD_PARAM_MMIX | core, 0xffc, 0, 0, 0);
	}
	sdr_cmd(rSdSetSwitch, SD_SWITCH_VMIXL, 0xffffff, 0, 0, 0);
	sdr_cmd(rSdSetSwitch, SD_SWITCH_VMIXR, 0xffffff, 0, 0, 0);
	return 1;
#else
	return 0;
#endif
}

int sfx_io_init(void)
{
	s_ready = 0;
	s_played = 0;
	s_audible = 0;
	s_sd_ok = 0;
	s_vag = 0;
	s_vag_sz = 0;
	s_mus_ready = 0;
	s_mus_playing = 0;
	s_mus_vol = 0x3fff;
	s_mus_vol_f = 1.0f;
	s_fade_left = 0.0f;
	s_mus_vag = 0;
	s_mus_vag_sz = 0;
#ifdef BLAZIUM_PS2_HAS_LIBSDR
	s_sd_bound = 0;
#endif
	load_sound_irx();
	s_sd_ok = init_sdr();
	s_sfx_pack = -1;
	const int ok = sfx_io_load(0);
	(void)sfx_io_music_load(0);
	return ok;
}

static void free_sfx(void)
{
	sfx_io_stop();
	if (s_pcm) {
		free(s_pcm);
		s_pcm = 0;
	}
	if (s_vag) {
		free(s_vag);
		s_vag = 0;
	}
	s_pcm_n = 0;
	s_vag_sz = 0;
	s_ready = 0;
	s_sfx_pack = -1;
	s_clip_n = 0;
	s_bank_on_spu = 0;
}

/* One clip's IMA nibbles -> VAG appended to s_vag. */
static int add_clip(const unsigned char *nibbles, unsigned nbytes, int rate)
{
	if (s_clip_n >= SFX_MAX_CLIPS) {
		return 0;
	}
	short *pcm = 0;
	int n = 0;
	if (!decode_ima(nibbles, nbytes, &pcm, &n)) {
		return 0;
	}
	unsigned char *vag = 0;
	unsigned vsz = 0;
	const int ok = encode_vag(pcm, n, &vag, &vsz, 0);
	if (s_clip_n == 0) {
		/* clip 0 keeps its PCM for sfx_io_ready()/the legacy path */
		s_pcm = pcm;
		s_pcm_n = n;
		s_rate = rate;
	} else {
		free(pcm);
	}
	if (!ok) {
		return 0;
	}
	unsigned char *grown = (unsigned char *)realloc(s_vag, s_vag_sz + vsz + 64);
	if (!grown) {
		free(vag);
		return 0;
	}
	s_vag = grown;
	memcpy(s_vag + s_vag_sz, vag, vsz);
	free(vag);
	s_clip_off[s_clip_n] = s_vag_sz;
	s_clip_len[s_clip_n] = vsz;
	s_clip_rate[s_clip_n] = rate;
	s_clip_n++;
	s_vag_sz += vsz;
	return 1;
}

static int decode_sfx_blob(unsigned char *blob, unsigned sz)
{
	if (!blob || sz < 8 || blob[0] != 'S' || blob[1] != 'F' || blob[2] != 'X' || ru16(blob + 4) != 1) {
		return 0;
	}
	s_clip_n = 0;
	if (blob[3] == ' ') {
		if (sz < 12) {
			return 0;
		}
		const unsigned nbytes = ru32(blob + 8);
		if (12 + nbytes > sz || !add_clip(blob + 12, nbytes, (int)ru16(blob + 6))) {
			return 0;
		}
	} else if (blob[3] == 'B') {
		const unsigned count = ru16(blob + 6);
		unsigned off = 8;
		for (unsigned i = 0; i < count; i++) {
			if (off + 6 > sz) {
				break;
			}
			const int rate = (int)ru16(blob + off);
			const unsigned nbytes = ru32(blob + off + 2);
			off += 6;
			if (off + nbytes > sz) {
				break;
			}
			if (!add_clip(blob + off, nbytes, rate)) {
				/* keep the index of every later clip right: an empty slot */
				if (s_clip_n < SFX_MAX_CLIPS) {
					s_clip_off[s_clip_n] = 0;
					s_clip_len[s_clip_n] = 0;
					s_clip_rate[s_clip_n] = 0;
					s_clip_n++;
				}
			}
			off += nbytes;
		}
	} else {
		return 0;
	}
	s_ready = s_clip_n > 0;
	return s_ready;
}

/* The whole bank to SPU2 RAM in one transfer; clips past SFX_SPU_END are
 * dropped (they would run into the music). */
static void upload_bank(void)
{
	s_bank_on_spu = 0;
#ifdef BLAZIUM_PS2_HAS_LIBSDR
#ifdef BLAZIUM_PS2_HAS_IOPHEAP
	if (!s_sd_ok || !s_vag || s_vag_sz < 16) {
		return;
	}
	unsigned sz = s_vag_sz;
	if (SFX_SPU_BASE + sz > SFX_SPU_END) {
		sz = SFX_SPU_END - SFX_SPU_BASE;
		for (int i = 0; i < s_clip_n; i++) {
			if (s_clip_off[i] + s_clip_len[i] > sz) {
				printf("SFX: clip %d does not fit in SPU2 RAM (%u bytes of %u), silent\n", i, s_vag_sz, sz);
				s_clip_len[i] = 0;
			}
		}
	}
	sz = (sz + 63) & ~63u;
	void *iop = SifAllocIopHeap((int)sz);
	if (!iop) {
		return;
	}
	FlushCache(0);
	if (sdr_trans_to_iop(s_vag, iop, sz) < 0) {
		SifFreeIopHeap(iop);
		return;
	}
	sdr_cmd(rSdVoiceTrans, 0, SD_TRANS_WRITE | SD_TRANS_MODE_DMA, (int)(unsigned long)iop, SFX_SPU_BASE, (int)sz);
	sdr_cmd(rSdVoiceTransStatus, 0, 1, 0, 0, 0);
	SifFreeIopHeap(iop);
	s_bank_on_spu = 1;
#endif
#endif
}

int sfx_io_load(int pack)
{
	if (pack < 0) {
		pack = 0;
	}
	char host[48];
	char iso_bs[56];
	char iso[56];
	sprintf(host, "host:SFX%02d.bin", pack);
	sprintf(iso_bs, "cdrom0:\\SFX%02d.BIN;1", pack);
	sprintf(iso, "cdrom0:SFX%02d.BIN;1", pack);
	const char *paths[] = { host, iso_bs, iso, NULL };
	unsigned char *blob = 0;
	unsigned sz = 0;
	if (!load_blob(paths, &blob, &sz) && pack != 0) {
		static const char *p0[] = {
			"host:SFX00.bin",
			"cdrom0:\\SFX00.BIN;1",
			"cdrom0:SFX00.BIN;1",
			NULL
		};
		(void)load_blob(p0, &blob, &sz);
	}
	if (!blob) {
		return 0;
	}
	free_sfx();
	const int ok = decode_sfx_blob(blob, sz);
	free(blob);
	if (ok) {
		s_sfx_pack = pack;
		upload_bank();
		/* One line in the EE log per bank, so a silent run says why. */
		printf("SFX: pack %d, %d clips, %u bytes, driver %s, on SPU2 %s\n", pack, s_clip_n, s_vag_sz,
				s_sd_ok ? "ok" : "missing", s_bank_on_spu ? "yes" : "no");
	}
	return ok;
}

void sfx_io_unload(void)
{
	const int was = s_sfx_pack;
	free_sfx();
	if (was > 0) {
		(void)sfx_io_load(0);
	}
}

void sfx_io_play(void)
{
	sfx_io_play_clip(0);
}

void sfx_io_play_clip(int clip)
{
	if (!s_ready || clip < 0 || clip >= s_clip_n || !s_clip_len[clip]) {
		return;
	}
	s_played = 1;
	s_audible = 0;
#ifdef BLAZIUM_PS2_HOST_HARNESS
	printf("SFX play clip %d\n", clip);
#endif
#ifdef BLAZIUM_PS2_HAS_LIBSDR
	if (!s_sd_ok || !s_bank_on_spu) {
		return;
	}
	/* round-robin over a few voices so sounds overlap instead of cutting.
	 * ADSR: instant attack (rate 0; 0x7f is "never", which is why the old
	 * one-sound path was silent), hold at full, short release. */
	const int v = SFX_VOICE_FIRST + s_next_voice;
	s_next_voice = (s_next_voice + 1) % SFX_VOICE_COUNT;
	const int voice = SD_VOICE(0, v);
	const int pitch = s_clip_rate[clip] > 0 ? (s_clip_rate[clip] * 4096) / 48000 : 2048;
	sdr_cmd(rSdSetSwitch, SD_SWITCH_KOFF, 1 << v, 0, 0, 0);
	sdr_cmd(rSdSetParam, SD_VPARAM_VOLL | voice, 0x3fff, 0, 0, 0);
	sdr_cmd(rSdSetParam, SD_VPARAM_VOLR | voice, 0x3fff, 0, 0, 0);
	sdr_cmd(rSdSetParam, SD_VPARAM_PITCH | voice, pitch, 0, 0, 0);
	sdr_cmd(rSdSetParam, SD_VPARAM_ADSR1 | voice, SD_SET_ADSR1(0, 0x00, 0xf, 0xf), 0, 0, 0);
	sdr_cmd(rSdSetParam, SD_VPARAM_ADSR2 | voice, SD_SET_ADSR2(0, 0x7f, 0, 0x0a), 0, 0, 0);
	sdr_cmd(rSdSetAddr, SD_VADDR_SSA | voice, (int)(SFX_SPU_BASE + s_clip_off[clip]), 0, 0, 0);
	sdr_cmd(rSdSetSwitch, SD_SWITCH_KON, 1 << v, 0, 0, 0);
	s_audible = 1;
#endif
}

int sfx_io_clip_count(void)
{
	return s_clip_n;
}

void sfx_io_stop(void)
{
	s_played = 0;
#ifdef BLAZIUM_PS2_HAS_LIBSDR
	if (s_sd_ok) {
		sdr_cmd(rSdSetSwitch, SD_SWITCH_KOFF, ((1 << SFX_VOICE_COUNT) - 1) << SFX_VOICE_FIRST, 0, 0, 0);
	}
#endif
}

int sfx_io_ready(void)
{
	return s_ready;
}

int sfx_io_audible(void)
{
	return s_audible;
}

static int vol_spu(float v)
{
	if (v < 0.0f) {
		v = 0.0f;
	}
	if (v > 1.0f) {
		v = 1.0f;
	}
	return (int)(v * 16383.0f);
}

static void apply_music_vol(void)
{
#ifdef BLAZIUM_PS2_HAS_LIBSDR
	if (!s_sd_ok) {
		return;
	}
	const int voice = SD_VOICE(0, 1);
	sdr_cmd(rSdSetParam, SD_VPARAM_VOLL | voice, s_mus_vol, 0, 0, 0);
	sdr_cmd(rSdSetParam, SD_VPARAM_VOLR | voice, s_mus_vol, 0, 0, 0);
#endif
}

int sfx_io_music_loaded(void)
{
	return s_mus_ready;
}

static void free_music(void)
{
	sfx_io_music_stop();
	if (s_mus_pcm) {
		free(s_mus_pcm);
		s_mus_pcm = 0;
	}
	if (s_mus_vag) {
		free(s_mus_vag);
		s_mus_vag = 0;
	}
	s_mus_pcm_n = 0;
	s_mus_vag_sz = 0;
	s_mus_ready = 0;
}

static int decode_music_blob(unsigned char *mblob, unsigned msz)
{
	if (!mblob || msz < 12) {
		return 0;
	}
	if (mblob[0] != 'M' || mblob[1] != 'U' || mblob[2] != 'S' || mblob[3] != ' ') {
		return 0;
	}
	if (ru16(mblob + 4) != 1) {
		return 0;
	}
	s_mus_rate = (int)ru16(mblob + 6);
	const unsigned mnbytes = ru32(mblob + 8);
	if (12 + mnbytes > msz) {
		return 0;
	}
	if (!decode_ima(mblob + 12, mnbytes, &s_mus_pcm, &s_mus_pcm_n)) {
		return 0;
	}
	if (!encode_vag(s_mus_pcm, s_mus_pcm_n, &s_mus_vag, &s_mus_vag_sz, 1)) {
		return 0;
	}
	s_mus_ready = 1;
	return 1;
}

int sfx_io_music_load(int pack)
{
	if (pack < 0) {
		pack = 0;
	}
	char host[48];
	char iso_bs[56];
	char iso[56];
	sprintf(host, "host:MUSIC%02d.bin", pack);
	sprintf(iso_bs, "cdrom0:\\MUSIC%02d.BIN;1", pack);
	sprintf(iso, "cdrom0:MUSIC%02d.BIN;1", pack);
	const char *paths[] = { host, iso_bs, iso, NULL };
	unsigned char *blob = 0;
	unsigned sz = 0;
	if (!load_blob(paths, &blob, &sz) && pack != 0) {
		static const char *p0[] = {
			"host:MUSIC00.bin",
			"cdrom0:\\MUSIC00.BIN;1",
			"cdrom0:MUSIC00.BIN;1",
			NULL
		};
		(void)load_blob(p0, &blob, &sz);
	}
	if (!blob) {
		return 0;
	}
	free_music();
	const int ok = decode_music_blob(blob, sz);
	free(blob);
	return ok;
}

void sfx_io_music_unload(void)
{
	free_music();
}

void sfx_io_music_play(void)
{
	if (!s_mus_ready || !s_mus_pcm) {
		return;
	}
#ifdef BLAZIUM_PS2_HAS_LIBSDR
#ifdef BLAZIUM_PS2_HAS_IOPHEAP
	if (!s_sd_ok || !s_mus_vag || s_mus_vag_sz < 16) {
		return;
	}
	void *iop = SifAllocIopHeap((int)s_mus_vag_sz);
	if (!iop) {
		return;
	}
#ifdef BLAZIUM_PS2_HAS_LOADFILE
	FlushCache(0);
#endif
	if (sdr_trans_to_iop(s_mus_vag, iop, s_mus_vag_sz) < 0) {
		SifFreeIopHeap(iop);
		return;
	}
	const int pitch = s_mus_rate > 0 ? (s_mus_rate * 4096) / 44100 : 2048;
	const int voice = SD_VOICE(0, 1);
	sdr_cmd(rSdVoiceTrans, 0, SD_TRANS_WRITE | SD_TRANS_MODE_DMA, (int)(unsigned long)iop, 0x10000, (int)s_mus_vag_sz);
	sdr_cmd(rSdVoiceTransStatus, 0, 1, 0, 0, 0);
	sdr_cmd(rSdSetParam, SD_VPARAM_VOLL | voice, s_mus_vol, 0, 0, 0);
	sdr_cmd(rSdSetParam, SD_VPARAM_VOLR | voice, s_mus_vol, 0, 0, 0);
	sdr_cmd(rSdSetParam, SD_VPARAM_PITCH | voice, pitch, 0, 0, 0);
	sdr_cmd(rSdSetParam, SD_VPARAM_ADSR1 | voice, SD_SET_ADSR1(0, 0x00, 0xf, 0xf), 0, 0, 0);
	sdr_cmd(rSdSetParam, SD_VPARAM_ADSR2 | voice, SD_SET_ADSR2(0, 0x7f, 0, 0x0a), 0, 0, 0);
	sdr_cmd(rSdSetAddr, SD_VADDR_SSA | voice, 0x10000, 0, 0, 0);
	sdr_cmd(rSdSetSwitch, SD_SWITCH_KON, 2, 0, 0, 0);
	SifFreeIopHeap(iop);
	s_mus_playing = 1;
	return;
#endif
#endif
	s_mus_playing = 1;
	(void)s_mus_rate;
	(void)s_mus_pcm_n;
}

void sfx_io_music_stop(void)
{
	s_mus_playing = 0;
	s_fade_left = 0.0f;
#ifdef BLAZIUM_PS2_HAS_LIBSDR
	if (s_sd_ok) {
		sdr_cmd(rSdSetSwitch, SD_SWITCH_KOFF, 2, 0, 0, 0);
	}
#endif
}

void sfx_io_music_set_vol(float vol)
{
	if (vol < 0.0f) {
		vol = 0.0f;
	}
	if (vol > 1.0f) {
		vol = 1.0f;
	}
	s_mus_vol_f = vol;
	s_mus_vol = vol_spu(vol);
	s_fade_left = 0.0f;
	if (s_mus_playing) {
		apply_music_vol();
	}
}

void sfx_io_music_fade(float to_vol, float ms)
{
	if (to_vol < 0.0f) {
		to_vol = 0.0f;
	}
	if (to_vol > 1.0f) {
		to_vol = 1.0f;
	}
	if (ms <= 0.0f) {
		sfx_io_music_set_vol(to_vol);
		return;
	}
	s_fade_from = s_mus_vol_f;
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
	float t = 1.0f - (s_fade_left / s_fade_total);
	if (t < 0.0f) {
		t = 0.0f;
	}
	if (t > 1.0f || s_fade_left <= 0.0f) {
		t = 1.0f;
		s_fade_left = 0.0f;
	}
	s_mus_vol_f = s_fade_from + (s_fade_to - s_fade_from) * t;
	s_mus_vol = vol_spu(s_mus_vol_f);
	if (s_mus_playing) {
		apply_music_vol();
	}
}
