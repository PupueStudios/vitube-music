#include "hlsres.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ytm.h"

static const char *const HDR_VISION[] = {"User-Agent: " YTM_UA_VISION, NULL};

static int fetch_text(http_t *h, const char *url, buf_t *out, volatile int *cancel, long *status) {
    http_req_t rq;
    memset(&rq, 0, sizeof rq);
    rq.url = url;
    rq.headers = HDR_VISION;
    rq.range_start = -1;
    rq.range_end = -1;
    rq.timeout_s = 30;
    rq.compress = 1;
    rq.cancel = cancel;
    return http_fetch(h, &rq, out, 16u * 1024u * 1024u, status);
}

int hlsres_resolve(http_t *h, const char *video_id, hls_media_t *out, volatile int *cancel, char *err, size_t ec) {
    char *master = malloc(3072);
    if (!master) return -1;
    int rc = ytm_hls_master(h, video_id, master, 3072, cancel, err, ec);
    if (rc < 0) {
        free(master);
        return rc;
    }
    buf_t b;
    buf_init(&b);
    long status = 0;
    rc = fetch_text(h, master, &b, cancel, &status);
    if (rc != HTTP_OK || status != 200 || !b.data) {
        log_msg("hls master fetch rc=%d status=%ld", rc, status);
        snprintf(err, ec, "HLS manifest fetch failed (%ld)", status);
        buf_free(&b);
        free(master);
        return -2;
    }
    hls_cand_t cands[12];
    int nc = hls_pick_variants(b.data, master, cands, 12);
    buf_free(&b);
    log_msg("hls: %d candidate playlists", nc);
    int ok = 0;
    for (int i = 0; i < nc && !ok && !*cancel; i++) {
        buf_t pb;
        buf_init(&pb);
        rc = fetch_text(h, cands[i].uri, &pb, cancel, &status);
        if (rc != HTTP_OK || status != 200 || !pb.data) {
            log_msg("hls playlist %d fetch rc=%d status=%ld", i, rc, status);
            buf_free(&pb);
            continue;
        }
        hls_media_t m;
        int n = hls_parse_media(pb.data, cands[i].uri, &m);
        if (n <= 0 || m.has_map) {
            log_msg("hls playlist %d unusable (n=%d map=%d)", i, n, m.has_map);
            hls_media_free(&m);
            continue;
        }
        *out = m;
        log_msg("hls: using playlist %d, %d segments, %.1fs", i, n, m.total);
        ok = 1;
    }
    hls_cands_free(cands, nc);
    free(master);
    if (!ok) {
        snprintf(err, ec, "No usable HLS audio");
        return -3;
    }
    return 0;
}
