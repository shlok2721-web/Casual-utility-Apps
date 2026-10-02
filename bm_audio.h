#ifndef BM_AUDIO_H
#define BM_AUDIO_H
typedef struct BmPlayer BmPlayer;

/* Starts looping `path` (MP3) forever on a worker thread using libmpg123 for
 * decoding and ALSA for output (goes through PipeWire if pipewire-alsa is
 * installed). Returns NULL if the thread could not be created. */
BmPlayer *bm_player_start(const char *path);

/* Stops playback immediately, joins the thread and frees everything. */
void bm_player_stop(BmPlayer *p);

/* Non-zero while the playback thread is still running. */
int bm_player_alive(BmPlayer *p);
#endif
