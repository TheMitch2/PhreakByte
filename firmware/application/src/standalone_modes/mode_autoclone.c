/*
 * mode_autoclone.c
 *
 * Two-step MIFARE Classic 1K clone to a gen1a "magic" card. Ultra only.
 *   1st BOTH_SHORT: scan a source card, read its blocks (default key), buffer.
 *   2nd BOTH_SHORT: write the buffered blocks to the magic card now present.
 * Optionally also clones the source into the active slot (cfg.also_slot).
 *
 * writes_tag + writes_slot -> requires STANDALONE_FLAG_HOST_OPTED_IN to arm.
 */

#include "app_standalone.h"
#include "standalone_led.h"

#if defined(PROJECT_CHAMELEON_ULTRA)

#include <string.h>

#include "nrf_log.h"
#include "app_status.h"
#include "rc522.h"
#include "rfid_main.h"
#include "bsp_delay.h"
#include "bsp_wdt.h"
#include "tag_emulation.h"
#include "tag_base_type.h"
#include "nfc_14a.h"
#include "nfc_mf1.h"

#define CFG_VERSION      1
#define MODE_NAME        "autoclone"
#define MFC1K_SECTORS    16
#define MFC1K_BLOCKS     64
#define BLK_SIZE         16
#define MFC_BLK(sec, i)  ((uint8_t)((sec) * 4 + (i)))

static const uint8_t DEFAULT_KEY[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t also_slot;      /* 1 = also clone source into the active slot */
    uint8_t reserved[2];
}
cfg_t;
_Static_assert(sizeof(cfg_t) == 4, "autoclone cfg_t must be 4 bytes");

/* Result record (11 bytes): result, uid_len, uid[7], blocks_written */
#define RES_OK           0
#define RES_NO_SOURCE    1
#define RES_NO_TARGET    2
#define RES_WRITE_FAIL   3
#define REC_SIZE             11
#define RESULT_BUFFER_BYTES  (8 * REC_SIZE)

static struct {
    cfg_t    cfg;
    bool     active;
    bool     have_source;
    picc_14a_tag_t src_tag;
    uint8_t  src[MFC1K_BLOCKS][BLK_SIZE];
    bool     valid[MFC1K_BLOCKS];
    size_t   write_cursor;
    size_t   read_cursor;
    uint8_t  buffer[RESULT_BUFFER_BYTES];
}
m_st;

static void apply_defaults(cfg_t *c) {
    memset(c, 0, sizeof(*c));
    c->version   = CFG_VERSION;
    c->also_slot = 0;
}

static bool cfg_valid(const cfg_t *c) {
    return c->version == CFG_VERSION && c->also_slot <= 1;
}

static void record(uint8_t result, const picc_14a_tag_t *tag, uint8_t written) {
    if (m_st.write_cursor + REC_SIZE > RESULT_BUFFER_BYTES) return;
    uint8_t *r = &m_st.buffer[m_st.write_cursor];
    memset(r, 0, REC_SIZE);
    r[0] = result;
    if (tag) {
        r[1] = tag->uid_len;
        memcpy(&r[2], tag->uid, tag->uid_len > 7 ? 7 : tag->uid_len);
    }
    r[9]  = written;
    m_st.write_cursor += REC_SIZE;
}

static void reader_on(void) {
    if (get_device_mode() != DEVICE_MODE_READER) {
        reader_mode_enter();
        bsp_delay_ms(8);
    }
    pcd_14a_reader_antenna_on();
    bsp_delay_ms(8);
}

/* Read the source card into the RAM buffer. Returns sectors read. */
static uint8_t read_source(void) {
    memset(m_st.valid, 0, sizeof(m_st.valid));
    uint8_t read = 0;
    for (uint8_t sec = 0; sec < MFC1K_SECTORS; sec++) {
        bsp_wdt_feed();
        picc_14a_tag_t t;
        if (pcd_14a_reader_scan_auto(&t) != STATUS_HF_TAG_OK) continue;
        if (pcd_14a_reader_mf1_auth(&t, PICC_AUTHENT1A, MFC_BLK(sec, 3),
                                    (uint8_t *)DEFAULT_KEY) != STATUS_HF_TAG_OK) continue;
        for (uint8_t i = 0; i < 4; i++) {
            uint8_t blk = MFC_BLK(sec, i);
            if (pcd_14a_reader_mf1_read(blk, m_st.src[blk]) == STATUS_HF_TAG_OK)
                m_st.valid[blk] = true;
        }
        read++;
    }
    pcd_14a_reader_mf1_unauth();
    return read;
}

/* Write buffered blocks to a gen1a magic card. Returns blocks written. */
static uint8_t write_magic(void) {
    uint8_t written = 0;
    if (pcd_14a_reader_gen1a_unlock() != STATUS_HF_TAG_OK) return 0;
    for (uint8_t blk = 0; blk < MFC1K_BLOCKS; blk++) {
        bsp_wdt_feed();
        if (!m_st.valid[blk]) continue;
        if (pcd_14a_reader_mf1_write(blk, m_st.src[blk]) == STATUS_HF_TAG_OK) written++;
    }
    pcd_14a_reader_gen1a_uplock();
    return written;
}

