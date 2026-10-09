/* Plays the part of MPC: opens a playback PCM on "card 0", configures 4 ch S32 44.1 kHz, writes a known signal, and checks the
 * addin's output. argv[1] = work dir (trigger/done/output paths come from the settings the script wrote). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "t.h"

typedef struct { int fmt; unsigned ch, rate; int access; } params_t;
int snd_pcm_open(void **, const char *, int, int);
int snd_pcm_close(void *);
int snd_pcm_hw_params(void *, void *);
long snd_pcm_writei(void *, const void *, unsigned long);
extern long fake_accept;
extern int fake_rm_private;
extern unsigned char fake_rm_out[256];
extern volatile size_t fake_rm_out_n;
void fake_rm_feed(const unsigned char *, size_t);
int snd_rawmidi_open(void **, void **, const char *, int);
int snd_rawmidi_close(void *);
ssize_t snd_rawmidi_read(void *, void *, size_t);
ssize_t snd_rawmidi_write(void *, const void *, size_t);
const int32_t *fake_last_written(void);

static const char *dir;
static char trig[300], done[300], outdir[300];

static void write_block(void *pcm, uint32_t *t, uint32_t frames, int32_t amp) {
  static int32_t buf[4 * 1024];
  while (frames) {
    uint32_t k = frames < 1024 ? frames : 1024;
    for (uint32_t i = 0; i < k; i++) { buf[i * 4] = (int32_t)(*t + i) * amp; buf[i * 4 + 1] = -(int32_t)(*t + i) * amp; buf[i * 4 + 2] = 5; buf[i * 4 + 3] = 5; }
    CHECK_EQ(snd_pcm_writei(pcm, buf, k), (long)k);
    *t += k; frames -= k;
  }
}

static int wait_done(char *body, size_t n) {
  for (int i = 0; i < 100; i++) {
    FILE *f = fopen(done, "r");
    if (f) { size_t r = fread(body, 1, n - 1, f); body[r] = 0; fclose(f); return 0; }
    usleep(50000);
  }
  return -1;
}

static void touch(const char *p) { FILE *f = fopen(p, "w"); if (f) fclose(f); }

int main(int argc, char **argv) {
  if (argc < 3) return 2;
  dir = argv[1];
  int expect_active = !strcmp(argv[2], "mpc");
  snprintf(trig, sizeof trig, "%s/trigger", dir); snprintf(done, sizeof done, "%s/done", dir); snprintf(outdir, sizeof outdir, "%s/out", dir);

  void *pcm; CHECK_EQ(snd_pcm_open(&pcm, "c0", 0, 0), 0);
  params_t p = { 10, 4, 44100, 3 };
  /* the hook reads the params through libasound's getters, which the fake serves from this struct */
  CHECK_EQ(snd_pcm_hw_params(pcm, &p), 0);
  uint32_t t = 0;
  write_block(pcm, &t, 44100 * 2, 1);          /* 2 s of ramp */
  touch(trig);
  char body[600];
  if (!expect_active) {
    sleep(1);
    CHECK(access(trig, F_OK) == 0);            /* nothing consumed it: the addin did not start in a non-MPC process */
    CHECK(access(done, F_OK) != 0);
    snd_pcm_close(pcm);
    T_DONE("inert outside MPC");
  }
  CHECK_EQ(wait_done(body, sizeof body), 0);
  CHECK(!strncmp(body, "ok ", 3));
  CHECK(access(trig, F_OK) != 0);              /* marker consumed */
  const char *wav = body + 3; char path[700]; snprintf(path, sizeof path, "%s", wav); path[strcspn(path, "\n")] = 0;
  CHECK(strstr(path, outdir) == path);
  FILE *f = fopen(path, "rb"); CHECK(f != NULL);
  if (f) {
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    CHECK_EQ(sz, 44 + 2 * 44100 * 6);          /* the window is 5 s but only 2 s were played */
    uint8_t h[44]; CHECK_EQ(fread(h, 1, 44, f), 44);
    uint8_t s[6 * 3]; CHECK_EQ(fread(s, 1, sizeof s, f), sizeof s);
    /* frame 1 = ramp value 1 in the 32-bit sample = 0x00000001: below 24-bit resolution, so 0; frame 0 is 0 too */
    fseek(f, -6 * 4, SEEK_END); uint8_t e[6 * 4]; CHECK_EQ(fread(e, 1, sizeof e, f), sizeof e);
    uint32_t last = t - 1;                     /* the last ramp value, >> 8 */
    int32_t l = (int32_t)((uint32_t)e[18] | (uint32_t)e[19] << 8 | (uint32_t)e[20] << 16); if (l & 0x800000) l |= ~0xffffff;
    int32_t r = (int32_t)((uint32_t)e[21] | (uint32_t)e[22] << 8 | (uint32_t)e[23] << 16); if (r & 0x800000) r |= ~0xffffff;
    CHECK_EQ(l, (int32_t)(last >> 8)); CHECK_EQ(r, -(int32_t)(last >> 8) - ((last & 0xff) ? 1 : 0));
    fclose(f);
  }

  /* a second trigger saves again, to a new name */
  unlink(done); touch(trig);
  char body2[600]; CHECK_EQ(wait_done(body2, sizeof body2), 0);
  CHECK(!strncmp(body2, "ok ", 3)); CHECK(strcmp(body, body2) != 0);

  /* a short write is rewound, so a retry doesn't duplicate frames: 100 offered, 40 taken, then all 100 again */
  uint32_t t2 = 0; static int32_t tmp[4 * 100]; for (int i = 0; i < 100; i++) { tmp[i * 4] = 1000000 + i; tmp[i * 4 + 1] = 0; }
  fake_accept = 40; CHECK_EQ(snd_pcm_writei(pcm, tmp, 100), 40); fake_accept = -1;
  CHECK_EQ(snd_pcm_writei(pcm, tmp + 40 * 4, 60), 60);
  (void)t2;

  /* click: on, a save adds it to what the codec gets, not to the recording */
  char flag[400]; snprintf(flag, sizeof flag, "%s/click-on", dir);
  if (access(flag, F_OK) == 0) {
    int32_t silent[4 * 1024] = {0};
    unlink(done); touch(trig); CHECK_EQ(wait_done(body, sizeof body), 0);
    int heard = 0; for (int i = 0; i < 20 && !heard; i++) { snd_pcm_writei(pcm, silent, 1024); const int32_t *w = fake_last_written(); for (int k = 0; k < 1024; k++) if (w[k * 4] || w[k * 4 + 1]) heard = 1; }
    CHECK(heard);
    snprintf(path, sizeof path, "%s", body + 3); path[strcspn(path, "\n")] = 0;
    CHECK(strstr(path, outdir) == path);
  }
  char bflag[400]; snprintf(bflag, sizeof bflag, "%s/btn-on", dir);
  if (access(bflag, F_OK) == 0) {
    void *in, *out, *oin, *oout;
    fake_rm_private = 0; CHECK_EQ(snd_rawmidi_open(&oin, &oout, "hw:9,0", 0), 0);
    fake_rm_private = 1; CHECK_EQ(snd_rawmidi_open(&in, &out, "hw:1,0", 0), 0);
    /* MPC lights the Rec Arm LED (CC ch1, controller 93, value 3) */
    const unsigned char led_on[] = { 0xB0, 93, 3 };
    CHECK_EQ(snd_rawmidi_write(out, led_on, 3), 3);
    const unsigned char two[] = { 0x90, 93, 0x7f, 0x90, 93, 0x00, 0x90, 93, 0x7f, 0x90, 93, 0x00 };
    unsigned char got[32];
    unlink(done);
    /* a double press on an unrelated port does nothing */
    fake_rm_feed(two, sizeof two); CHECK_EQ(snd_rawmidi_read(oin, got, sizeof got), (ssize_t)sizeof two);
    usleep(400000); CHECK(access(done, F_OK) != 0);
    /* on the controller port the bytes come through unchanged and the second press starts a save */
    fake_rm_feed(two, sizeof two); CHECK_EQ(snd_rawmidi_read(in, got, sizeof got), (ssize_t)sizeof two);
    CHECK(!memcmp(got, two, sizeof two));
    CHECK_EQ(wait_done(body, sizeof body), 0); CHECK(!strncmp(body, "ok ", 3));
    for (int i = 0; i < 100 && fake_rm_out_n < 3 + 6 * 3 + 3; i++) usleep(20000);
    CHECK_EQ(fake_rm_out_n, 3 + 6 * 3 + 3);                    /* MPC's write, 3 blinks (on, off), then the real state back */
    if (fake_rm_out_n == 24) {
      for (int k = 0; k < 3; k++) { CHECK(!memcmp(fake_rm_out + 3 + k * 6, "\xB0\x5D\x03", 3)); CHECK(!memcmp(fake_rm_out + 6 + k * 6, "\xB0\x5D\x00", 3)); }
      CHECK(!memcmp(fake_rm_out + 21, "\xB0\x5D\x03", 3));
    }
    /* one press, or two slow ones, don't */
    unlink(done); fake_rm_feed(two, 6); CHECK_EQ(snd_rawmidi_read(in, got, sizeof got), 6);
    usleep(600000); fake_rm_feed(two + 6, 6); CHECK_EQ(snd_rawmidi_read(in, got, sizeof got), 6);
    usleep(400000); CHECK(access(done, F_OK) != 0);
    snd_rawmidi_close(in); snd_rawmidi_close(out); snd_rawmidi_close(oin); snd_rawmidi_close(oout);
  }
  CHECK_EQ(snd_pcm_close(pcm), 0);
  T_DONE("addin in MPC");
}
