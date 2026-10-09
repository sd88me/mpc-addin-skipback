/* addin.c: skipback for MPC OS standalone devices, an LD_PRELOAD addin that runs inside MPC.
 *
 *   1. hooks snd_pcm_open / hw_params / close to find MPC's playback stream on the codec card;
 *   2. hooks snd_pcm_writei / writen to copy main out (two channels of that stream) into a rolling buffer;
 *   3. a thread watches for the trigger file and saves the last window_sec seconds as a 24-bit WAV, then writes
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

enum { PCM_PLAYBACK = 0, ACCESS_RW_INTERLEAVED = 3, ACCESS_RW_NONINTERLEAVED = 4, MAX_CH = 32 };

static int (*real_open)(snd_pcm_t **, const char *, int, int);
static int (*real_close)(snd_pcm_t *);
static int (*real_hw_params)(snd_pcm_t *, snd_pcm_hw_params_t *);
static snd_pcm_sframes_t (*real_writei)(snd_pcm_t *, const void *, snd_pcm_uframes_t);
static snd_pcm_sframes_t (*real_writen)(snd_pcm_t *, void **, snd_pcm_uframes_t);
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
  _Atomic uint32_t click_pos;         /* frames of click still to mix, counted up; 0 = idle */
  _Atomic int click_req;
  pthread_once_t once;
  int log_fd;
} G = { .once = PTHREAD_ONCE_INIT, .log_fd = -1 };

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
  G.active = 1;
  logf_("active: codec card %d, window %u s, channels %u/%u", G.card, G.cfg.window_sec, G.cfg.left, G.cfg.right);
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

static void save_now(void) {
  unsigned rate = G.rate;
  uint32_t want = G.cfg.window_sec * rate;
  int32_t *snap = rate ? malloc((size_t)want * 2 * sizeof(int32_t)) : NULL;
  if (!snap) { write_done("error", rate ? "out of memory" : "no audio yet"); logf_("save: %s", rate ? "out of memory" : "no audio stream yet"); return; }
  uint32_t n = sb_buf_snapshot(&G.buf, snap, want);
  char dir[256], path[512];
  sb_resolve_output_dir(&G.cfg, dir, sizeof dir);
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
    return;
  }
  logf_("saved %u frames (%.1f s) to %s", n, (double)n / rate, path);
  write_done("ok", path);
  if (G.cfg.click) atomic_store(&G.click_req, 1);
}

static void *save_thread(void *arg) {
  (void)arg;
  for (;;) {
    struct timespec ts = { 0, (long)G.cfg.poll_ms * 1000000L };
    nanosleep(&ts, NULL);
    if (unlink(G.cfg.trigger) == 0) save_now();   /* consuming the marker is the trigger: two presses are two saves */
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
    if (G.cfg.click) mix_click((void *)buf, NULL, (uint32_t)n);
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
    if (G.cfg.click) mix_click(NULL, bufs, (uint32_t)n);
  }
  snd_pcm_sframes_t r = real_writen(pcm, bufs, n);
  if (tap && r < (snd_pcm_sframes_t)n) sb_buf_rewind(&G.buf, (uint32_t)(r < 0 ? n : n - (snd_pcm_uframes_t)r));
  return r;
}
