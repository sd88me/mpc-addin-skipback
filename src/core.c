/* core.c: see core.h. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "core.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- settings ------------------------------------------------------------------------------- */

void sb_cfg_defaults(sb_cfg *c) {
  memset(c, 0, sizeof *c);
  c->enabled = 1;
  c->window_sec = 30;
  snprintf(c->output_dir, sizeof c->output_dir, "auto");
  snprintf(c->trigger, sizeof c->trigger, "/tmp/mpc-addin-skipback.trigger");
  snprintf(c->done, sizeof c->done, "/tmp/mpc-addin-skipback.done");
  snprintf(c->log, sizeof c->log, "auto");
  c->tap_card = -1;
  c->left = 0;
  c->right = 1;
  c->click = 0;
  c->poll_ms = 100;
}

static int parse_uint(const char *v, unsigned lo, unsigned hi, unsigned *out) {
  char *e;
  errno = 0;
  unsigned long x = strtoul(v, &e, 10);
  if (errno || e == v || *e || x < lo || x > hi) return -1;
  *out = (unsigned)x;
  return 0;
}

static int set_str(char *dst, size_t n, const char *v, int must_be_abs) {
  if (strlen(v) >= n) return -1;
  if (must_be_abs && v[0] && v[0] != '/') return -1;
  memcpy(dst, v, strlen(v) + 1);
  return 0;
}

int sb_cfg_load(sb_cfg *c, const char *path, char *err, size_t errn) {
  FILE *f = fopen(path, "re");
  if (!f) return -1;
  int bad = 0;
  char line[512];
  if (err && errn) err[0] = 0;
  while (fgets(line, sizeof line, f)) {
    char *s = line;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s || *s == '#' || *s == '\n' || *s == '\r') continue;
    char orig[sizeof line];
    snprintf(orig, sizeof orig, "%s", s);
    char *eq = strchr(s, '=');
    int rc = -1;
    if (eq) {
      *eq = 0;
      char *k = s, *v = eq + 1;
      size_t kl = strlen(k);
      while (kl && (k[kl - 1] == ' ' || k[kl - 1] == '\t')) k[--kl] = 0;
      while (*v == ' ' || *v == '\t') v++;
      size_t vl = strlen(v);
      while (vl && (v[vl - 1] == '\n' || v[vl - 1] == '\r' || v[vl - 1] == ' ' || v[vl - 1] == '\t')) v[--vl] = 0;
      unsigned u;
      if (!strcmp(k, "enabled")) { if (!parse_uint(v, 0, 1, &u)) { c->enabled = (int)u; rc = 0; } }
      else if (!strcmp(k, "window_sec")) { if (!parse_uint(v, 1, SB_MAX_WINDOW_SEC, &u)) { c->window_sec = u; rc = 0; } }
      else if (!strcmp(k, "output_dir")) rc = strcmp(v, "auto") ? set_str(c->output_dir, sizeof c->output_dir, v, 1) : set_str(c->output_dir, sizeof c->output_dir, v, 0);
      else if (!strcmp(k, "trigger")) rc = v[0] ? set_str(c->trigger, sizeof c->trigger, v, 1) : -1;
      else if (!strcmp(k, "done")) rc = v[0] ? set_str(c->done, sizeof c->done, v, 1) : -1;
      else if (!strcmp(k, "log")) rc = !strcmp(v, "auto") ? set_str(c->log, sizeof c->log, v, 0) : set_str(c->log, sizeof c->log, v, 1);
      else if (!strcmp(k, "tap_card")) { if (!strcmp(v, "auto")) { c->tap_card = -1; rc = 0; } else if (!parse_uint(v, 0, 31, &u)) { c->tap_card = (int)u; rc = 0; } }
      else if (!strcmp(k, "left")) { if (!parse_uint(v, 0, 31, &u)) { c->left = u; rc = 0; } }
      else if (!strcmp(k, "right")) { if (!parse_uint(v, 0, 31, &u)) { c->right = u; rc = 0; } }
      else if (!strcmp(k, "click")) { if (!parse_uint(v, 0, 1, &u)) { c->click = (int)u; rc = 0; } }
      else if (!strcmp(k, "poll_ms")) { if (!parse_uint(v, 20, 5000, &u)) { c->poll_ms = u; rc = 0; } }
    }
    if (rc) {
      if (!bad && err && errn) { orig[strcspn(orig, "\r\n")] = 0; snprintf(err, errn, "%s", orig); }
      bad++;
    }
  }
  fclose(f);
  return bad;
}

/* ---- sample formats ------------------------------------------------------------------------- */

sb_fmt sb_fmt_from_alsa(int a) {
  switch (a) {
    case SB_ALSA_S16_LE: return SB_FMT_S16;
    case SB_ALSA_S24_3LE: return SB_FMT_S24_3;
    case SB_ALSA_S24_LE: return SB_FMT_S24_4;
    case SB_ALSA_S32_LE: return SB_FMT_S32;
    case SB_ALSA_FLOAT_LE: return SB_FMT_F32;
    default: return SB_FMT_UNKNOWN;
  }
}

