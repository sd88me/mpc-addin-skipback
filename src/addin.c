/* addin.c: skipback for MPC OS standalone devices, an LD_PRELOAD addin that runs inside MPC.
 *
 *   1. hooks snd_pcm_open / hw_params / close to find MPC's playback stream on the codec card;
 *   2. hooks snd_pcm_writei / writen to copy main out (two channels of that stream) into a rolling buffer;
 *   3. hooks snd_rawmidi_open/read/write/close on the controller's "Private" port: a double press of `button`
 *      (default Rec Arm) starts a save, and the same port takes the LED change that says it has finished;
 *   4. a thread watches for the trigger file (or the button) and saves the last window_sec seconds as a 24-bit WAV, then writes
 *      the done file (and, if enabled, mixes a short click into main out).
 *
 * Nothing runs unless the process is MPC. The audio thread only copies samples and never locks, allocates or
 * makes a syscall. The thread is started from the first playback hw_params, not from the library constructor:
 * a thread started that early crashed MPC while its own libraries were still initialising.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "core.h"

#define EXPORT __attribute__((visibility("default")))

typedef struct _snd_pcm snd_pcm_t;
typedef struct _snd_pcm_hw_params snd_pcm_hw_params_t;
typedef struct _snd_pcm_info snd_pcm_info_t;
typedef unsigned long snd_pcm_uframes_t;
typedef long snd_pcm_sframes_t;

typedef struct _snd_rawmidi snd_rawmidi_t;
typedef struct _snd_rawmidi_info snd_rawmidi_info_t;

enum { PCM_PLAYBACK = 0, ACCESS_RW_INTERLEAVED = 3, ACCESS_RW_NONINTERLEAVED = 4, MAX_CH = 32 };

static int (*real_open)(snd_pcm_t **, const char *, int, int);
static int (*real_close)(snd_pcm_t *);
static int (*real_hw_params)(snd_pcm_t *, snd_pcm_hw_params_t *);
static snd_pcm_sframes_t (*real_writei)(snd_pcm_t *, const void *, snd_pcm_uframes_t);
static snd_pcm_sframes_t (*real_writen)(snd_pcm_t *, void **, snd_pcm_uframes_t);
static int (*real_rm_open)(snd_rawmidi_t **, snd_rawmidi_t **, const char *, int);
static int (*real_rm_close)(snd_rawmidi_t *);
static ssize_t (*real_rm_read)(snd_rawmidi_t *, void *, size_t);
static ssize_t (*real_rm_write)(snd_rawmidi_t *, const void *, size_t);
static size_t (*a_info_sizeof)(void);
static int (*a_info)(snd_pcm_t *, snd_pcm_info_t *);
static int (*a_info_get_card)(const snd_pcm_info_t *);
static int (*a_get_format)(const snd_pcm_hw_params_t *, int *);
static int (*a_get_channels)(const snd_pcm_hw_params_t *, unsigned *);
static int (*a_get_rate)(const snd_pcm_hw_params_t *, unsigned *, int *);
static int (*a_get_access)(const snd_pcm_hw_params_t *, int *);

static struct {
  int active;                         /* running inside MPC and enabled */
  sb_cfg cfg;
  int card;
  char dir[128];
  _Atomic(snd_pcm_t *) pcm;           /* the playback stream being tapped */
  _Atomic int ok;                     /* format is supported: tap it */
  sb_fmt fmt;
  unsigned ch, rate;
  int interleaved;
  sb_buf buf;
  _Atomic(snd_rawmidi_t *) rm_in, rm_out;   /* the controller's Private port, once MPC has opened it */
  sb_btn btn;                         /* read hook only (MPC's MIDI input thread) */
  sb_learn learn;                     /* ditto; used while learning */
  int learning;                       /* read hook: the next double-pressed button becomes `button` */
  int is_force;                       /* the controller port's name says Force */
  _Atomic int learned_req;            /* note + 1 of a button just learned, for the save thread to confirm and store */
  char learned_path[200];
  sb_led led;                         /* guarded by wr_lock */
  pthread_mutex_t wr_lock;
  _Atomic int btn_req;                /* the button asked for a save */
  _Atomic uint32_t click_pos;         /* frames of click still to mix, counted up; 0 = idle */
  _Atomic int click_req;
  pthread_once_t once;
  int log_fd;
} G = { .once = PTHREAD_ONCE_INIT, .log_fd = -1, .wr_lock = PTHREAD_MUTEX_INITIALIZER };

