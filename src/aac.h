#ifndef AAC_H
#define AAC_H

#include <stddef.h>
#include <stdint.h>

typedef void (*aac_pcm_fn)(const int16_t *pcm, int frames, int channels, int rate, void *user);

typedef struct aac aac_t;

aac_t *aac_new(aac_pcm_fn cb, void *user);
void aac_free(aac_t *a);
void aac_reset_adts(aac_t *a);
int aac_push_adts(aac_t *a, const uint8_t *data, size_t len);

#endif
