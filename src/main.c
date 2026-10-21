#include <jansson.h>
#include <stdio.h>
#include <string.h>

#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>

#include "jobs.h"
#include "net.h"
#include "player.h"
#include "store.h"
#include "ui.h"
#include "util.h"
#include "ytm.h"

#define CONFIG_PATH DATA_DIR "/config.json"

int _newlib_heap_size_user = 40 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize = 1 * 1024 * 1024;

typedef struct {
    char hl[16];
    char gl[16];
    int insecure;
} config_t;

static void load_config(config_t *c) {
    snprintf(c->hl, sizeof c->hl, "en");
    snprintf(c->gl, sizeof c->gl, "US");
    c->insecure = 0;
    json_error_t e;
    json_t *root = json_load_file(CONFIG_PATH, 0, &e);
    if (!root) {
        json_t *def = json_pack("{s:s,s:s,s:b}", "hl", c->hl, "gl", c->gl, "insecure_tls", 0);
        json_dump_file(def, CONFIG_PATH, JSON_INDENT(2));
        json_decref(def);
        return;
    }
    const char *s = json_string_value(json_object_get(root, "hl"));
    if (s && s[0]) str_set(c->hl, sizeof c->hl, s);
    s = json_string_value(json_object_get(root, "gl"));
    if (s && s[0]) str_set(c->gl, sizeof c->gl, s);
    c->insecure = json_is_true(json_object_get(root, "insecure_tls"));
    json_decref(root);
}

int main(void) {
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    log_init();
    config_t cfg;
    load_config(&cfg);
    log_msg("config: hl=%s gl=%s insecure=%d", cfg.hl, cfg.gl, cfg.insecure);

    if (ui_init() < 0) {
        log_msg("ui init failed");
        sceKernelExitProcess(1);
    }
    if (net_init(cfg.insecure) < 0) log_msg("net init failed");
    ytm_configure(cfg.hl, cfg.gl);
    store_init();
    if (player_init() < 0) log_msg("player init failed");
    jobs_init();

    while (!ui_frame()) {
    }

    player_shutdown();
    jobs_shutdown();
    ui_shutdown();
    sceKernelExitProcess(0);
    return 0;
}