/* ---- log ------------------------------------------------------------------------------------- */

static void logf_(const char *fmt, ...) {
  if (G.log_fd < 0) return;
  struct stat st;
  if (fstat(G.log_fd, &st) == 0 && st.st_size > 256 * 1024 && ftruncate(G.log_fd, 0) != 0) return;
  char line[512];
  time_t t = time(NULL);
  struct tm tm;
  localtime_r(&t, &tm);
  int n = (int)strftime(line, sizeof line, "%Y-%m-%d %H:%M:%S ", &tm);
  va_list ap;
  va_start(ap, fmt);
  int m = vsnprintf(line + n, sizeof line - (size_t)n - 1, fmt, ap);
  va_end(ap);
  if (m < 0) return;
  n += m < (int)(sizeof line - (size_t)n - 1) ? m : (int)(sizeof line - (size_t)n - 2);
  line[n++] = '\n';
  ssize_t w = write(G.log_fd, line, (size_t)n);
  (void)w;
}

/* ---- setup ----------------------------------------------------------------------------------- */

static void resolve_reals(void) {
  *(void **)&real_open = dlsym(RTLD_NEXT, "snd_pcm_open");
  *(void **)&real_close = dlsym(RTLD_NEXT, "snd_pcm_close");
  *(void **)&real_hw_params = dlsym(RTLD_NEXT, "snd_pcm_hw_params");
  *(void **)&real_writei = dlsym(RTLD_NEXT, "snd_pcm_writei");
  *(void **)&real_writen = dlsym(RTLD_NEXT, "snd_pcm_writen");
  *(void **)&real_rm_open = dlsym(RTLD_NEXT, "snd_rawmidi_open");
  *(void **)&real_rm_close = dlsym(RTLD_NEXT, "snd_rawmidi_close");
  *(void **)&real_rm_read = dlsym(RTLD_NEXT, "snd_rawmidi_read");
  *(void **)&real_rm_write = dlsym(RTLD_NEXT, "snd_rawmidi_write");
  *(void **)&a_info_sizeof = dlsym(RTLD_NEXT, "snd_pcm_info_sizeof");
  *(void **)&a_info = dlsym(RTLD_NEXT, "snd_pcm_info");
  *(void **)&a_info_get_card = dlsym(RTLD_NEXT, "snd_pcm_info_get_card");
  *(void **)&a_get_format = dlsym(RTLD_NEXT, "snd_pcm_hw_params_get_format");
  *(void **)&a_get_channels = dlsym(RTLD_NEXT, "snd_pcm_hw_params_get_channels");
  *(void **)&a_get_rate = dlsym(RTLD_NEXT, "snd_pcm_hw_params_get_rate");
  *(void **)&a_get_access = dlsym(RTLD_NEXT, "snd_pcm_hw_params_get_access");
}

static int exe_is_mpc(void) {
  char p[256];
  ssize_t n = readlink("/proc/self/exe", p, sizeof p - 1);
  if (n <= 0) return 0;
  p[n] = 0;
  const char *b = strrchr(p, '/');
  return strcmp(b ? b + 1 : p, "MPC") == 0;
}

/* The folder this .so was loaded from (settings and log live there), "" if unknown. */
static void addin_dir(char *out, size_t n) {
  out[0] = 0;
  FILE *f = fopen("/proc/self/maps", "re");
  if (!f) return;
  unsigned long me = (unsigned long)(uintptr_t)&addin_dir;
  char line[512];
  while (fgets(line, sizeof line, f)) {
    unsigned long a, b;
    char *path = strchr(line, '/');
    if (!path || sscanf(line, "%lx-%lx", &a, &b) != 2 || me < a || me >= b) continue;
    path[strcspn(path, "\n")] = 0;
    char *slash = strrchr(path, '/');
    if (slash) *slash = 0;
    if (strlen(path) < n) memcpy(out, path, strlen(path) + 1);
    break;
  }
  fclose(f);
}

