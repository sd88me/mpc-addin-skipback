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
