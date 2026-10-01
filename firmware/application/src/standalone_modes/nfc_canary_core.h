/*
 * nfc_canary_core.h
 *
 * Hardware-free logic for the nfc_canary standalone mode: probe
 * classification, the alert/cooldown "window" state machine, and the two wire
 * formats (BLE event payload, 16-byte log record).
 *
 * Deliberately has no firmware includes so it can be unit-tested natively
 * (see standalone_modes/tests/). mode_nfc_canary.c is the thin glue that feeds
 * it ISR counters and acts on what it returns.
 *
 * Concepts
 * --------
 *  level   how deep a reader got into probing us:
 *            FIELD   its RF field appeared
 *            POLL    it sent REQA / WUPA
 *            SELECT  it ran anticollision / SELECT (0x93 / 0x95 / 0x97)
 *            ENGAGE  anything beyond that (RATS, AUTH, READ, magic wake-ups..)
 *
 *  window  a burst of related activity. It opens on the first activity and
 *          closes after cooldown_s seconds with no activity and no field (or
 *          after NC_MAX_WINDOW_S, so a reader that never goes away still
 *          re-alerts). A reader that pulses its field every second therefore
 *          produces ONE window, not one alert per pulse.
 *
 *  alert   emitted at most once per level per window, and only for levels
 *          >= min_level. A window therefore sends at most NC_LV__COUNT alerts.
 */

#ifndef NFC_CANARY_CORE_H
#define NFC_CANARY_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Levels and classification
 * ------------------------------------------------------------------------- */

typedef enum {
    NC_LV_FIELD  = 0,
    NC_LV_POLL   = 1,
    NC_LV_SELECT = 2,
    NC_LV_ENGAGE = 3,
    NC_LV__COUNT
} nc_level_t;

#define NC_LV_NONE  (-1)

/* Classify one received frame. `bits` is the received bit count; the first 8
 * bits of `data` are always data byte 0 (parity, if present, follows it).
 * Returns an nc_level_t, or NC_LV_NONE for frames too short to be real
 * (< 7 bits: field-transient noise). */
int nc_classify(const uint8_t *data, uint16_t bits);

/* -------------------------------------------------------------------------
 * Config blob (persisted by the framework, 4 bytes)
 *   [0] version (=1)
 *   [1] min_level   0..3   ignore windows that never reach this level
 *   [2] cooldown_s  1..255 quiet seconds before a window closes
 *   [3] flags       bit0 BLE alerts   bit1 BLE end-of-window summaries
 * ------------------------------------------------------------------------- */

#define NC_CFG_SIZE             4
#define NC_CFG_VERSION          1
#define NC_CFG_FLAG_BLE_ALERT   0x01
#define NC_CFG_FLAG_BLE_END     0x02
#define NC_COOLDOWN_DEFAULT_S   10
#define NC_MAX_WINDOW_S         300u

typedef struct {
    uint8_t min_level;
    uint8_t cooldown_s;
    uint8_t flags;
} nc_cfg_t;

/* NULL/0-length blob -> defaults (min_level 0, cooldown 10 s, both BLE flags).
 * Returns false (and leaves *out at defaults) if the blob is malformed. */
bool nc_cfg_parse(const uint8_t *blob, size_t len, nc_cfg_t *out);

/* -------------------------------------------------------------------------
 * Window state machine
 * ------------------------------------------------------------------------- */

/* Monotonic counters maintained by the NFCT ISR; the mode copies them out and
 * hands the copy here. cnt[FIELD] counts field-on edges. */
typedef struct {
    uint32_t cnt[NC_LV__COUNT];
    uint8_t  cmd[NC_LV__COUNT];     /* first byte of the latest frame at that level */
    bool     field_present;
} nc_snapshot_t;

typedef struct {
    nc_cfg_t cfg;
    uint32_t seen[NC_LV__COUNT];
    bool     open;
    uint32_t start_s;
    uint32_t last_act_s;
    uint8_t  max_level;
    uint8_t  cmd;                   /* command byte of the deepest frame so far */
    int8_t   alerted;               /* highest level alerted this window, -1 = none */
    uint32_t field_ons;
    uint32_t frames;
} nc_t;