static int resolve_card(void) {
  if (G.cfg.tap_card >= 0) return G.cfg.tap_card;
  char t[64];
  ssize_t n = readlink("/dev/snd/by-path/platform-sound", t, sizeof t - 1);
  if (n <= 0) return -1;
  t[n] = 0;
  const char *s = strstr(t, "controlC");
  return s ? atoi(s + 8) : -1;
}

__attribute__((constructor)) static void sb_ctor(void) {
  resolve_reals();
  if (!exe_is_mpc()) return;
  addin_dir(G.dir, sizeof G.dir);
  char defpath[192];
  snprintf(defpath, sizeof defpath, "%s/skipback.conf", G.dir[0] ? G.dir : ".");
  const char *path = getenv("MPC_SKIPBACK_CONF");
  if (!path || !*path) path = defpath;
  sb_cfg_defaults(&G.cfg);
  char err[160];
  int bad = sb_cfg_load(&G.cfg, path, err, sizeof err);
  if (!strcmp(G.cfg.log, "auto")) snprintf(G.cfg.log, sizeof G.cfg.log, "%s/skipback.log", G.dir[0] ? G.dir : ".");
  if (G.cfg.log[0]) G.log_fd = open(G.cfg.log, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (bad < 0) logf_("settings %s unreadable, using defaults", path);
  else if (bad) logf_("settings %s: %d problem(s), first: %s", path, bad, err);
  if (!G.cfg.enabled) { logf_("disabled in settings"); return; }
  if (G.cfg.left == G.cfg.right) { logf_("left and right are the same channel; disabled"); return; }
  G.card = resolve_card();
  const char *lp = getenv("MPC_SKIPBACK_LEARNED");
  if (lp && *lp) snprintf(G.learned_path, sizeof G.learned_path, "%s", lp);
  else snprintf(G.learned_path, sizeof G.learned_path, "%s/button.learned", G.dir[0] ? G.dir : ".");
  if (G.cfg.button_mode == SB_BTN_LEARN) {
    FILE *lf = fopen(G.learned_path, "re");
    unsigned note = 0;
    if (lf && fscanf(lf, "%u", &note) == 1 && note >= 1 && note <= 127) { G.cfg.button = note; G.cfg.button_mode = SB_BTN_NUM; logf_("button %u, as learned earlier (delete %s to learn again)", note, G.learned_path); }
    else { G.learning = 1; logf_("learning: double-press the button to use"); }
    if (lf) fclose(lf);
  }
  sb_btn_init(&G.btn, G.cfg.button, G.cfg.double_ms);
  sb_learn_init(&G.learn, G.cfg.double_ms);
  sb_led_init(&G.led);
  G.active = 1;
  logf_("active: codec card %d, window %u s, channels %u/%u, double press within %u ms", G.card, G.cfg.window_sec, G.cfg.left, G.cfg.right, G.cfg.double_ms);
}

/* ---- save thread ----------------------------------------------------------------------------- */

static void write_done(const char *status, const char *detail) {
  char tmp[160], body[640];
  snprintf(body, sizeof body, "%s %s\n", status, detail);
  if (snprintf(tmp, sizeof tmp, "%s.tmp", G.cfg.done) >= (int)sizeof tmp) return;
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return;
  ssize_t w = write(fd, body, strlen(body));
  (void)w;
  close(fd);
  if (rename(tmp, G.cfg.done) != 0) unlink(tmp);
}

static int save_now(void) {
  unsigned rate = G.rate;
  uint32_t want = G.cfg.window_sec * rate;
  int32_t *snap = rate ? malloc((size_t)want * 2 * sizeof(int32_t)) : NULL;
  if (!snap) { write_done("error", rate ? "out of memory" : "no audio yet"); logf_("save: %s", rate ? "out of memory" : "no audio stream yet"); return 0; }
  uint32_t n = sb_buf_snapshot(&G.buf, snap, want);
  char dir[256], path[512];
  const char *settings = getenv("MPC_SKIPBACK_SETTINGS");
  if (!settings || !*settings) settings = access("/data/Settings/MPC/MPC.settings", R_OK) == 0 ? "/data/Settings/MPC/MPC.settings"
                                                                                     : "/media/az01-internal/Settings/MPC/MPC.settings";
  const char *mounts = getenv("MPC_SKIPBACK_MOUNTS");
  if (!mounts || !*mounts) mounts = "/proc/mounts";
  sb_resolve_output_dir(&G.cfg, settings, mounts, dir, sizeof dir);
  int rc = n ? sb_mkdir_p(dir) : -ENODATA;
  if (!rc) {
    sb_make_path(dir, (long)time(NULL), path, sizeof path);
    rc = sb_wav_write(path, snap, n, rate);
  }
  free(snap);
  if (rc) {
    char d[96];
    snprintf(d, sizeof d, "%s", strerror(-rc));
    write_done("error", d);
    logf_("save failed: %s (dir %s)", d, dir);
    return 0;
  }
  logf_("saved %u frames (%.1f s) to %s", n, (double)n / rate, path);
  write_done("ok", path);
  if (G.cfg.click) atomic_store(&G.click_req, 1);
  return 1;
}

static void sleep_ms(unsigned ms) {
  struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
  nanosleep(&ts, NULL);
}

/* LED feedback: on when asked for, and by default only on a Force (the one model the LED protocol is known for). */
static int led_enabled(void) { return G.cfg.led == 1 || (G.cfg.led == -1 && G.is_force); }

/* One LED change on the controller: control change, channel 1, controller = button, value = state. */
static void led_set(snd_rawmidi_t *out, unsigned button, unsigned value) {
  unsigned char m[3] = { 0xB0, (unsigned char)button, (unsigned char)value };
  pthread_mutex_lock(&G.wr_lock);
  if (real_rm_write && out == atomic_load(&G.rm_out)) { ssize_t w = real_rm_write(out, m, 3); (void)w; }
  pthread_mutex_unlock(&G.wr_lock);
}

/* Blink the LED n times, then put back the value MPC last gave it (looked up again at the end: MPC may have changed it). */
static void led_blink(unsigned n, unsigned ms) {
  snd_rawmidi_t *out = atomic_load(&G.rm_out);
  unsigned btn = G.cfg.led_button >= 0 ? (unsigned)G.cfg.led_button : G.cfg.button;
  if (!led_enabled() || !n || !out || btn > 127) return;
  for (unsigned i = 0; i < n; i++) {
    led_set(out, btn, G.cfg.led_on);
    sleep_ms(ms);
    led_set(out, btn, 0);
    sleep_ms(ms);
  }
  pthread_mutex_lock(&G.wr_lock);
  unsigned v = G.led.val[btn];
  pthread_mutex_unlock(&G.wr_lock);
  led_set(out, btn, v);
}

/* Fast blinks say the trigger registered. They run beside the save, so the save is not held up by them. */
static void *ack_thread(void *arg) {
  (void)arg;
  led_blink(G.cfg.led_fast_blinks, G.cfg.led_fast_ms);
  return NULL;
}

static void *save_thread(void *arg) {
  (void)arg;
  for (;;) {
    sleep_ms(G.cfg.poll_ms);
    int learned = atomic_exchange(&G.learned_req, 0);
    if (learned) {                                 /* a button was just learned: remember it, and say so */
      FILE *lf = fopen(G.learned_path, "we");
      if (lf) { fprintf(lf, "%d\n", learned - 1); fclose(lf); } else logf_("could not write %s", G.learned_path);
      logf_("learned button %d", learned - 1);
      atomic_store(&G.click_req, 1);
      led_blink(G.cfg.led_blinks, G.cfg.led_ms);
    }
    int want = atomic_exchange(&G.btn_req, 0);
    if (unlink(G.cfg.trigger) == 0) want = 1;     /* consuming the marker is the trigger: two presses are two saves */
    if (!want) continue;
    pthread_t ack;
    int acking = pthread_create(&ack, NULL, ack_thread, NULL) == 0;
    int saved = save_now();
    if (acking) pthread_join(ack, NULL);
    if (saved) led_blink(G.cfg.led_blinks, G.cfg.led_ms);
  }
  return NULL;
}

static void start_thread(void) {
  pthread_t t;
  if (pthread_create(&t, NULL, save_thread, NULL) == 0) pthread_detach(t);
  else logf_("could not start the save thread");
}

/* ---- PCM tracking (MPC setup threads) -------------------------------------------------------- */

EXPORT int snd_pcm_open(snd_pcm_t **pcmp, const char *name, int stream, int mode) {
  if (!real_open) resolve_reals();
  if (!real_open) return -ENOSYS;
  int r = real_open(pcmp, name, stream, mode);
  if (r < 0 || !G.active || G.card < 0 || stream != PCM_PLAYBACK || !a_info_sizeof || !a_info || !a_info_get_card) return r;
  snd_pcm_info_t *info = alloca(a_info_sizeof());
  memset(info, 0, a_info_sizeof());
  if (a_info(*pcmp, info) < 0 || a_info_get_card(info) != G.card) return r;
  atomic_store(&G.ok, 0);
  atomic_store(&G.pcm, *pcmp);
  logf_("tapping playback PCM '%s' (card %d)", name ? name : "?", G.card);
  return r;
}

EXPORT int snd_pcm_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t *params) {
  if (!real_hw_params) resolve_reals();
  if (!real_hw_params) return -ENOSYS;
  int mine = G.active && pcm && pcm == atomic_load(&G.pcm);
  if (mine) atomic_store(&G.ok, 0);
  int r = real_hw_params(pcm, params);
  if (!mine || r < 0 || !a_get_format || !a_get_channels || !a_get_rate || !a_get_access) return r;
  int fmt = -1, access = -1, dir = 0;
  unsigned ch = 0, rate = 0;
  a_get_format(params, &fmt);
  a_get_channels(params, &ch);
  a_get_rate(params, &rate, &dir);
  a_get_access(params, &access);
  sb_fmt f = sb_fmt_from_alsa(fmt);
  if (f == SB_FMT_UNKNOWN || ch <= G.cfg.left || ch <= G.cfg.right || ch > MAX_CH || rate < 8000 || rate > SB_MAX_RATE ||
      (access != ACCESS_RW_INTERLEAVED && access != ACCESS_RW_NONINTERLEAVED)) {
    logf_("playback: unsupported format %d / %u ch / %u Hz / access %d; not tapping", fmt, ch, rate, access);
    return r;
  }
  if (!G.buf.data && sb_buf_init(&G.buf, (SB_MAX_WINDOW_SEC + SB_SLACK_SEC) * SB_MAX_RATE) != 0) {
    logf_("could not allocate the rolling buffer");
    return r;
  }
  G.fmt = f; G.ch = ch; G.rate = rate; G.interleaved = access == ACCESS_RW_INTERLEAVED;
  sb_buf_reset(&G.buf);   /* a new stream: the old audio may be another rate */
  atomic_store_explicit(&G.ok, 1, memory_order_release);
  logf_("playback: %u ch, format %d, %u Hz, %s", ch, fmt, rate, G.interleaved ? "interleaved" : "non-interleaved");
  pthread_once(&G.once, start_thread);
  return r;
}

