/*
 * nfc_canary_core.c - see nfc_canary_core.h
 */

#include "nfc_canary_core.h"

#include <string.h>

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static uint8_t sat8(uint32_t v)  { return v > 0xFFu   ? 0xFFu   : (uint8_t)v;  }
static uint16_t sat16(uint32_t v) { return v > 0xFFFFu ? 0xFFFFu : (uint16_t)v; }

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* -------------------------------------------------------------------------
 * Classification
 * ------------------------------------------------------------------------- */

int nc_classify(const uint8_t *data, uint16_t bits) {
    if (data == NULL || bits < 7) return NC_LV_NONE;

    if (bits == 7) {
        /* ISO14443-3A short frame: REQA / WUPA are the normal polls. Any other
         * 7-bit frame is a deliberate non-standard probe (e.g. the 0x40 / 0x43
         * magic-card wake-ups) and is worth flagging as such. */
        return (data[0] == 0x26 || data[0] == 0x52) ? NC_LV_POLL : NC_LV_ENGAGE;
    }

    switch (data[0]) {
        case 0x93:      /* SEL / anticollision cascade level 1 */
        case 0x95:      /* ... level 2 */
        case 0x97:      /* ... level 3 */
            return NC_LV_SELECT;
        default:
            return NC_LV_ENGAGE;
    }
}

/* -------------------------------------------------------------------------
 * Config
 * ------------------------------------------------------------------------- */

static void cfg_defaults(nc_cfg_t *c) {
    c->min_level  = NC_LV_FIELD;
    c->cooldown_s = NC_COOLDOWN_DEFAULT_S;
    c->flags      = NC_CFG_FLAG_BLE_ALERT | NC_CFG_FLAG_BLE_END;
}

bool nc_cfg_parse(const uint8_t *blob, size_t len, nc_cfg_t *out) {
    cfg_defaults(out);
    if (blob == NULL || len == 0) return true;
    if (len != NC_CFG_SIZE)                          return false;
    if (blob[0] != NC_CFG_VERSION)                   return false;
    if (blob[1] >= NC_LV__COUNT)                     return false;
    if (blob[2] == 0)                                return false;
    if (blob[3] & ~(NC_CFG_FLAG_BLE_ALERT | NC_CFG_FLAG_BLE_END)) return false;

    out->min_level  = blob[1];
    out->cooldown_s = blob[2];
    out->flags      = blob[3];
    return true;
}

/* -------------------------------------------------------------------------
 * Window state machine
 * ------------------------------------------------------------------------- */

void nc_init(nc_t *s, const nc_cfg_t *cfg, const nc_snapshot_t *baseline) {
    memset(s, 0, sizeof(*s));
    s->cfg     = *cfg;
    s->alerted = -1;
    if (baseline != NULL) {
        for (int i = 0; i < NC_LV__COUNT; i++) s->seen[i] = baseline->cnt[i];
    }
}

static void window_open(nc_t *s, uint32_t now_s) {
    s->open       = true;
    s->start_s    = now_s;
    s->last_act_s = now_s;
    s->max_level  = NC_LV_FIELD;
    s->cmd        = 0;
    s->alerted    = -1;
    s->field_ons  = 0;
    s->frames     = 0;
}

static void window_end(nc_t *s, uint32_t now_s, uint8_t flags, nc_action_t *a) {
    /* Normal close reports the real activity span; a forced/disarm close has no
     * quiet tail to exclude, so it reports up to "now". */
    uint32_t end_s = (flags & (NC_FLAG_FORCED | NC_FLAG_DISARM)) ? now_s : s->last_act_s;

    memset(a, 0, sizeof(*a));
    a->kind      = NC_ACT_END;
    a->level     = s->max_level;
    a->cmd       = s->cmd;
    a->start_s   = s->start_s;
    a->dur_s     = end_s - s->start_s;
    a->field_ons = s->field_ons;
    a->frames    = s->frames;
    a->flags     = flags;
    a->report    = s->max_level >= s->cfg.min_level;

    s->open = false;
}

