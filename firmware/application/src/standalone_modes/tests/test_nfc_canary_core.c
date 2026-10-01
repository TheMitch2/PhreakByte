/*
 * Native unit tests for nfc_canary_core. Build + run:  make -C tests
 */
#include "../nfc_canary_core.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static nc_cfg_t default_cfg(void) { nc_cfg_t c; nc_cfg_parse(NULL, 0, &c); return c; }

/* ---- classification ---------------------------------------------------- */
static void test_classify(void) {
    uint8_t reqa = 0x26, wupa = 0x52, magic = 0x40, sel[2] = {0x93, 0x20};
    uint8_t sel2 = 0x95, sel3 = 0x97, auth[2] = {0x60, 0x04}, rats[2] = {0xE0, 0x80};
    CHECK(nc_classify(NULL, 8)  == NC_LV_NONE);
    CHECK(nc_classify(&reqa, 3) == NC_LV_NONE);
    CHECK(nc_classify(&reqa, 6) == NC_LV_NONE);
    CHECK(nc_classify(&reqa, 7) == NC_LV_POLL);
    CHECK(nc_classify(&wupa, 7) == NC_LV_POLL);
    CHECK(nc_classify(&magic, 7) == NC_LV_ENGAGE);   /* non-standard 7-bit frame */
    CHECK(nc_classify(sel, 16)  == NC_LV_SELECT);
    CHECK(nc_classify(&sel2, 16) == NC_LV_SELECT);
    CHECK(nc_classify(&sel3, 16) == NC_LV_SELECT);
    CHECK(nc_classify(auth, 32) == NC_LV_ENGAGE);
    CHECK(nc_classify(rats, 32) == NC_LV_ENGAGE);
}

/* ---- config ------------------------------------------------------------ */
static void test_cfg(void) {
    nc_cfg_t c;
    CHECK(nc_cfg_parse(NULL, 0, &c));
    CHECK(c.min_level == NC_LV_FIELD && c.cooldown_s == NC_COOLDOWN_DEFAULT_S);
    CHECK(c.flags == (NC_CFG_FLAG_BLE_ALERT | NC_CFG_FLAG_BLE_END));

    uint8_t ok[4] = {1, NC_LV_SELECT, 30, NC_CFG_FLAG_BLE_ALERT};
    CHECK(nc_cfg_parse(ok, 4, &c));
    CHECK(c.min_level == NC_LV_SELECT && c.cooldown_s == 30 && c.flags == NC_CFG_FLAG_BLE_ALERT);

    uint8_t bad_ver[4] = {2, 0, 10, 3}, bad_lv[4] = {1, 4, 10, 3};
    uint8_t bad_cd[4]  = {1, 0, 0, 3},  bad_fl[4] = {1, 0, 10, 0x04};
    uint8_t any[5] = {1, 0, 10, 3, 0};
    CHECK(!nc_cfg_parse(bad_ver, 4, &c));
    CHECK(c.cooldown_s == NC_COOLDOWN_DEFAULT_S);       /* left at defaults */
    CHECK(!nc_cfg_parse(bad_lv, 4, &c));
    CHECK(!nc_cfg_parse(bad_cd, 4, &c));
    CHECK(!nc_cfg_parse(bad_fl, 4, &c));
    CHECK(!nc_cfg_parse(any, 5, &c));
    CHECK(!nc_cfg_parse(ok, 3, &c));
}

/* ---- window state machine ---------------------------------------------- */
static nc_snapshot_t snap;
static nc_action_t act[2];

static size_t step(nc_t *s, uint32_t now) { return nc_step(s, &snap, now, act); }

static void test_quiet(void) {
    nc_cfg_t cfg = default_cfg(); nc_t s; memset(&snap, 0, sizeof snap);
    nc_init(&s, &cfg, &snap);
    for (uint32_t t = 0; t < 100; t++) CHECK(step(&s, t) == 0);
    CHECK(!s.open);
}