EXPORT int snd_pcm_close(snd_pcm_t *pcm) {
  if (!real_close) resolve_reals();
  if (!real_close) return -ENOSYS;
  if (G.active && pcm && pcm == atomic_load(&G.pcm)) {
    atomic_store(&G.ok, 0);
    atomic_store(&G.pcm, NULL);
  }
  return real_close(pcm);
}

/* ---- audio thread ---------------------------------------------------------------------------- */

/* Mix the click into what MPC is about to play, after the tap has copied it, so it is never recorded. */
static void mix_click(void *buf, void *const *bufs, uint32_t n) {
  if (atomic_exchange_explicit(&G.click_req, 0, memory_order_relaxed)) atomic_store_explicit(&G.click_pos, 1, memory_order_relaxed);
  uint32_t p = atomic_load_explicit(&G.click_pos, memory_order_relaxed);
  if (!p) return;
  uint32_t total = sb_click_frames(G.rate), i = 0;
  for (; i < n && p - 1 + i < total; i++) {
    int32_t s = sb_click_sample(p - 1 + i, G.rate);
    unsigned chs[2] = { G.cfg.left, G.cfg.right };
    for (int k = 0; k < 2; k++) {
      if (bufs) { if (bufs[chs[k]]) sb_fmt_add(bufs[chs[k]], G.fmt, i, s); }
      else sb_fmt_add(buf, G.fmt, (size_t)i * G.ch + chs[k], s);
    }
  }
  atomic_store_explicit(&G.click_pos, p - 1 + i >= total ? 0 : p + i, memory_order_relaxed);
}

