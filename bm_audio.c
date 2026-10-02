/*
 * bm_audio.c - infinite-loop alarm playback: libmpg123 -> ALSA.
 *
 * Nothing exists while idle: the thread, decoder, PCM handle and buffers are
 * created when an alarm starts and fully released when it stops.
 */
#include "bm_audio.h"

#include <alsa/asoundlib.h>
#include <mpg123.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>

struct BmPlayer {
    pthread_t   thread;
    atomic_int  stop;
    atomic_int  finished;   /* set when the thread has ended on its own */
    char       *path;
};

static const char *const PCM_DEVICES[] = { "default", "pipewire", "plughw:0,0", NULL };

static snd_pcm_t *open_pcm(unsigned rate, unsigned channels) {
    for (int i = 0; PCM_DEVICES[i]; i++) {
        snd_pcm_t *pcm = NULL;
        if (snd_pcm_open(&pcm, PCM_DEVICES[i], SND_PCM_STREAM_PLAYBACK, 0) < 0) continue;
        /* soft_resample=1, 400 ms latency: robust, and stop latency stays small */
        if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16, SND_PCM_ACCESS_RW_INTERLEAVED,
                               channels, rate, 1, 400000) < 0) {
            snd_pcm_close(pcm);
            continue;
        }
        return pcm;
    }
    return NULL;
}

/* returns 0 on success, -1 on unrecoverable pcm error */
static int write_all(BmPlayer *p, snd_pcm_t *pcm, const unsigned char *buf,
                     size_t bytes, unsigned channels) {
    snd_pcm_uframes_t frames = bytes / (2 * channels);
    while (frames > 0 && !atomic_load(&p->stop)) {
        snd_pcm_sframes_t n = snd_pcm_writei(pcm, buf, frames);
        if (n < 0) {
            n = snd_pcm_recover(pcm, (int)n, 1);
            if (n < 0) return -1;
            continue;
        }
        buf    += (size_t)n * 2 * channels;
        frames -= (snd_pcm_uframes_t)n;
    }
    return 0;
}

static void sleep_ms_interruptible(BmPlayer *p, int ms) {
    for (int i = 0; i < ms / 50 && !atomic_load(&p->stop); i++) usleep(50 * 1000);
}

static void player_body(BmPlayer *p) {
    int err = 0;
    mpg123_handle *mh = mpg123_new(NULL, &err);
    if (!mh) { fprintf(stderr, "audio: mpg123_new failed\n"); return; }
    mpg123_param(mh, MPG123_FLAGS, MPG123_QUIET | MPG123_GAPLESS, 0);

    /* allow every common sample rate, mono or stereo, 16-bit */
    const long *rates; size_t nrates;
    mpg123_rates(&rates, &nrates);
    mpg123_format_none(mh);
    for (size_t i = 0; i < nrates; i++)
        mpg123_format(mh, rates[i], MPG123_MONO | MPG123_STEREO, MPG123_ENC_SIGNED_16);

    if (mpg123_open(mh, p->path) != MPG123_OK) {
        fprintf(stderr, "audio: cannot open %s: %s\n", p->path, mpg123_strerror(mh));
        mpg123_delete(mh);
        return;
    }

    long rate = 0; int channels = 0, enc = 0;
    if (mpg123_getformat(mh, &rate, &channels, &enc) != MPG123_OK) {
        fprintf(stderr, "audio: cannot determine format of %s\n", p->path);
        mpg123_close(mh); mpg123_delete(mh);
        return;
    }

    size_t bufsize = mpg123_outblock(mh);
    unsigned char *buf = malloc(bufsize);
    snd_pcm_t *pcm = NULL;
    int warned = 0, bad_reads = 0;

    while (!atomic_load(&p->stop) && buf) {
        /* (re)open the device; the audio server may still be waking up */
        if (!pcm) {
            pcm = open_pcm((unsigned)rate, (unsigned)channels);
            if (!pcm) {
                if (!warned) { fprintf(stderr, "audio: no ALSA device could be opened, retrying...\n"); warned = 1; }
                sleep_ms_interruptible(p, 500);
                continue;
            }
        }

        size_t done = 0;
        int ret = mpg123_read(mh, buf, bufsize, &done);

        if (ret == MPG123_NEW_FORMAT) {
            mpg123_getformat(mh, &rate, &channels, &enc);
            if (pcm) { snd_pcm_drain(pcm); snd_pcm_close(pcm); pcm = NULL; }
            continue;
        }
        if (done > 0) {
            bad_reads = 0;
            if (write_all(p, pcm, buf, done, (unsigned)channels) < 0) {
                snd_pcm_close(pcm); pcm = NULL;     /* reopen on next iteration */
                sleep_ms_interruptible(p, 300);
            }
        }
        if (ret == MPG123_DONE || ret == MPG123_NEED_MORE) {
            if (mpg123_seek(mh, 0, SEEK_SET) < 0) break;   /* loop forever */
        } else if (ret != MPG123_OK && ret != MPG123_NEW_FORMAT && done == 0) {
            if (++bad_reads > 20 || mpg123_seek(mh, 0, SEEK_SET) < 0) {
                fprintf(stderr, "audio: decode error: %s\n", mpg123_strerror(mh));
                break;
            }
        }
    }

    if (pcm) { snd_pcm_drop(pcm); snd_pcm_close(pcm); }   /* drop = instant silence */
    free(buf);
    mpg123_close(mh);
    mpg123_delete(mh);
    return;
}

static void *player_thread(void *arg) {
    BmPlayer *p = arg;
    player_body(p);
    atomic_store(&p->finished, 1);
    return NULL;
}

BmPlayer *bm_player_start(const char *path) {
    if (!path || access(path, R_OK) != 0) {
        fprintf(stderr, "audio: sound file not readable: %s\n", path ? path : "(null)");
        return NULL;
    }
    mpg123_init();
    BmPlayer *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->path = strdup(path);
    atomic_init(&p->stop, 0);
    atomic_init(&p->finished, 0);
    if (pthread_create(&p->thread, NULL, player_thread, p) != 0) {
        free(p->path); free(p);
        return NULL;
    }
    return p;
}

void bm_player_stop(BmPlayer *p) {
    if (!p) return;
    atomic_store(&p->stop, 1);
    pthread_join(p->thread, NULL);
    free(p->path);
    free(p);
    mpg123_exit();
}

int bm_player_alive(BmPlayer *p) { return p && !atomic_load(&p->finished); }
