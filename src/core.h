/* core.h: the parts of the skipback addin that don't touch ALSA, so the host test can run them directly. */
#ifndef SB_CORE_H
#define SB_CORE_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define SB_MAX_WINDOW_SEC 60u
#define SB_MAX_RATE       96000u
#define SB_SLACK_SEC      2u    /* buffer kept beyond the window, so a snapshot copy can never be overrun by the audio thread */

typedef struct {
  int enabled;
  unsigned window_sec;          /* seconds saved per trigger, 1..SB_MAX_WINDOW_SEC */
  char output_dir[256];         /* "auto" picks one in sb_resolve_output_dir */
  char trigger[128];            /* marker file another program touches to start a save */
  char done[128];               /* file written after every save, with the result */
  char log[256];                /* "auto": skipback.log next to the library, "" none */
  int tap_card;                 /* ALSA card of the codec, -1 = ask /dev/snd/by-path/platform-sound */
  unsigned left, right;         /* channels of MPC's playback stream that hold main out */
  int click;                    /* mix a short click into main out when a save finishes (not recorded) */
  unsigned poll_ms;
} sb_cfg;

void sb_cfg_defaults(sb_cfg *c);
/* Parse key=value lines. Returns the number of bad lines, or -1 if the file can't be read (defaults stay). */
int sb_cfg_load(sb_cfg *c, const char *path, char *err, size_t errn);

/* Sample formats MPC may open the codec with. */
typedef enum { SB_FMT_UNKNOWN = 0, SB_FMT_S16, SB_FMT_S24_3, SB_FMT_S24_4, SB_FMT_S32, SB_FMT_F32 } sb_fmt;
enum { SB_ALSA_S16_LE = 2, SB_ALSA_S24_LE = 6, SB_ALSA_S32_LE = 10, SB_ALSA_FLOAT_LE = 14, SB_ALSA_S24_3LE = 32 };
sb_fmt sb_fmt_from_alsa(int alsa_fmt);
unsigned sb_fmt_bytes(sb_fmt f);
int32_t sb_fmt_get(const void *base, sb_fmt f, size_t idx);          /* sample idx as left-justified int32 */
void sb_fmt_add(void *base, sb_fmt f, size_t idx, int32_t v);        /* sample idx = saturate(sample + v) */

/* Rolling buffer of stereo int32 frames. One writer (MPC's audio thread, wait-free), any other thread reads snapshots. */
typedef struct {
  int32_t *data;                /* cap * 2 samples */
  uint32_t cap;                 /* frames */
  _Atomic uint64_t pos;         /* frames written since start/reset */
} sb_buf;

int sb_buf_init(sb_buf *b, uint32_t cap_frames);
void sb_buf_free(sb_buf *b);
void sb_buf_reset(sb_buf *b);
/* Append n frames from an interleaved (or, with bufs != NULL, per-channel) PCM buffer: channels l and r of ch. */
void sb_buf_push(sb_buf *b, const void *buf, void *const *bufs, sb_fmt f, unsigned ch, unsigned l, unsigned r, uint32_t n);
/* Take back the last k frames pushed (the real write took fewer than were offered). Writer thread only. */
void sb_buf_rewind(sb_buf *b, uint32_t k);
/* Copy up to `want` of the newest frames into out (stereo int32). Returns the frames copied. Frames the writer
 * may have overwritten during the copy are dropped from the start. */
uint32_t sb_buf_snapshot(sb_buf *b, int32_t *out, uint32_t want);

/* 24-bit stereo WAV, written to path.tmp and renamed. Returns 0 or -errno. */
int sb_wav_write(const char *path, const int32_t *frames, uint32_t n, unsigned rate);

/* "auto": /sdcard/Force Documents/Samples/Skipback if /sdcard/Force Documents exists, else /sdcard/Skipback. */
void sb_resolve_output_dir(const sb_cfg *c, char *out, size_t n);
/* out_dir/Skipback_YYYYMMDD_HHMMSS.wav (local time of `t`; a suffix _2, _3 ... if the file exists). */
void sb_make_path(const char *dir, long t, char *out, size_t n);
int sb_mkdir_p(const char *dir);

/* A short click, as +-int32 samples: sb_click_sample(i) for i in [0, SB_CLICK_FRAMES) at `rate`. */
#define SB_CLICK_MS 60u
int32_t sb_click_sample(unsigned i, unsigned rate);
uint32_t sb_click_frames(unsigned rate);

#endif