unsigned sb_fmt_bytes(sb_fmt f) {
  switch (f) { case SB_FMT_S16: return 2; case SB_FMT_S24_3: return 3; case SB_FMT_S24_4: case SB_FMT_S32: case SB_FMT_F32: return 4; default: return 0; }
}

static int32_t f2i(float x) {
  if (!(x == x)) return 0;
  if (x >= 1.0f) return INT32_MAX;
  if (x <= -1.0f) return INT32_MIN;
  return (int32_t)(x * 2147483648.0f);
}

int32_t sb_fmt_get(const void *base, sb_fmt f, size_t i) {
  const uint8_t *p = base;
  switch (f) {
    case SB_FMT_S16: { int16_t v; memcpy(&v, p + i * 2, 2); return (int32_t)((uint32_t)(uint16_t)v << 16); }
    case SB_FMT_S24_3: return (int32_t)((uint32_t)p[i * 3] << 8 | (uint32_t)p[i * 3 + 1] << 16 | (uint32_t)p[i * 3 + 2] << 24);
    case SB_FMT_S24_4: { int32_t v; memcpy(&v, p + i * 4, 4); return (int32_t)((uint32_t)v << 8); }
    case SB_FMT_S32: { int32_t v; memcpy(&v, p + i * 4, 4); return v; }
    case SB_FMT_F32: { float v; memcpy(&v, p + i * 4, 4); return f2i(v); }
    default: return 0;
  }
}

static int32_t sat(int64_t s) { return s > INT32_MAX ? INT32_MAX : s < INT32_MIN ? INT32_MIN : (int32_t)s; }

void sb_fmt_add(void *base, sb_fmt f, size_t i, int32_t v) {
  uint8_t *p = base;
  int32_t s = sat((int64_t)sb_fmt_get(base, f, i) + v);
  switch (f) {
    case SB_FMT_S16: { int16_t o = (int16_t)(s >> 16); memcpy(p + i * 2, &o, 2); break; }
    case SB_FMT_S24_3: p[i * 3] = (uint8_t)(s >> 8); p[i * 3 + 1] = (uint8_t)(s >> 16); p[i * 3 + 2] = (uint8_t)(s >> 24); break;
    case SB_FMT_S24_4: { int32_t o = s >> 8; memcpy(p + i * 4, &o, 4); break; }
    case SB_FMT_S32: memcpy(p + i * 4, &s, 4); break;
    case SB_FMT_F32: { float o = (float)s / 2147483648.0f; memcpy(p + i * 4, &o, 4); break; }
    default: break;
  }
}

/* ---- rolling buffer ------------------------------------------------------------------------- */

int sb_buf_init(sb_buf *b, uint32_t cap) {
  b->data = calloc((size_t)cap * 2, sizeof(int32_t));   /* untouched pages cost no RAM */
  b->cap = b->data ? cap : 0;
  atomic_store(&b->pos, 0);
  return b->data ? 0 : -1;
}

void sb_buf_free(sb_buf *b) { free(b->data); b->data = NULL; b->cap = 0; }
void sb_buf_reset(sb_buf *b) { atomic_store_explicit(&b->pos, 0, memory_order_release); }

void sb_buf_push(sb_buf *b, const void *buf, void *const *bufs, sb_fmt f, unsigned ch, unsigned l, unsigned r, uint32_t n) {
  if (!b->data || !b->cap || l >= ch || r >= ch) return;
  uint64_t pos = atomic_load_explicit(&b->pos, memory_order_relaxed);
  for (uint32_t i = 0; i < n; i++) {
    int32_t *d = &b->data[(size_t)((pos + i) % b->cap) * 2];
    if (bufs) {
      d[0] = bufs[l] ? sb_fmt_get(bufs[l], f, i) : 0;
      d[1] = bufs[r] ? sb_fmt_get(bufs[r], f, i) : 0;
    } else {
      d[0] = sb_fmt_get(buf, f, (size_t)i * ch + l);
      d[1] = sb_fmt_get(buf, f, (size_t)i * ch + r);
    }
  }
  atomic_store_explicit(&b->pos, pos + n, memory_order_release);
}

void sb_buf_rewind(sb_buf *b, uint32_t k) {
  uint64_t pos = atomic_load_explicit(&b->pos, memory_order_relaxed);
  atomic_store_explicit(&b->pos, k > pos ? 0 : pos - k, memory_order_release);
}

