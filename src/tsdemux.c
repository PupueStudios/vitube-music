#include "tsdemux.h"

#include <string.h>

void ts_init(ts_demux_t *t, ts_audio_fn cb, void *user) {
    memset(t, 0, sizeof *t);
    t->cb = cb;
    t->user = user;
    t->pmt_pid = -1;
    t->aud_pid = -1;
}

static void parse_pat(ts_demux_t *t, const uint8_t *p, int len) {
    if (len < 2) return;
    int ptr = p[0];
    if (1 + ptr + 8 > len) return;
    const uint8_t *s = p + 1 + ptr;
    int avail = len - 1 - ptr;
    if (s[0] != 0x00) return;
    int slen = ((s[1] & 0x0F) << 8) | s[2];
    int end = 3 + slen - 4;
    if (end > avail) end = avail;
    for (int i = 8; i + 4 <= end; i += 4) {
        int prog = (s[i] << 8) | s[i + 1];
        int pid = ((s[i + 2] & 0x1F) << 8) | s[i + 3];
        if (prog != 0) {
            t->pmt_pid = pid;
            t->pat_done = 1;
            return;
        }
    }
}

static void parse_pmt(ts_demux_t *t, const uint8_t *p, int len) {
    if (len < 2) return;
    int ptr = p[0];
    if (1 + ptr + 12 > len) return;
    const uint8_t *s = p + 1 + ptr;
    int avail = len - 1 - ptr;
    if (s[0] != 0x02) return;
    int slen = ((s[1] & 0x0F) << 8) | s[2];
    int end = 3 + slen - 4;
    if (end > avail) end = avail;
    int pil = ((s[10] & 0x0F) << 8) | s[11];
    int i = 12 + pil;
    while (i + 5 <= end) {
        int type = s[i];
        int pid = ((s[i + 1] & 0x1F) << 8) | s[i + 2];
        int esl = ((s[i + 3] & 0x0F) << 8) | s[i + 4];
        if (type == 0x0F || type == 0x11) {
            t->aud_pid = pid;
            return;
        }
        i += 5 + esl;
    }
}

static void packet(ts_demux_t *t) {
    const uint8_t *b = t->pkt;
    if (b[0] != 0x47) return;
    int pusi = (b[1] >> 6) & 1;
    int pid = ((b[1] & 0x1F) << 8) | b[2];
    int afc = (b[3] >> 4) & 3;
    int p = 4;
    if (afc & 2) p += 1 + b[4];
    if (!(afc & 1) || p >= 188) return;
    const uint8_t *pl = b + p;
    int plen = 188 - p;
    if (pid == 0) {
        if (!t->pat_done && pusi) parse_pat(t, pl, plen);
    } else if (pid == t->pmt_pid) {
        if (t->aud_pid < 0 && pusi) parse_pmt(t, pl, plen);
    } else if (pid == t->aud_pid) {
        if (pusi) {
            if (plen < 9 || pl[0] != 0 || pl[1] != 0 || pl[2] != 1) return;
            int hl = 9 + pl[8];
            if (hl < plen) t->cb(pl + hl, (size_t)(plen - hl), t->user);
        } else {
            t->cb(pl, (size_t)plen, t->user);
        }
    }
}

void ts_feed(ts_demux_t *t, const uint8_t *data, size_t len) {
    while (len > 0) {
        if (t->fill == 0) {
            while (len > 0 && data[0] != 0x47) {
                data++;
                len--;
            }
            if (len == 0) return;
        }
        size_t need = (size_t)(188 - t->fill);
        size_t take = len < need ? len : need;
        memcpy(t->pkt + t->fill, data, take);
        t->fill += (int)take;
        data += take;
        len -= take;
        if (t->fill == 188) {
            packet(t);
            t->fill = 0;
        }
    }
}
