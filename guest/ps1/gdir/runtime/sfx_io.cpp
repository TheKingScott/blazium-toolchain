// MIT. Sound is not on the PS1 runtime yet: every call is a quiet no-op.

#include "sfx_io.h"

int sfx_io_init(void) { return 0; }
int sfx_io_load(int pack) { (void)pack; return 0; }
void sfx_io_unload(void) {}
void sfx_io_play(void) {}
void sfx_io_play_clip(int clip) { (void)clip; }
int sfx_io_clip_count(void) { return 0; }
void sfx_io_stop(void) {}
int sfx_io_ready(void) { return 0; }
int sfx_io_audible(void) { return 0; }
int sfx_io_music_loaded(void) { return 0; }
int sfx_io_music_load(int pack) { (void)pack; return 0; }
void sfx_io_music_unload(void) {}
void sfx_io_music_play(void) {}
void sfx_io_music_stop(void) {}
void sfx_io_music_set_vol(float vol) { (void)vol; }
void sfx_io_music_fade(float to_vol, float ms) { (void)to_vol; (void)ms; }
void sfx_io_tick(float delta) { (void)delta; }