size_t nc_step(nc_t *s, const nc_snapshot_t *snap, uint32_t now_s,
               nc_action_t out[2]) {
    size_t n = 0;

    /* Deltas since the last step. Unsigned subtraction is wrap-safe. */
    uint32_t d[NC_LV__COUNT];
    int deepest = -1;
    for (int i = 0; i < NC_LV__COUNT; i++) {
        d[i]       = snap->cnt[i] - s->seen[i];
        s->seen[i] = snap->cnt[i];
        if (d[i] != 0) deepest = i;         /* ascending loop: last hit = deepest */
    }

    bool active = (deepest >= 0) || snap->field_present;

    /* --- closing --------------------------------------------------------- */
    if (s->open) {
        if (!active && (now_s - s->last_act_s) >= s->cfg.cooldown_s) {
            window_end(s, now_s, 0, &out[n++]);
        } else if ((now_s - s->start_s) >= NC_MAX_WINDOW_S) {
            window_end(s, now_s, NC_FLAG_FORCED, &out[n++]);
        }
    }

    /* --- activity -------------------------------------------------------- */
    if (active) {
        if (!s->open) window_open(s, now_s);
        s->last_act_s = now_s;

        s->field_ons += d[NC_LV_FIELD];
        s->frames    += d[NC_LV_POLL] + d[NC_LV_SELECT] + d[NC_LV_ENGAGE];

        if (deepest >= 0 && deepest >= (int)s->max_level) {
            s->max_level = (uint8_t)deepest;
            s->cmd       = snap->cmd[deepest];
        }

        if ((int)s->max_level > (int)s->alerted &&
                s->max_level >= s->cfg.min_level) {
            nc_action_t *a = &out[n++];
            memset(a, 0, sizeof(*a));
            a->kind      = NC_ACT_ALERT;
            a->level     = s->max_level;
            a->cmd       = s->cmd;
            a->start_s   = s->start_s;
            a->dur_s     = now_s - s->start_s;
            a->field_ons = s->field_ons;
            a->frames    = s->frames;
            s->alerted   = (int8_t)s->max_level;
        }
    }

    return n;
}

bool nc_close(nc_t *s, uint32_t now_s, nc_action_t *out) {
    if (!s->open) return false;
    window_end(s, now_s, NC_FLAG_DISARM, out);
    return true;
}

/* -------------------------------------------------------------------------
 * Wire formats
 * ------------------------------------------------------------------------- */

void nc_evt_pack(uint8_t out[NC_EVT_PAYLOAD_SIZE], uint8_t type,
                 const nc_action_t *a, uint8_t seq) {
    memset(out, 0, NC_EVT_PAYLOAD_SIZE);
    out[0] = type;
    out[3] = seq;
    if (a == NULL) return;

    out[1] = a->level;
    out[2] = a->cmd;
    put_u16(&out[4], sat16(a->dur_s));
    out[6] = sat8(a->field_ons);
    out[7] = (a->kind == NC_ACT_END) ? a->flags : 0;
    put_u16(&out[8], sat16(a->frames));
}

void nc_rec_pack(uint8_t out[NC_REC_SIZE], const nc_action_t *end, uint8_t epoch) {
    memset(out, 0, NC_REC_SIZE);
    out[0] = NC_REC_TYPE_WINDOW;
    out[1] = end->level;
    out[2] = end->cmd;
    out[3] = epoch;
    put_u32(&out[4], end->start_s);
    put_u16(&out[8],  sat16(end->dur_s));
    put_u16(&out[10], sat16(end->field_ons));
    put_u16(&out[12], sat16(end->frames));
    out[14] = end->flags;
    out[15] = 0;
}

size_t nc_log_append(uint8_t *buf, size_t cap, size_t used,
                     const uint8_t rec[NC_REC_SIZE], bool *dropped) {
    if (dropped != NULL) *dropped = false;
    if (cap < NC_REC_SIZE) return used;

    while (used + NC_REC_SIZE > cap) {
        /* Evict the oldest record. */
        if (used < NC_REC_SIZE) { used = 0; break; }
        memmove(buf, buf + NC_REC_SIZE, used - NC_REC_SIZE);
        used -= NC_REC_SIZE;
        if (dropped != NULL) *dropped = true;
    }
    memcpy(buf + used, rec, NC_REC_SIZE);
    return used + NC_REC_SIZE;
}