typedef enum {
    NC_ACT_ALERT = 1,   /* window reached a new, reportable level   */
    NC_ACT_END   = 2,   /* window closed                            */
} nc_act_kind_t;

#define NC_FLAG_FORCED  0x01    /* END: closed because it hit NC_MAX_WINDOW_S */
#define NC_FLAG_DISARM  0x02    /* END: closed because the mode was disarmed  */

typedef struct {
    nc_act_kind_t kind;
    uint8_t  level;
    uint8_t  cmd;
    uint32_t start_s;
    uint32_t dur_s;         /* ALERT: window age now.  END: first..last activity */
    uint32_t field_ons;
    uint32_t frames;
    uint8_t  flags;         /* NC_FLAG_* (END only) */
    bool     report;        /* END only: window reached min_level -> log + notify */
} nc_action_t;

/* `baseline` seeds the "already seen" counters so activity from before arming
 * is not counted. NULL = all zero. */
void   nc_init(nc_t *s, const nc_cfg_t *cfg, const nc_snapshot_t *baseline);

/* Advance the machine. Writes at most 2 actions into out[] (a forced END can be
 * followed by the ALERT that opens the next window) and returns the count.
 * now_s must be monotonic non-decreasing. */
size_t nc_step(nc_t *s, const nc_snapshot_t *snap, uint32_t now_s,
               nc_action_t out[2]);

/* Close the open window (if any) because the mode is being disarmed.
 * Returns true and fills *out if a window was open. */
bool   nc_close(nc_t *s, uint32_t now_s, nc_action_t *out);

/* -------------------------------------------------------------------------
 * Wire formats (all little-endian)
 * ------------------------------------------------------------------------- */

/* BLE event payload, 10 bytes. Sent inside a standard data frame with
 * cmd = DATA_CMD_STANDALONE_CANARY_EVENT, so one notification at the default
 * 23-byte ATT MTU (frame = 10 + 10 = 20 bytes).
 *   [0]    type      NC_EVT_*
 *   [1]    level
 *   [2]    cmd       command byte of the deepest frame (0 if level == FIELD)
 *   [3]    seq       increments per event; a gap means a dropped notification
 *   [4..5] dur_s     ALERT: window age.  END: first..last activity (saturating)
 *   [6]    field_ons (saturating)
 *   [7]    flags     NC_FLAG_* (END only)
 *   [8..9] frames    (saturating)
 */
#define NC_EVT_PAYLOAD_SIZE    10
#define NC_EVT_ALERT           1
#define NC_EVT_END             2
#define NC_EVT_TEST            3

/* a == NULL is allowed for NC_EVT_TEST (payload is all zero apart from type/seq). */
void nc_evt_pack(uint8_t out[NC_EVT_PAYLOAD_SIZE], uint8_t type,
                 const nc_action_t *a, uint8_t seq);

/* Log record, 16 bytes, one per closed reportable window.
 *   [0]     type       NC_REC_TYPE_WINDOW
 *   [1]     level
 *   [2]     cmd
 *   [3]     epoch      arm counter; start_s is relative to that arm
 *   [4..7]  start_s    seconds since arm
 *   [8..9]  dur_s      (saturating)
 *   [10..11] field_ons (saturating)
 *   [12..13] frames    (saturating)
 *   [14]    flags      NC_FLAG_*
 *   [15]    reserved   0
 */
#define NC_REC_SIZE            16
#define NC_REC_TYPE_WINDOW     1

void nc_rec_pack(uint8_t out[NC_REC_SIZE], const nc_action_t *end, uint8_t epoch);

/* Append one record to a flat buffer, dropping the OLDEST record if full.
 * `used` and `cap` should be multiples of NC_REC_SIZE. Returns the new used
 * length. *dropped is set true if a record was evicted. */
size_t nc_log_append(uint8_t *buf, size_t cap, size_t used,
                     const uint8_t rec[NC_REC_SIZE], bool *dropped);

#ifdef __cplusplus
}
#endif

#endif /* NFC_CANARY_CORE_H */
