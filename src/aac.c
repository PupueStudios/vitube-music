#include "aac.h"

#include <stdlib.h>
#include <string.h>

#include <neaacdec.h>

#include "util.h"

#define ADTS_CAP (64 * 1024)
#define MAX_CONSECUTIVE_ERRORS 40

struct aac {
    NeAACDecHandle dec;
    aac_pcm_fn cb;
    void *user;
    int inited;
    int errors;
    uint8_t *adts;
    size_t adts_len;
};

static void configure(aac_t *a) {
    NeAACDecConfigurationPtr c = NeAACDecGetCurrentConfiguration(a->dec);
    c->outputFormat = FAAD_FMT_16BIT;
    c->downMatrix = 1;
    c->dontUpSampleImplicitSBR = 0;
    NeAACDecSetConfiguration(a->dec, c);
}

aac_t *aac_new(aac_pcm_fn cb, void *user) {
    aac_t *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    a->dec = NeAACDecOpen();
    a->adts = malloc(ADTS_CAP);
    if (!a->dec || !a->adts) {
        aac_free(a);
        return NULL;
    }
    a->cb = cb;
    a->user = user;
    configure(a);
    return a;
}

void aac_free(aac_t *a) {
    if (!a) return;
    if (a->dec) NeAACDecClose(a->dec);
    free(a->adts);
    free(a);
}

static int decode_frame(aac_t *a, uint8_t *frame, int len) {
    NeAACDecFrameInfo fi;
    memset(&fi, 0, sizeof fi);
    void *out = NeAACDecDecode(a->dec, &fi, frame, (unsigned long)len);
    if (fi.error != 0 || !out || fi.samples == 0 || fi.channels == 0) {
        a->errors++;
        if (fi.error != 0 && a->errors < 4) log_msg("faad error: %s", NeAACDecGetErrorMessage(fi.error));
        return a->errors > MAX_CONSECUTIVE_ERRORS ? -1 : 0;
    }
    a->errors = 0;
    int frames = (int)(fi.samples / fi.channels);
    a->cb((const int16_t *)out, frames, fi.channels, (int)fi.samplerate, a->user);
    return 0;
}

void aac_reset_adts(aac_t *a) {
    a->adts_len = 0;
}

int aac_push_adts(aac_t *a, const uint8_t *data, size_t len) {
    while (len > 0) {
        size_t room = ADTS_CAP - a->adts_len;
        size_t take = len < room ? len : room;
        memcpy(a->adts + a->adts_len, data, take);
        a->adts_len += take;
        data += take;
        len -= take;

        size_t pos = 0;
        for (;;) {
            while (pos + 1 < a->adts_len && !(a->adts[pos] == 0xFF && (a->adts[pos + 1] & 0xF6) == 0xF0)) pos++;
            if (pos + 7 > a->adts_len) break;
            uint8_t *f = a->adts + pos;
            size_t flen = (size_t)(((f[3] & 3) << 11) | (f[4] << 3) | (f[5] >> 5));
            if (flen < 7) {
                pos++;
                continue;
            }
            if (pos + flen > a->adts_len) break;
            if (!a->inited) {
                unsigned long sr = 0;
                unsigned char ch = 0;
                long r = NeAACDecInit(a->dec, f, (unsigned long)flen, &sr, &ch);
                if (r < 0) {
                    pos += flen;
                    continue;
                }
                a->inited = 1;
            }
            if (decode_frame(a, f, (int)flen) < 0) return -1;
            pos += flen;
        }
        if (pos > 0) {
            memmove(a->adts, a->adts + pos, a->adts_len - pos);
            a->adts_len -= pos;
        }
        if (a->adts_len == ADTS_CAP) a->adts_len = 0;
    }
    return 0;
}