static void test_single_pulse(void) {
    nc_cfg_t cfg = default_cfg(); nc_t s; memset(&snap, 0, sizeof snap);
    nc_init(&s, &cfg, &snap);

    snap.cnt[NC_LV_FIELD] = 1;
    CHECK(step(&s, 5) == 1);
    CHECK(act[0].kind == NC_ACT_ALERT && act[0].level == NC_LV_FIELD);
    CHECK(act[0].start_s == 5 && act[0].dur_s == 0 && act[0].field_ons == 1);

    /* no more activity: window stays open for cooldown, then ends */
    for (uint32_t t = 6; t < 5u + cfg.cooldown_s; t++) CHECK(step(&s, t) == 0);
    CHECK(step(&s, 5 + cfg.cooldown_s) == 1);
    CHECK(act[0].kind == NC_ACT_END && act[0].report);
    CHECK(act[0].level == NC_LV_FIELD && act[0].dur_s == 0 && act[0].flags == 0);
    CHECK(!s.open);
}

static void test_escalation_one_alert_per_level(void) {
    nc_cfg_t cfg = default_cfg(); nc_t s; memset(&snap, 0, sizeof snap);
    nc_init(&s, &cfg, &snap);

    snap.cnt[NC_LV_FIELD] = 1;  snap.field_present = true;
    CHECK(step(&s, 10) == 1 && act[0].level == NC_LV_FIELD);

    snap.cnt[NC_LV_POLL] += 3; snap.cmd[NC_LV_POLL] = 0x26;
    CHECK(step(&s, 11) == 1 && act[0].level == NC_LV_POLL && act[0].cmd == 0x26);

    snap.cnt[NC_LV_POLL] += 5;                    /* more of the same: no alert */
    CHECK(step(&s, 12) == 0);

    snap.cnt[NC_LV_SELECT] += 1; snap.cmd[NC_LV_SELECT] = 0x93;
    CHECK(step(&s, 13) == 1 && act[0].level == NC_LV_SELECT);

    snap.cnt[NC_LV_ENGAGE] += 1; snap.cmd[NC_LV_ENGAGE] = 0x60;
    CHECK(step(&s, 14) == 1 && act[0].level == NC_LV_ENGAGE && act[0].cmd == 0x60);
    CHECK(act[0].frames == 3 + 5 + 1 + 1 && act[0].dur_s == 4);

    snap.cnt[NC_LV_ENGAGE] += 1;                  /* already at max: silent */
    CHECK(step(&s, 15) == 0);

    snap.field_present = false;
    for (uint32_t t = 16; t < 15u + cfg.cooldown_s; t++) CHECK(step(&s, t) == 0);
    CHECK(step(&s, 15 + cfg.cooldown_s) == 1);
    CHECK(act[0].kind == NC_ACT_END && act[0].level == NC_LV_ENGAGE && act[0].cmd == 0x60);
    CHECK(act[0].dur_s == 5);                     /* 10 .. last activity at 15 */
    CHECK(act[0].field_ons == 1 && act[0].frames == 3 + 5 + 1 + 1 + 1);
}

static void test_jump_straight_to_engage(void) {
    nc_cfg_t cfg = default_cfg(); nc_t s; memset(&snap, 0, sizeof snap);
    nc_init(&s, &cfg, &snap);
    snap.cnt[NC_LV_FIELD] = 1; snap.cnt[NC_LV_POLL] = 1; snap.cnt[NC_LV_ENGAGE] = 1;
    snap.cmd[NC_LV_ENGAGE] = 0xE0;
    CHECK(step(&s, 0) == 1);                      /* one alert, at the deepest level */
    CHECK(act[0].level == NC_LV_ENGAGE && act[0].cmd == 0xE0);
}

static void test_field_pulsing_is_one_window(void) {
    nc_cfg_t cfg = default_cfg(); nc_t s; memset(&snap, 0, sizeof snap);
    nc_init(&s, &cfg, &snap);
    size_t alerts = 0, ends = 0;
    for (uint32_t t = 0; t < 60; t++) {
        if (t % 2 == 0) snap.cnt[NC_LV_FIELD]++;   /* reader pulses every 2 s */
        size_t n = step(&s, t);
        for (size_t i = 0; i < n; i++) {
            if (act[i].kind == NC_ACT_ALERT) alerts++; else ends++;
        }
    }
    CHECK(alerts == 1 && ends == 0 && s.open);
    CHECK(s.field_ons == 30);
}