EXPORT snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm, const void *buf, snd_pcm_uframes_t n) {
  if (!real_writei) resolve_reals();
  if (!real_writei) return -ENOSYS;
  int tap = G.active && pcm == atomic_load_explicit(&G.pcm, memory_order_relaxed) && atomic_load_explicit(&G.ok, memory_order_acquire) &&
            G.interleaved && buf && n;
  if (tap) {
    sb_buf_push(&G.buf, buf, NULL, G.fmt, G.ch, G.cfg.left, G.cfg.right, (uint32_t)n);
    if (G.cfg.click || atomic_load_explicit(&G.click_req, memory_order_relaxed) || atomic_load_explicit(&G.click_pos, memory_order_relaxed)) mix_click((void *)buf, NULL, (uint32_t)n);
  }
  snd_pcm_sframes_t r = real_writei(pcm, buf, n);
  if (tap && r < (snd_pcm_sframes_t)n) sb_buf_rewind(&G.buf, (uint32_t)(r < 0 ? n : n - (snd_pcm_uframes_t)r));
  return r;
}

EXPORT snd_pcm_sframes_t snd_pcm_writen(snd_pcm_t *pcm, void **bufs, snd_pcm_uframes_t n) {
  if (!real_writen) resolve_reals();
  if (!real_writen) return -ENOSYS;
  int tap = G.active && pcm == atomic_load_explicit(&G.pcm, memory_order_relaxed) && atomic_load_explicit(&G.ok, memory_order_acquire) &&
            !G.interleaved && bufs && n;
  if (tap) {
    sb_buf_push(&G.buf, NULL, bufs, G.fmt, G.ch, G.cfg.left, G.cfg.right, (uint32_t)n);
    if (G.cfg.click || atomic_load_explicit(&G.click_req, memory_order_relaxed) || atomic_load_explicit(&G.click_pos, memory_order_relaxed)) mix_click(NULL, bufs, (uint32_t)n);
  }
  snd_pcm_sframes_t r = real_writen(pcm, bufs, n);
  if (tap && r < (snd_pcm_sframes_t)n) sb_buf_rewind(&G.buf, (uint32_t)(r < 0 ? n : n - (snd_pcm_uframes_t)r));
  return r;
}

