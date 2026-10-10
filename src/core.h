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
  unsigned button;              /* controller button (channel 1 note) whose double press saves; 0 = no button */
  unsigned double_ms;           /* two presses within this many ms are a double press */
  int led;                      /* flash an LED on the controller when a save finishes */
  int led_button;               /* the LED (a button number); -1 = the same as `button` */
  unsigned led_on;              /* LED value for the lit half of a blink */
  unsigned led_blinks;          /* slow blinks when the WAV is written */
  unsigned led_ms;              /* length of each half of a slow blink */
  unsigned led_fast_blinks;     /* fast blinks when the trigger is registered, 0 = none */
  unsigned led_fast_ms;
  int midi_log;                 /* log the controller's button presses and LED changes (to learn the values) */
} sb_cfg;

void sb_cfg_defaults(sb_cfg *c);
/* Parse key=value lines. Returns the number of bad lines, or -1 if the file can't be read (defaults stay). */
int sb_cfg_load(sb_cfg *c, const char *path, char *err, size_t errn);

/* ---- controller MIDI -------------------------------------------------------------------------- */

/* Splits a MIDI byte stream (running status, SysEx and realtime bytes handled; chunks may end anywhere) into
 * 3-byte channel messages and calls cb(status, d1, d2, ctx) for each; 1-data-byte messages are skipped. */
typedef struct { uint8_t run, d1; int have, in_sysex; } sb_mparse;
typedef void (*sb_msg_cb)(uint8_t status, uint8_t d1, uint8_t d2, void *ctx);
void sb_mparse_feed(sb_mparse *s, const uint8_t *p, size_t n, sb_msg_cb cb, void *ctx);

/* Double-press detector for one button: note-on (channel 1, velocity > 0) of `note` twice within `dbl_ms`. */
typedef struct { sb_mparse mp; unsigned note, dbl_ms; int has_last; long long last_ms, now_ms; int doubles; } sb_btn;
void sb_btn_init(sb_btn *b, unsigned note, unsigned dbl_ms);
/* Feeds bytes read from the controller at time now_ms. Returns the number of double presses seen in them. */
int sb_btn_feed(sb_btn *b, const uint8_t *p, size_t n, long long now_ms);

/* LED state as MPC last wrote it: control change channel 1, controller = button, value = LED state. */
typedef struct { sb_mparse mp; uint8_t val[128]; int changed_btn, changed_val, changed; } sb_led;
void sb_led_init(sb_led *l);
/* Feeds bytes MPC wrote to the controller. Returns the number of LED values that changed; the last change is in
 * changed_btn / changed_val. */
int sb_led_feed(sb_led *l, const uint8_t *p, size_t n);

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

/* Where saves go. An explicit output_dir is used as it is. "auto" takes the first of:
 *   1. a mounted USB drive or SSD (/dev/sd*) that is writable,
 *   2. a mounted external SD card (/dev/mmcblk1 and up; mmcblk0 is the internal flash) that is writable,
 *   3. the device's own Samples folder (what "samples" gives),
 * each as <drive>/<Force|MPC|APC> Documents/Samples/Skipback if the drive has that Samples folder, else <drive>/Skipback.
 * "samples" is only 3: <Samples folder>/Skipback, found from MPC's settings file: a browser shortcut (folder1.. in
 * MPC.settings) that ends in /Samples, else the Samples folder next to the Projects folder of a recent project, else
 * /sdcard/<Force|MPC|APC> Documents/Samples, else /data/Skipback. Only folders that exist are taken.
 * mounts_path is /proc/mounts (a file in the test). */
void sb_resolve_output_dir(const sb_cfg *c, const char *settings_path, const char *mounts_path, char *out, size_t n);
/* out_dir/Skipback_YYYYMMDD_HHMMSS.wav (local time of `t`; a suffix _2, _3 ... if the file exists). */
void sb_make_path(const char *dir, long t, char *out, size_t n);
int sb_mkdir_p(const char *dir);

/* A short click, as +-int32 samples: sb_click_sample(i) for i in [0, SB_CLICK_FRAMES) at `rate`. */
#define SB_CLICK_MS 60u
int32_t sb_click_sample(unsigned i, unsigned rate);
uint32_t sb_click_frames(unsigned rate);

#endif