/* Clone the buffered source into the active slot (cfg.also_slot). */
static void clone_to_slot(void) {
    uint8_t slot = tag_emulation_get_slot();
    tag_emulation_change_type(slot, TAG_TYPE_MIFARE_1024);
    tag_data_buffer_t *buf = get_buffer_by_tag_type(TAG_TYPE_MIFARE_1024);
    if (buf == NULL) return;
    nfc_tag_mf1_information_t *info = (nfc_tag_mf1_information_t *)buf->buffer;

    nfc_tag_14a_coll_res_entity_t *res = &info->res_coll;
    res->size = (nfc_tag_14a_uid_size)m_st.src_tag.uid_len;
    memcpy(res->uid, m_st.src_tag.uid, m_st.src_tag.uid_len);
    memcpy(res->atqa, m_st.src_tag.atqa, 2);
    res->sak[0]     = m_st.src_tag.sak;
    res->ats.length = m_st.src_tag.ats_len;
    memcpy(res->ats.data, m_st.src_tag.ats, m_st.src_tag.ats_len);

    for (uint8_t blk = 0; blk < MFC1K_BLOCKS; blk++)
        if (m_st.valid[blk]) memcpy(info->memory[blk], m_st.src[blk], BLK_SIZE);

    tag_emulation_save();
}

static standalone_rc_t on_enter(const uint8_t *cfg, size_t cfg_len) {
    apply_defaults(&m_st.cfg);
    if (cfg != NULL && cfg_len == sizeof(cfg_t)) {
        cfg_t p;
        memcpy(&p, cfg, sizeof(p));
        if (cfg_valid(&p)) m_st.cfg = p;
        else NRF_LOG_WARNING(MODE_NAME ": invalid cfg, using defaults");
    }
    m_st.active      = true;
    m_st.have_source = false;
    NRF_LOG_INFO(MODE_NAME ": armed");
    return STANDALONE_RC_OK;
}

static standalone_rc_t on_exit(void) {
    m_st.active      = false;
    m_st.have_source = false;
    if (get_device_mode() == DEVICE_MODE_READER) tag_mode_enter();
    return STANDALONE_RC_OK;
}

static standalone_rc_t on_button(standalone_button_evt_t evt) {
    if (!m_st.active) return STANDALONE_RC_INVALID_STATE;

    if (evt == STANDALONE_BTN_BOTH_VLONG) {
        m_st.have_source  = false;
        m_st.write_cursor = 0;
        m_st.read_cursor  = 0;
        standalone_feedback(SL_FB_SUCCESS);
        return STANDALONE_RC_OK;
    }
    if (evt != STANDALONE_BTN_BOTH_SHORT) return STANDALONE_RC_OK;

    standalone_feedback(SL_FB_BUSY_START);

    if (!m_st.have_source) {
        /* Step 1: read the source card. */
        reader_on();
        uint8_t st = pcd_14a_reader_scan_auto(&m_st.src_tag);
        uint8_t read = (st == STATUS_HF_TAG_OK) ? read_source() : 0;
        pcd_14a_reader_antenna_off();

        if (st == STATUS_HF_TAG_OK && read > 0) {
            m_st.have_source = true;
            if (m_st.cfg.also_slot) clone_to_slot();
            tag_mode_enter();
            standalone_feedback(SL_FB_SUCCESS);   /* present the target, press again */
            return STANDALONE_RC_OK;
        }
        tag_mode_enter();
        record(RES_NO_SOURCE, (st == STATUS_HF_TAG_OK) ? &m_st.src_tag : NULL, 0);
        standalone_feedback(SL_FB_ERROR);
        return STANDALONE_RC_NO_TAG;
    }

    /* Step 2: write the buffered card to the magic card now present. */
    reader_on();
    picc_14a_tag_t target;
    uint8_t st = pcd_14a_reader_scan_auto(&target);
    uint8_t written = (st == STATUS_HF_TAG_OK) ? write_magic() : 0;
    pcd_14a_reader_antenna_off();
    tag_mode_enter();

    if (st != STATUS_HF_TAG_OK) {
        record(RES_NO_TARGET, &m_st.src_tag, 0);
        standalone_feedback(SL_FB_ERROR);
        return STANDALONE_RC_NO_TAG;
    }
    m_st.have_source = false;
    record(written > 0 ? RES_OK : RES_WRITE_FAIL, &m_st.src_tag, written);
    standalone_feedback(written > 0 ? SL_FB_SUCCESS : SL_FB_ERROR);
    return written > 0 ? STANDALONE_RC_OK : STANDALONE_RC_WRITE_FAIL;
}

static size_t get_result_size(void) {
    return m_st.write_cursor;
}

static standalone_rc_t read_result(uint8_t *out, size_t out_max, size_t *out_len) {
    if (!out || !out_len) return STANDALONE_RC_INVALID_CFG;
    if (m_st.read_cursor >= m_st.write_cursor) {
        *out_len = 0;
        return STANDALONE_RC_NO_RESULT;
    }
    size_t rem  = m_st.write_cursor - m_st.read_cursor;
    size_t take = rem < out_max ? rem : out_max;
    memcpy(out, &m_st.buffer[m_st.read_cursor], take);
    m_st.read_cursor += take;
    *out_len = take;
    return STANDALONE_RC_OK;
}

static void clear_result(void) {
    m_st.write_cursor = 0;
    m_st.read_cursor  = 0;
}

const standalone_mode_iface_t mode_autoclone_iface = {
    .id              = STANDALONE_MODE_AUTOCLONE,
    .name            = MODE_NAME,
    .writes_tag      = true,
    .writes_slot     = true,
    .wants_tick      = false,
    .on_enter        = on_enter,
    .on_exit         = on_exit,
    .on_button       = on_button,
    .on_tick         = NULL,
    .get_result_size = get_result_size,
    .read_result     = read_result,
    .clear_result    = clear_result,
    .ensure_loaded   = NULL,
};

#endif /* PROJECT_CHAMELEON_ULTRA */