/* ---- controller MIDI (the "Private" rawmidi port) ---------------------------------------------- */

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Same test as the button remap shim: the port's name or subdevice name has "Private" in it. The names go to `names`
 * (device / subdevice) for the log and for telling a Force from the rest. */
static int is_private(snd_rawmidi_t *h, char *names, size_t nn) {
  size_t (*info_sizeof)(void) = dlsym(RTLD_DEFAULT, "snd_rawmidi_info_sizeof");
  int (*info)(snd_rawmidi_t *, snd_rawmidi_info_t *) = dlsym(RTLD_DEFAULT, "snd_rawmidi_info");
  const char *(*subname)(const snd_rawmidi_info_t *) = dlsym(RTLD_DEFAULT, "snd_rawmidi_info_get_subdevice_name");
  const char *(*devname)(const snd_rawmidi_info_t *) = dlsym(RTLD_DEFAULT, "snd_rawmidi_info_get_name");
  int found = 0;
  if (names && nn) names[0] = 0;
  if (!info_sizeof || !info || !subname || !devname) return 0;
  snd_rawmidi_info_t *ri = calloc(1, info_sizeof());
  if (ri && info(h, ri) == 0) {
    const char *sn = subname(ri), *dn = devname(ri);
    found = (sn && strstr(sn, "Private")) || (dn && strstr(dn, "Private"));
    if (names && nn) snprintf(names, nn, "'%s' / '%s'", dn ? dn : "", sn ? sn : "");
  }
  free(ri);
  return found;
}