uint32_t sb_buf_snapshot(sb_buf *b, int32_t *out, uint32_t want) {
  if (!b->data || !b->cap) return 0;
  uint64_t end = atomic_load_explicit(&b->pos, memory_order_acquire);
  uint64_t have = end < b->cap ? end : b->cap;
  uint64_t n = want < have ? want : have;
  uint64_t start = end - n;
  for (uint64_t i = 0; i < n; i++) {
    const int32_t *s = &b->data[(size_t)((start + i) % b->cap) * 2];
    out[i * 2] = s[0];
    out[i * 2 + 1] = s[1];
  }
  uint64_t now = atomic_load_explicit(&b->pos, memory_order_acquire);
  uint64_t torn = now > b->cap && now - b->cap > start ? now - b->cap - start : 0;   /* overwritten while we copied */
  if (torn >= n) return 0;
  if (torn) { memmove(out, out + torn * 2, (size_t)(n - torn) * 2 * sizeof(int32_t)); n -= torn; }
  return (uint32_t)n;
}

/* ---- WAV ------------------------------------------------------------------------------------- */

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

int sb_wav_write(const char *path, const int32_t *frames, uint32_t n, unsigned rate) {
  char tmp[512];
  if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return -ENAMETOOLONG;
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return -errno;
  uint32_t bytes = n * 6;
  uint8_t h[44] = {0};
  memcpy(h, "RIFF", 4); put32(h + 4, 36 + bytes);
  memcpy(h + 8, "WAVEfmt ", 8); put32(h + 16, 16);
  put16(h + 20, 1); put16(h + 22, 2); put32(h + 24, rate); put32(h + 28, rate * 6);
  put16(h + 32, 6); put16(h + 34, 24);
  memcpy(h + 36, "data", 4); put32(h + 40, bytes);
  int rc = 0;
  uint8_t chunk[6 * 4096];
  if (write(fd, h, 44) != 44) rc = -errno ? -errno : -EIO;
  for (uint32_t i = 0; !rc && i < n;) {
    uint32_t k = n - i < 4096 ? n - i : 4096;
    for (uint32_t j = 0; j < k * 2; j++) {
      int32_t s = frames[(size_t)i * 2 + j];
      chunk[j * 3] = (uint8_t)(s >> 8); chunk[j * 3 + 1] = (uint8_t)(s >> 16); chunk[j * 3 + 2] = (uint8_t)(s >> 24);
    }
    const uint8_t *p = chunk;
    size_t left = (size_t)k * 6;
    while (left) {
      ssize_t w = write(fd, p, left);
      if (w < 0) { if (errno == EINTR) continue; rc = -errno; break; }
      p += w; left -= (size_t)w;
    }
    i += k;
  }
  if (!rc && fsync(fd) != 0) rc = -errno;
  if (close(fd) != 0 && !rc) rc = -errno;
  if (!rc && rename(tmp, path) != 0) rc = -errno;
  if (rc) unlink(tmp);
  return rc;
}

/* ---- paths ----------------------------------------------------------------------------------- */

void sb_resolve_output_dir(const sb_cfg *c, char *out, size_t n) {
  if (strcmp(c->output_dir, "auto")) { snprintf(out, n, "%s", c->output_dir); return; }
  struct stat st;
  if (stat("/sdcard/Force Documents", &st) == 0 && S_ISDIR(st.st_mode))
    snprintf(out, n, "/sdcard/Force Documents/Samples/Skipback");
  else
    snprintf(out, n, "/sdcard/Skipback");
}

int sb_mkdir_p(const char *dir) {
  char p[512];
  if (snprintf(p, sizeof p, "%s", dir) >= (int)sizeof p) return -ENAMETOOLONG;
  for (char *s = p + 1; ; s++) {
    if (*s == '/' || !*s) {
      char c = *s;
      *s = 0;
      if (mkdir(p, 0755) != 0 && errno != EEXIST) return -errno;
      *s = c;
      if (!c) break;
    }
  }
  return 0;
}

void sb_make_path(const char *dir, long t, char *out, size_t n) {
  time_t tt = (time_t)t;
  struct tm tm;
  localtime_r(&tt, &tm);
  char stamp[32];
  strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &tm);
  snprintf(out, n, "%s/Skipback_%s.wav", dir, stamp);
  for (int k = 2; k < 1000 && access(out, F_OK) == 0; k++) snprintf(out, n, "%s/Skipback_%s_%d.wav", dir, stamp, k);
}

/* ---- click ----------------------------------------------------------------------------------- */

uint32_t sb_click_frames(unsigned rate) { return rate * SB_CLICK_MS / 1000u; }

int32_t sb_click_sample(unsigned i, unsigned rate) {
  uint32_t n = sb_click_frames(rate);
  if (i >= n) return 0;
  double env = 1.0 - (double)i / n;                      /* linear fade, no click at the end */
  double s = sin(2.0 * M_PI * 1500.0 * i / rate) * env * 0.1;   /* about -20 dBFS peak */
  return (int32_t)(s * 2147483647.0);
}
