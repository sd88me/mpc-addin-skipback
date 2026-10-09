#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../src/core.h"
#include "t.h"

static void write_file(const char *p, const char *s) { FILE *f = fopen(p, "w"); fputs(s, f); fclose(f); }

int main(void) {
  char dir[] = "/tmp/sbcoreXXXXXX";
  CHECK(mkdtemp(dir) != NULL);
  char p[600];

  /* settings */
  sb_cfg c; sb_cfg_defaults(&c);
  CHECK_EQ(c.window_sec, 30); CHECK_EQ(c.click, 0); CHECK(!strcmp(c.output_dir, "auto"));
  snprintf(p, sizeof p, "%s/skipback.conf", dir);
  write_file(p, "# c\nwindow_sec = 45\nclick=1\nleft=2\nright=3\noutput_dir=/x/y\ntap_card=auto\n");
  char err[100];
  CHECK_EQ(sb_cfg_load(&c, p, err, sizeof err), 0);
  CHECK_EQ(c.window_sec, 45); CHECK_EQ(c.click, 1); CHECK_EQ(c.left, 2); CHECK_EQ(c.right, 3); CHECK(!strcmp(c.output_dir, "/x/y"));
  write_file(p, "window_sec=0\nwindow_sec=61\nbogus=1\nnoequals\noutput_dir=relative\nwindow_sec=10\n");
  sb_cfg_defaults(&c);
  CHECK_EQ(sb_cfg_load(&c, p, err, sizeof err), 5);
  CHECK(!strcmp(err, "window_sec=0")); CHECK_EQ(c.window_sec, 10);
  CHECK_EQ(sb_cfg_load(&c, "/nonexistent/x", err, sizeof err), -1);

  /* formats */
  uint8_t b[16] = {0};
  int16_t s16[2] = { 0x1234, -2 }; CHECK_EQ(sb_fmt_get(s16, SB_FMT_S16, 0), 0x12340000); CHECK_EQ(sb_fmt_get(s16, SB_FMT_S16, 1), -2 * 65536);
  int32_t v = 0x11223344; CHECK_EQ(sb_fmt_get(&v, SB_FMT_S32, 0), 0x11223344);
  int32_t v24 = 0x00123456; CHECK_EQ(sb_fmt_get(&v24, SB_FMT_S24_4, 0), 0x12345600);
  uint8_t t3[3] = { 0x56, 0x34, 0x12 }; CHECK_EQ(sb_fmt_get(t3, SB_FMT_S24_3, 0), 0x12345600);
  float fl[3] = { 0.5f, 2.0f, -2.0f }; CHECK_EQ(sb_fmt_get(fl, SB_FMT_F32, 0), 0x40000000); CHECK_EQ(sb_fmt_get(fl, SB_FMT_F32, 1), INT32_MAX); CHECK_EQ(sb_fmt_get(fl, SB_FMT_F32, 2), INT32_MIN);
  for (int f = SB_FMT_S16; f <= SB_FMT_F32; f++) {   /* add into zero, read back */
    memset(b, 0, sizeof b);
    sb_fmt_add(b, (sb_fmt)f, 1, 0x20000000);
    int32_t g = sb_fmt_get(b, (sb_fmt)f, 1);
    CHECK(g > 0x1fff0000 && g <= 0x20000000);
    CHECK_EQ(sb_fmt_get(b, (sb_fmt)f, 0), 0);
  }
  int32_t big = INT32_MAX - 5; sb_fmt_add(&big, SB_FMT_S32, 0, 100); CHECK_EQ(big, INT32_MAX);
  CHECK_EQ(sb_fmt_from_alsa(SB_ALSA_S32_LE), SB_FMT_S32); CHECK_EQ(sb_fmt_from_alsa(99), SB_FMT_UNKNOWN);

  /* rolling buffer: interleaved 4ch, channels 0/1, wrap-around */
  sb_buf bf; CHECK_EQ(sb_buf_init(&bf, 1000), 0);
  int32_t in[4 * 300];
  uint32_t total = 0;
  for (int round = 0; round < 7; round++) {          /* 2100 frames into a 1000-frame buffer */
    for (int i = 0; i < 300; i++) { in[i * 4] = (int32_t)(total + i); in[i * 4 + 1] = -(int32_t)(total + i); in[i * 4 + 2] = 7; in[i * 4 + 3] = 7; }
    sb_buf_push(&bf, in, NULL, SB_FMT_S32, 4, 0, 1, 300);
    total += 300;
  }
  int32_t *out = malloc(1000 * 2 * 4);
  uint32_t n = sb_buf_snapshot(&bf, out, 400);
  CHECK_EQ(n, 400);
  for (uint32_t i = 0; i < n; i++) { CHECK_EQ(out[i * 2], total - 400 + i); CHECK_EQ(out[i * 2 + 1], -(int64_t)(total - 400 + i)); }
  CHECK_EQ(sb_buf_snapshot(&bf, out, 5000), 1000);
  CHECK_EQ(out[0], total - 1000);
  sb_buf_rewind(&bf, 100);
  n = sb_buf_snapshot(&bf, out, 1);
  CHECK_EQ(out[0], total - 101);
  sb_buf_reset(&bf);
  CHECK_EQ(sb_buf_snapshot(&bf, out, 10), 0);
  /* non-interleaved */
  int32_t L[10], R[10]; void *planes[3] = { L, R, NULL };
  for (int i = 0; i < 10; i++) { L[i] = i + 1; R[i] = 100 + i; }
  sb_buf_push(&bf, NULL, planes, SB_FMT_S32, 3, 0, 1, 10);
  CHECK_EQ(sb_buf_snapshot(&bf, out, 10), 10); CHECK_EQ(out[2], 2); CHECK_EQ(out[3], 101);
  sb_buf_push(&bf, in, NULL, SB_FMT_S32, 4, 0, 4, 5);   /* channel out of range: ignored */
  CHECK_EQ(sb_buf_snapshot(&bf, out, 100), 10);
  sb_buf_free(&bf);
  free(out);

  /* wav */
  int32_t fr[6] = { 0x12345600, -0x12345600, 0x00000100, 0x7fffff00, 0, INT32_MIN };
  snprintf(p, sizeof p, "%s/a.wav", dir);
  CHECK_EQ(sb_wav_write(p, fr, 3, 44100), 0);
  CHECK(access("a.wav.tmp", F_OK) != 0);
  FILE *f = fopen(p, "rb"); uint8_t w[44 + 18]; CHECK_EQ(fread(w, 1, sizeof w, f), 44 + 18); CHECK_EQ(fgetc(f), EOF); fclose(f);
  CHECK(!memcmp(w, "RIFF", 4) && !memcmp(w + 8, "WAVEfmt ", 8) && !memcmp(w + 36, "data", 4));
  CHECK_EQ(w[4] | w[5] << 8, 36 + 18); CHECK_EQ(w[22], 2); CHECK_EQ(w[24] | w[25] << 8, 44100); CHECK_EQ(w[34], 24); CHECK_EQ(w[40], 18);
  CHECK(w[44] == 0x56 && w[45] == 0x34 && w[46] == 0x12);   /* 0x123456 little-endian */
  CHECK(w[62 - 3] == 0x00 && w[62 - 2] == 0x00 && w[62 - 1] == 0x80);   /* INT32_MIN = 0x800000 */
  CHECK(sb_wav_write("/nonexistent/dir/x.wav", fr, 3, 44100) < 0);

  /* paths */
  char path[600], d2[600];
  snprintf(d2, sizeof d2, "%s/a/b/c", dir);
  CHECK_EQ(sb_mkdir_p(d2), 0); CHECK_EQ(sb_mkdir_p(d2), 0);
  sb_make_path(d2, 1700000000, path, sizeof path);
  CHECK(strstr(path, "/Skipback_2023") != NULL && strstr(path, ".wav") != NULL);
  write_file(path, "x");
  char path2[600]; sb_make_path(d2, 1700000000, path2, sizeof path2);
  CHECK(strcmp(path, path2) != 0 && strstr(path2, "_2.wav") != NULL);

  /* click: starts at zero, ends at zero, bounded, silent after */
  uint32_t cf = sb_click_frames(44100); CHECK_EQ(cf, 2646);
  int32_t mx = 0; for (uint32_t i = 0; i < cf; i++) { int32_t s = sb_click_sample(i, 44100); if (s > mx) mx = s; }
  CHECK(mx > 100000000 && mx < 300000000); CHECK_EQ(sb_click_sample(0, 44100), 0); CHECK_EQ(sb_click_sample(cf, 44100), 0);

  /* controller MIDI: double press of button 93 */
  {
    sb_btn bt; sb_btn_init(&bt, 93, 350);
    const uint8_t press[] = { 0x90, 93, 0x7f, 0x90, 93, 0x00 };
    CHECK_EQ(sb_btn_feed(&bt, press, sizeof press, 1000), 0);
    CHECK_EQ(sb_btn_feed(&bt, press, sizeof press, 1200), 1);          /* second press 200 ms later */
    CHECK_EQ(sb_btn_feed(&bt, press, sizeof press, 1300), 0);          /* a third press starts over */
    CHECK_EQ(sb_btn_feed(&bt, press, sizeof press, 2000), 0);          /* too slow */
    CHECK_EQ(sb_btn_feed(&bt, press, sizeof press, 2100), 1);
    const uint8_t other[] = { 0x90, 92, 0x7f, 0x90, 92, 0x7f };
    CHECK_EQ(sb_btn_feed(&bt, other, sizeof other, 3000), 0);          /* another button */
    /* one byte at a time, running status, realtime and a SysEx in between */
    const uint8_t odd[] = { 0xF0, 1, 2, 3, 0xF7, 0x90, 93, 0x7f, 0xF8, 93, 0x7f, 0xB0, 5, 6 };
    sb_btn_init(&bt, 93, 350);
    int n = 0; for (size_t i = 0; i < sizeof odd; i++) n += sb_btn_feed(&bt, odd + i, 1, 5000 + (long long)i);
    CHECK_EQ(n, 1);
    const uint8_t vel0[] = { 0x90, 93, 0, 0x90, 93, 0, 0x80, 93, 64, 0x91, 93, 0x7f, 0x91, 93, 0x7f };
    sb_btn_init(&bt, 93, 350);
    CHECK_EQ(sb_btn_feed(&bt, vel0, sizeof vel0, 9000), 0);            /* releases and other channels don't count */
    sb_led lv; sb_led_init(&lv);
    const uint8_t cc[] = { 0xB0, 93, 3, 0xB0, 93, 3, 0xB0, 91, 1, 0xB1, 92, 5, 0xC0, 7 };
    CHECK_EQ(sb_led_feed(&lv, cc, sizeof cc), 2);
    CHECK_EQ(lv.val[93], 3); CHECK_EQ(lv.val[91], 1); CHECK_EQ(lv.val[92], 0);
    CHECK_EQ(sb_led_feed(&lv, cc, 3), 0);
    const uint8_t off[] = { 93, 0 };                                    /* running status */
    CHECK_EQ(sb_led_feed(&lv, off, sizeof off), 1); CHECK_EQ(lv.val[93], 0); CHECK_EQ(lv.changed_btn, 93);
    sb_cfg c2; sb_cfg_defaults(&c2); CHECK_EQ(c2.button, 93); CHECK_EQ(c2.led_button, -1);
  }

  snprintf(p, sizeof p, "rm -rf %s", dir); if (system(p)) {}
  T_DONE("core");
}
