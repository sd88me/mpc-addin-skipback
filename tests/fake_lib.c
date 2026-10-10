/* A stand-in for libasound: just enough of the PCM API for the addin's hooks to sit in front of. */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

typedef struct { int fmt; unsigned ch, rate; int access; } params_t;
static int card_of_pcm = 0;
static char last_written[1 << 16];
static size_t last_n;

int snd_pcm_open(void **pcmp, const char *name, int stream, int mode) { (void)stream; (void)mode; card_of_pcm = name[0] == 'c' ? atoi(name + 1) : 0; *pcmp = malloc(8); return 0; }
int snd_pcm_close(void *p) { free(p); return 0; }
size_t snd_pcm_info_sizeof(void) { return 64; }
int snd_pcm_info(void *pcm, void *info) { (void)pcm; memcpy(info, &card_of_pcm, sizeof card_of_pcm); return 0; }
int snd_pcm_info_get_card(const void *info) { int c; memcpy(&c, info, sizeof c); return c; }
int snd_pcm_hw_params(void *pcm, void *p) { (void)pcm; (void)p; return 0; }
int snd_pcm_hw_params_get_format(const params_t *p, int *v) { *v = p->fmt; return 0; }
int snd_pcm_hw_params_get_channels(const params_t *p, unsigned *v) { *v = p->ch; return 0; }
int snd_pcm_hw_params_get_rate(const params_t *p, unsigned *v, int *d) { *v = p->rate; *d = 0; return 0; }
int snd_pcm_hw_params_get_access(const params_t *p, int *v) { *v = p->access; return 0; }
/* what the "codec" received from the last write, for the test to inspect; a limit simulates a short write */
long fake_accept = -1;
long snd_pcm_writei(void *pcm, const void *buf, unsigned long n) {
  (void)pcm;
  size_t bytes = n * 16; if (bytes > sizeof last_written) bytes = sizeof last_written;
  memcpy(last_written, buf, bytes); last_n = n;
  return fake_accept >= 0 && (unsigned long)fake_accept < n ? fake_accept : (long)n;
}
long snd_pcm_writen(void *pcm, void **bufs, unsigned long n) {
  (void)pcm; (void)bufs; last_n = n; return (long)n;
}
const int32_t *fake_last_written(void) { return (const int32_t *)last_written; }

/* ---- rawmidi: a "Private" controller port (handle byte 0 = 1) and an unrelated one (0) ---------------------------- */
#include <sys/types.h>
int fake_rm_private = 1;                          /* what the next snd_rawmidi_open makes: 1 Force private, 2 MPC private, 0 another port */
static unsigned char rm_in_buf[64];
static size_t rm_in_n, rm_in_pos;
unsigned char fake_rm_out[256];
volatile size_t fake_rm_out_n;
void fake_rm_feed(const unsigned char *p, size_t n) { memcpy(rm_in_buf, p, n); rm_in_n = n; rm_in_pos = 0; }
int snd_rawmidi_open(void **in, void **out, const char *name, int mode) {
  (void)name; (void)mode;
  if (in) { *in = calloc(1, 8); *(char *)*in = (char)fake_rm_private; }
  if (out) { *out = calloc(1, 8); *(char *)*out = (char)fake_rm_private; }
  return 0;
}
int snd_rawmidi_close(void *h) { free(h); return 0; }
size_t snd_rawmidi_info_sizeof(void) { return 32; }
int snd_rawmidi_info(void *h, void *info) { memcpy(info, h, 1); return 0; }
const char *snd_rawmidi_info_get_subdevice_name(const void *info) { int k = *(const char *)info; return k == 1 ? "Akai Pro Force Private" : k == 2 ? "MPC Live Private" : "Other Port"; }
const char *snd_rawmidi_info_get_name(const void *info) { int k = *(const char *)info; return k == 1 ? "Akai Pro Force" : k == 2 ? "MPC Live" : "Other"; }
ssize_t snd_rawmidi_read(void *h, void *buf, size_t size) {
  (void)h;
  size_t n = rm_in_n - rm_in_pos < size ? rm_in_n - rm_in_pos : size;
  if (!n) return -11;
  memcpy(buf, rm_in_buf + rm_in_pos, n); rm_in_pos += n;
  return (ssize_t)n;
}
ssize_t snd_rawmidi_write(void *h, const void *buf, size_t n) {
  (void)h;
  size_t at = fake_rm_out_n;
  if (at + n > sizeof fake_rm_out) return -1;
  memcpy(fake_rm_out + at, buf, n);
  fake_rm_out_n = at + n;
  return (ssize_t)n;
}