EXPORT int snd_rawmidi_open(snd_rawmidi_t **in, snd_rawmidi_t **out, const char *name, int mode) {
  if (!real_rm_open) resolve_reals();
  if (!real_rm_open) return -ENOSYS;
  int r = real_rm_open(in, out, name, mode);
  if (r != 0 || !G.active || (G.cfg.button_mode == SB_BTN_NUM && !G.cfg.button)) return r;
  char names[160];
  if (in && *in && is_private(*in, names, sizeof names)) {
    G.is_force = strstr(names, "Force") != NULL;
    if (G.cfg.button_mode == SB_BTN_AUTO) { G.cfg.button = G.is_force ? SB_FORCE_BUTTON : SB_MPC_BUTTON; G.cfg.button_mode = SB_BTN_NUM; }
    sb_btn_init(&G.btn, G.cfg.button, G.cfg.double_ms);
    sb_learn_init(&G.learn, G.cfg.double_ms);
    atomic_store(&G.rm_in, *in);
    if (G.learning) logf_("controller input hooked (%s %s): double-press the button you want to use", name ? name : "?", names);
    else logf_("controller input hooked (%s %s): button %u, double press within %u ms, led %s", name ? name : "?", names, G.cfg.button, G.cfg.double_ms, led_enabled() ? "on" : "off");
  }
  if (out && *out && is_private(*out, names, sizeof names)) {
    pthread_mutex_lock(&G.wr_lock);
    sb_led_init(&G.led);                 /* MPC sends every LED again after opening */
    pthread_mutex_unlock(&G.wr_lock);
    atomic_store(&G.rm_out, *out);
    logf_("controller output hooked (%s)", name ? name : "?");
  }
  return r;
}

EXPORT int snd_rawmidi_close(snd_rawmidi_t *h) {
  if (!real_rm_close) resolve_reals();
  if (!real_rm_close) return -ENOSYS;
  if (G.active && h) {
    if (h == atomic_load(&G.rm_in)) atomic_store(&G.rm_in, NULL);
    if (h == atomic_load(&G.rm_out)) {
      pthread_mutex_lock(&G.wr_lock);      /* wait for a flash write in flight */
      atomic_store(&G.rm_out, NULL);
      pthread_mutex_unlock(&G.wr_lock);
    }
  }
  return real_rm_close(h);
}

static void log_hex(const char *what, const unsigned char *p, size_t n) {
  char b[96];
  size_t k = 0;
  for (size_t i = 0; i < n && i < 16 && k + 4 < sizeof b; i++) k += (size_t)snprintf(b + k, sizeof b - k, "%02X ", p[i]);
  logf_("%s %s", what, b);
}

/* Reads pass through untouched; the bytes are only looked at. */
EXPORT ssize_t snd_rawmidi_read(snd_rawmidi_t *h, void *buf, size_t size) {
  if (!real_rm_read) resolve_reals();
  if (!real_rm_read) return -ENOSYS;
  ssize_t r = real_rm_read(h, buf, size);
  if (r > 0 && G.active && h == atomic_load_explicit(&G.rm_in, memory_order_relaxed)) {
    if (G.cfg.midi_log && r <= 8) log_hex("controller in:", buf, (size_t)r);
    if (G.learning) {
      int note = sb_learn_feed(&G.learn, buf, (size_t)r, now_ms());
      if (note >= 0) {                      /* this double press only teaches; the next one saves */
        G.cfg.button = (unsigned)note;
        G.learning = 0;
        sb_btn_init(&G.btn, G.cfg.button, G.cfg.double_ms);
        atomic_store(&G.learned_req, note + 1);
      }
    } else if (G.cfg.button && sb_btn_feed(&G.btn, buf, (size_t)r, now_ms())) {
      logf_("button %u pressed twice", G.cfg.button);
      atomic_store(&G.btn_req, 1);
    }
  }
  return r;
}

/* Writes pass through; LED changes are noted so a flash can put the real state back. */
EXPORT ssize_t snd_rawmidi_write(snd_rawmidi_t *h, const void *buf, size_t size) {
  if (!real_rm_write) resolve_reals();
  if (!real_rm_write) return -ENOSYS;
  if (!G.active || h != atomic_load_explicit(&G.rm_out, memory_order_relaxed)) return real_rm_write(h, buf, size);
  pthread_mutex_lock(&G.wr_lock);
  ssize_t r = real_rm_write(h, buf, size);
  if (r > 0 && sb_led_feed(&G.led, buf, (size_t)r) && G.cfg.midi_log)
    logf_("led %d = %d", G.led.changed_btn, G.led.changed_val);
  pthread_mutex_unlock(&G.wr_lock);
  return r;
}