static void test_min_level_filter(void) {
    nc_cfg_t cfg = default_cfg(); cfg.min_level = NC_LV_SELECT;
    nc_t s; memset(&snap, 0, sizeof snap);
    nc_init(&s, &cfg, &snap);

    /* window that never reaches SELECT: no alert, END has report == false */
    snap.cnt[NC_LV_FIELD] = 1; snap.cnt[NC_LV_POLL] = 1;
    CHECK(step(&s, 0) == 0);
    for (uint32_t t = 1; t < cfg.cooldown_s; t++) CHECK(step(&s, t) == 0);
    CHECK(step(&s, cfg.cooldown_s) == 1);
    CHECK(act[0].kind == NC_ACT_END && !act[0].report);

    /* window that does: alert at SELECT only */
    uint32_t t0 = 100;
    snap.cnt[NC_LV_FIELD]++; CHECK(step(&s, t0) == 0);
    snap.cnt[NC_LV_SELECT]++; snap.cmd[NC_LV_SELECT] = 0x93;
    CHECK(step(&s, t0 + 1) == 1 && act[0].level == NC_LV_SELECT);
}

static void test_baseline(void) {
    nc_cfg_t cfg = default_cfg(); nc_t s; memset(&snap, 0, sizeof snap);
    snap.cnt[NC_LV_FIELD] = 7; snap.cnt[NC_LV_POLL] = 9;
    nc_init(&s, &cfg, &snap);                      /* pre-arm activity ignored */
    CHECK(step(&s, 0) == 0);
    snap.cnt[NC_LV_POLL]++;
    CHECK(step(&s, 1) == 1 && act[0].frames == 1);
}

static void test_forced_close_and_reopen(void) {
    nc_cfg_t cfg = default_cfg(); nc_t s; memset(&snap, 0, sizeof snap);
    nc_init(&s, &cfg, &snap);
    snap.field_present = true;                    /* reader never goes away */
    snap.cnt[NC_LV_FIELD] = 1;
    CHECK(step(&s, 0) == 1);
    for (uint32_t t = 1; t < NC_MAX_WINDOW_S; t++) CHECK(step(&s, t) == 0);

    size_t n = step(&s, NC_MAX_WINDOW_S);          /* END (forced) + new ALERT */
    CHECK(n == 2);
    CHECK(act[0].kind == NC_ACT_END && (act[0].flags & NC_FLAG_FORCED));
    CHECK(act[0].dur_s == NC_MAX_WINDOW_S);        /* forced: reports up to now */
    CHECK(act[1].kind == NC_ACT_ALERT && act[1].level == NC_LV_FIELD);
    CHECK(act[1].start_s == NC_MAX_WINDOW_S && s.open);
}

static void test_disarm_close(void) {
    nc_cfg_t cfg = default_cfg(); nc_t s; memset(&snap, 0, sizeof snap);
    nc_init(&s, &cfg, &snap);
    nc_action_t a;
    CHECK(!nc_close(&s, 0, &a));                   /* nothing open */
    snap.cnt[NC_LV_POLL] = 2;
    CHECK(step(&s, 20) == 1);
    CHECK(nc_close(&s, 23, &a));
    CHECK(a.kind == NC_ACT_END && (a.flags & NC_FLAG_DISARM) && a.dur_s == 3 && a.report);
    CHECK(!s.open && !nc_close(&s, 24, &a));
}

static void test_counter_wrap(void) {
    nc_cfg_t cfg = default_cfg(); nc_t s; memset(&snap, 0, sizeof snap);
    snap.cnt[NC_LV_POLL] = 0xFFFFFFFFu;
    nc_init(&s, &cfg, &snap);
    snap.cnt[NC_LV_POLL] = 1;                      /* wrapped: delta = 2 */
    CHECK(step(&s, 0) == 1 && act[0].frames == 2);
}

