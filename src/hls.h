#ifndef HLS_H
#define HLS_H

typedef struct {
    char *uri;
    double dur;
    int owned;
} hls_seg_t;

typedef struct {
    char *text;
    hls_seg_t *segs;
    int n;
    double total;
    int has_map;
} hls_media_t;

typedef struct {
    char *uri;
    int prio;
} hls_cand_t;

int hls_pick_variants(const char *master, const char *base, hls_cand_t *out, int max);
void hls_cands_free(hls_cand_t *c, int n);
int hls_parse_media(char *text, const char *base, hls_media_t *out);
void hls_media_free(hls_media_t *m);
int hls_find_segment(const hls_media_t *m, int ms, int *seg_start_ms);

#endif
