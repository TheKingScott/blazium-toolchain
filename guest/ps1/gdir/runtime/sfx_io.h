// MIT. Load ABI 1 "SFX " IMA ADPCM. Play on Cross. AFL freesd when present; no audsrv.

#ifndef BLAZIUM_PS2_SFX_IO_H
#define BLAZIUM_PS2_SFX_IO_H

#ifdef __cplusplus
extern "C" {
#endif

int sfx_io_init(void);
int sfx_io_load(int pack);
void sfx_io_unload(void);
void sfx_io_play(void);
void sfx_io_play_clip(int clip); /* bank index (ps2/audio/sfx_bank order) */
int sfx_io_clip_count(void);
void sfx_io_stop(void);
int sfx_io_ready(void);
int sfx_io_audible(void);
int sfx_io_music_loaded(void);
int sfx_io_music_load(int pack);
void sfx_io_music_unload(void);
void sfx_io_music_play(void);
void sfx_io_music_stop(void);
void sfx_io_music_set_vol(float vol);
void sfx_io_music_fade(float to_vol, float ms);
void sfx_io_tick(float delta);

#ifdef __cplusplus
}
#endif

#endif