/* ---- wire formats -------------------------------------------------------- */
static void test_evt_pack(void) {
    nc_action_t a; memset(&a, 0, sizeof a);
    a.kind = NC_ACT_END; a.level = NC_LV_ENGAGE; a.cmd = 0x60;
    a.dur_s = 0x12345; a.field_ons = 999; a.frames = 0x0102; a.flags = NC_FLAG_FORCED;
    uint8_t p[NC_EVT_PAYLOAD_SIZE];
    nc_evt_pack(p, NC_EVT_END, &a, 7);
    CHECK(p[0] == NC_EVT_END && p[1] == 3 && p[2] == 0x60 && p[3] == 7);
    CHECK(p[4] == 0xFF && p[5] == 0xFF);           /* dur saturated */
    CHECK(p[6] == 0xFF);                           /* field_ons saturated */
    CHECK(p[7] == NC_FLAG_FORCED && p[8] == 0x02 && p[9] == 0x01);

    a.kind = NC_ACT_ALERT; a.dur_s = 300;
    nc_evt_pack(p, NC_EVT_ALERT, &a, 8);
    CHECK(p[7] == 0 && p[4] == (300 & 0xFF) && p[5] == (300 >> 8));  /* flags END-only */

    nc_evt_pack(p, NC_EVT_TEST, NULL, 9);
    uint8_t exp[NC_EVT_PAYLOAD_SIZE] = {NC_EVT_TEST, 0, 0, 9, 0, 0, 0, 0, 0, 0};
    CHECK(memcmp(p, exp, sizeof p) == 0);
}

static void test_rec_pack(void) {
    nc_action_t a; memset(&a, 0, sizeof a);
    a.kind = NC_ACT_END; a.level = 2; a.cmd = 0x93; a.start_s = 0x01020304;
    a.dur_s = 70000; a.field_ons = 5; a.frames = 300; a.flags = NC_FLAG_DISARM;
    uint8_t r[NC_REC_SIZE];
    nc_rec_pack(r, &a, 4);
    CHECK(r[0] == NC_REC_TYPE_WINDOW && r[1] == 2 && r[2] == 0x93 && r[3] == 4);
    CHECK(r[4] == 4 && r[5] == 3 && r[6] == 2 && r[7] == 1);
    CHECK(r[8] == 0xFF && r[9] == 0xFF);
    CHECK(r[10] == 5 && r[11] == 0 && r[12] == (300 & 0xFF) && r[13] == (300 >> 8));
    CHECK(r[14] == NC_FLAG_DISARM && r[15] == 0);
}

static void test_log(void) {
    uint8_t buf[NC_REC_SIZE * 3]; size_t used = 0; bool dropped = true;
    uint8_t rec[NC_REC_SIZE];
    for (int i = 1; i <= 3; i++) {
        memset(rec, i, sizeof rec);
        used = nc_log_append(buf, sizeof buf, used, rec, &dropped);
        CHECK(!dropped && used == (size_t)i * NC_REC_SIZE);
    }
    memset(rec, 4, sizeof rec);
    used = nc_log_append(buf, sizeof buf, used, rec, &dropped);
    CHECK(dropped && used == sizeof buf);
    CHECK(buf[0] == 2 && buf[16] == 3 && buf[32] == 4);   /* oldest evicted */

    used = nc_log_append(buf, sizeof buf, used, rec, NULL);   /* NULL ok */
    CHECK(used == sizeof buf && buf[0] == 3);

    uint8_t tiny[8];
    CHECK(nc_log_append(tiny, sizeof tiny, 0, rec, &dropped) == 0 && !dropped);
}

int main(void) {
    test_classify(); test_cfg();
    test_quiet(); test_single_pulse(); test_escalation_one_alert_per_level();
    test_jump_straight_to_engage(); test_field_pulsing_is_one_window();
    test_min_level_filter(); test_baseline(); test_forced_close_and_reopen();
    test_disarm_close(); test_counter_wrap();
    test_evt_pack(); test_rec_pack(); test_log();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
