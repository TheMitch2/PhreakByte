/**
 * @file nfc_st25ta.c
 * @brief ST25TA (NFC Forum Type 4 Tag) emulation
 *
 * Command set and file layout follow the ST25TA64K datasheet (DocID027764).
 */

#include <string.h>
#include <stddef.h>
#include "nfc_st25ta.h"
#include "nfc_14a_4.h"
#include "nfc_14a.h"
#include "tag_emulation.h"
#include "tag_persistence.h"
#include "fds_util.h"
#include "nrf_log.h"

#define ST25TA_HDR_SIZE     offsetof(nfc_tag_st25ta_information_t, ndef)
#define ST25TA_SYS_SIZE     18
#define ST25TA_MAX_RW       0xF6
#define ST25TA_PWD_TRIES    3

#define FID_CC      0xE103
#define FID_SYSTEM  0xE101
#define FID_NDEF    0x0001

_Static_assert(sizeof(nfc_tag_st25ta_information_t) <= 8192, "ST25TA record exceeds HF buffer");

static const uint8_t ST25TA_AID[] = {0xD2, 0x76, 0x00, 0x00, 0x85, 0x01, 0x01};

typedef enum {
    SEL_NONE = 0,
    SEL_CC,
    SEL_NDEF,
    SEL_SYSTEM,
} sel_file_t;

typedef struct {
    bool       app_selected;
    sel_file_t file;
    bool       read_ok;
    bool       write_ok;
    uint8_t    read_tries;
    uint8_t    write_tries;
} st25ta_session_t;

static nfc_tag_st25ta_information_t *m_tag_information = NULL;
static nfc_tag_14a_coll_res_reference_t m_shadow_coll_res;
static nfc_tag_14a_4_tcl_state_t m_tcl_session_state;
static st25ta_session_t m_sess;

static void session_reset(void) {
    memset(&m_sess, 0, sizeof(m_sess));
    m_sess.read_tries = ST25TA_PWD_TRIES;
    m_sess.write_tries = ST25TA_PWD_TRIES;
}

nfc_tag_14a_coll_res_reference_t *nfc_tag_st25ta_get_coll_res(void) {
    if (m_tag_information == NULL) return NULL;
    m_shadow_coll_res.sak  = m_tag_information->res_coll.sak;
    m_shadow_coll_res.atqa = m_tag_information->res_coll.atqa;
    m_shadow_coll_res.uid  = m_tag_information->res_coll.uid;
    m_shadow_coll_res.size = &m_tag_information->res_coll.size;
    m_shadow_coll_res.ats  = &m_tag_information->res_coll.ats;
    return &m_shadow_coll_res;
}

void nfc_tag_st25ta_sync_cc(nfc_tag_st25ta_information_t *info) {
    info->cc[ST25TA_CC_OFF_FILE_SIZE]     = (uint8_t)(info->ndef_size >> 8);
    info->cc[ST25TA_CC_OFF_FILE_SIZE + 1] = (uint8_t)(info->ndef_size & 0xFF);
}

static void build_system_file(uint8_t *out) {
    nfc_tag_st25ta_information_t *i = m_tag_information;
    uint16_t mem_size = i->ndef_size - 1;
    memset(out, 0, ST25TA_SYS_SIZE);
    out[0] = 0x00; out[1] = ST25TA_SYS_SIZE;
    out[2] = 0x01; out[3] = 0x00;
    out[4] = 0x11; out[5] = 0x00;
    memcpy(&out[8], i->res_coll.uid, 7);
    out[15] = (uint8_t)(mem_size >> 8);
    out[16] = (uint8_t)(mem_size & 0xFF);
    out[17] = i->res_coll.uid[1];   // product code
}

static inline uint16_t ndef_msg_len(void) {
    return ((uint16_t)m_tag_information->ndef[0] << 8) | m_tag_information->ndef[1];
}

static void respond(uint16_t data_len, uint8_t sw1, uint8_t sw2) {
    m_tcl_session_state.m_resp_buf[data_len++] = sw1;
    m_tcl_session_state.m_resp_buf[data_len++] = sw2;
    m_tcl_session_state.m_resp_len = data_len;
    m_tcl_session_state.m_response_ready = true;
    nfc_tag_14a_4_base_respond(&m_tcl_session_state);
}

static inline void respond_sw(uint8_t sw1, uint8_t sw2) {
    respond(0, sw1, sw2);
}

static bool read_allowed(void) {
    uint8_t acc = m_tag_information->cc[ST25TA_CC_OFF_READ_ACCESS];
    if (acc == ST25TA_ACCESS_FREE) return true;
    return acc == ST25TA_ACCESS_LOCKED && m_sess.read_ok;
}

static bool write_allowed(void) {
    uint8_t acc = m_tag_information->cc[ST25TA_CC_OFF_WRITE_ACCESS];
    if (acc == ST25TA_ACCESS_FREE) return true;
    return acc == ST25TA_ACCESS_LOCKED && m_sess.write_ok;
}

// Rights to change protection: no write protection, or write password verified
static bool admin_allowed(void) {
    uint8_t acc = m_tag_information->cc[ST25TA_CC_OFF_WRITE_ACCESS];
    return acc == ST25TA_ACCESS_FREE || (acc == ST25TA_ACCESS_LOCKED && m_sess.write_ok);
}

static void do_select(uint8_t *apdu, uint16_t len) {
    uint8_t p1 = apdu[2], p2 = apdu[3];
    uint8_t lc = (len > 4) ? apdu[4] : 0;
    if (len < 5 + (uint16_t)lc) { respond_sw(0x67, 0x00); return; }
    uint8_t *d = &apdu[5];

    if (p1 == 0x04 && p2 == 0x00) {
        if (lc == sizeof(ST25TA_AID) && memcmp(d, ST25TA_AID, lc) == 0) {
            session_reset();
            m_sess.app_selected = true;
            respond_sw(0x90, 0x00);
        } else {
            respond_sw(0x6A, 0x82);
        }
        return;
    }
    if (p1 != 0x00 || p2 != 0x0C) { respond_sw(0x6A, 0x86); return; }
    if (lc != 2) { respond_sw(0x6A, 0x80); return; }
    if (!m_sess.app_selected) { respond_sw(0x6A, 0x82); return; }

    uint16_t fid = ((uint16_t)d[0] << 8) | d[1];
    sel_file_t f;
    switch (fid) {
        case FID_CC:     f = SEL_CC; break;
        case FID_NDEF:   f = SEL_NDEF; break;
        case FID_SYSTEM: f = SEL_SYSTEM; break;
        default: respond_sw(0x6A, 0x82); return;
    }
    // Rights are reset only when another file gets selected
    if (f != m_sess.file) {
        m_sess.read_ok = false;
        m_sess.write_ok = false;
    }
    m_sess.file = f;
    respond_sw(0x90, 0x00);
}

static void do_read(uint8_t *apdu, uint16_t len, bool extended) {
    if (len < 5) { respond_sw(0x67, 0x00); return; }
    uint16_t off = ((uint16_t)apdu[2] << 8) | apdu[3];
    uint16_t le = apdu[4];
    if (m_sess.file == SEL_NONE) { respond_sw(0x6A, 0x82); return; }
    if (le == 0 || le > ST25TA_MAX_RW) { respond_sw(0x67, 0x00); return; }

    uint8_t *dst = m_tcl_session_state.m_resp_buf;
    if (m_sess.file == SEL_CC) {
        if (off + le > NFC_TAG_ST25TA_CC_SIZE) { respond_sw(0x67, 0x00); return; }
        memcpy(dst, &m_tag_information->cc[off], le);
    } else if (m_sess.file == SEL_SYSTEM) {
        uint8_t sys[ST25TA_SYS_SIZE];
        if (off + le > ST25TA_SYS_SIZE) { respond_sw(0x67, 0x00); return; }
        build_system_file(sys);
        memcpy(dst, &sys[off], le);
    } else {
        if (!read_allowed()) { respond_sw(0x69, 0x82); return; }
        uint16_t limit = m_tag_information->ndef_size;
        if (!extended) {
            // Standard read stays inside the NDEF message
            uint32_t msg_end = (uint32_t)ndef_msg_len() + 2;
            if (msg_end < limit) limit = (uint16_t)msg_end;
        }
        if ((uint32_t)off + le > limit) { respond_sw(0x67, 0x00); return; }
        memcpy(dst, &m_tag_information->ndef[off], le);
    }
    respond(le, 0x90, 0x00);
}

static void do_update(uint8_t *apdu, uint16_t len) {
    if (len < 5) { respond_sw(0x67, 0x00); return; }
    uint16_t off = ((uint16_t)apdu[2] << 8) | apdu[3];
    uint8_t lc = apdu[4];
    if (m_sess.file == SEL_NONE) { respond_sw(0x6A, 0x82); return; }
    if (lc == 0 || lc > ST25TA_MAX_RW || len < 5 + (uint16_t)lc) { respond_sw(0x67, 0x00); return; }
    if (m_sess.file != SEL_NDEF) { respond_sw(0x69, 0x82); return; }
    if (!write_allowed()) { respond_sw(0x69, 0x82); return; }
    if ((uint32_t)off + lc > m_tag_information->ndef_size) { respond_sw(0x6A, 0x84); return; }
    memcpy(&m_tag_information->ndef[off], &apdu[5], lc);
    respond_sw(0x90, 0x00);
}

static void do_verify(uint8_t *apdu, uint16_t len) {
    uint16_t id = ((uint16_t)apdu[2] << 8) | apdu[3];
    uint8_t lc = (len > 4) ? apdu[4] : 0;
    if (m_sess.file != SEL_NDEF) { respond_sw(0x69, 0x85); return; }
    if (id != 0x0001 && id != 0x0002) { respond_sw(0x6A, 0x80); return; }
    bool rd = (id == 0x0001);
    uint8_t acc = m_tag_information->cc[rd ? ST25TA_CC_OFF_READ_ACCESS : ST25TA_CC_OFF_WRITE_ACCESS];
    uint8_t *tries = rd ? &m_sess.read_tries : &m_sess.write_tries;
    bool *ok = rd ? &m_sess.read_ok : &m_sess.write_ok;

    if (lc == 0) {
        if (acc == ST25TA_ACCESS_FREE || *ok) respond_sw(0x90, 0x00);
        else respond_sw(0x63, 0x00);
        return;
    }
    if (lc != NFC_TAG_ST25TA_PWD_SIZE || len < 5 + (uint16_t)lc) { respond_sw(0x6A, 0x80); return; }
    if (*tries == 0) { respond_sw(0x63, 0xC0); return; }
    const uint8_t *pwd = rd ? m_tag_information->pwd_read : m_tag_information->pwd_write;
    if (memcmp(pwd, &apdu[5], NFC_TAG_ST25TA_PWD_SIZE) == 0) {
        *ok = true;
        *tries = ST25TA_PWD_TRIES;
        respond_sw(0x90, 0x00);
    } else {
        (*tries)--;
        respond_sw(0x63, (uint8_t)(0xC0 | *tries));
    }
}

static void do_change_ref(uint8_t *apdu, uint16_t len) {
    uint16_t id = ((uint16_t)apdu[2] << 8) | apdu[3];
    uint8_t lc = (len > 4) ? apdu[4] : 0;
    if (m_sess.file == SEL_NONE) { respond_sw(0x6A, 0x82); return; }
    if (m_sess.file != SEL_NDEF) { respond_sw(0x6A, 0x80); return; }
    if (id != 0x0001 && id != 0x0002) { respond_sw(0x6A, 0x86); return; }
    if (!admin_allowed()) { respond_sw(0x69, 0x82); return; }
    if (lc != NFC_TAG_ST25TA_PWD_SIZE || len < 5 + (uint16_t)lc) { respond_sw(0x6A, 0x80); return; }
    memcpy(id == 0x0001 ? m_tag_information->pwd_read : m_tag_information->pwd_write,
           &apdu[5], NFC_TAG_ST25TA_PWD_SIZE);
    respond_sw(0x90, 0x00);
}

// ins 0x28 enable, 0x26 disable (ISO), 0x28 with CLA A2 = permanent state
static void do_access_change(uint8_t *apdu, bool enable, bool permanent) {
    uint16_t id = ((uint16_t)apdu[2] << 8) | apdu[3];
    if (m_sess.file == SEL_NONE) { respond_sw(0x6A, 0x82); return; }
    if (m_sess.file != SEL_NDEF) { respond_sw(0x6A, 0x80); return; }
    if (id != 0x0001 && id != 0x0002) { respond_sw(0x6A, 0x86); return; }
    if (!admin_allowed()) { respond_sw(0x69, 0x82); return; }

    bool rd = (id == 0x0001);
    uint8_t *acc = &m_tag_information->cc[rd ? ST25TA_CC_OFF_READ_ACCESS : ST25TA_CC_OFF_WRITE_ACCESS];
    if (*acc == ST25TA_READ_FORBIDDEN || *acc == ST25TA_WRITE_FORBIDDEN) {
        respond_sw(0x69, 0x82);
        return;
    }
    if (permanent) *acc = rd ? ST25TA_READ_FORBIDDEN : ST25TA_WRITE_FORBIDDEN;
    else *acc = enable ? ST25TA_ACCESS_LOCKED : ST25TA_ACCESS_FREE;
    respond_sw(0x90, 0x00);
}

static void do_update_file_type(uint8_t *apdu, uint16_t len) {
    if (m_sess.file == SEL_NONE) { respond_sw(0x6A, 0x82); return; }
    if (m_sess.file != SEL_NDEF) { respond_sw(0x6A, 0x80); return; }
    if (len < 6 || apdu[2] != 0x00 || apdu[3] != 0x00 || apdu[4] != 0x01 ||
            (apdu[5] != 0x04 && apdu[5] != 0x05)) {
        respond_sw(0x6A, 0x86);
        return;
    }
    // Needs empty file and free access
    if (ndef_msg_len() != 0 ||
            m_tag_information->cc[ST25TA_CC_OFF_READ_ACCESS] != ST25TA_ACCESS_FREE ||
            m_tag_information->cc[ST25TA_CC_OFF_WRITE_ACCESS] != ST25TA_ACCESS_FREE) {
        respond_sw(0x69, 0x82);
        return;
    }
    m_tag_information->cc[ST25TA_CC_OFF_FILE_TYPE] = apdu[5];
    respond_sw(0x90, 0x00);
}

static void nfc_tag_st25ta_state_handler(uint8_t *data, uint16_t szBytes) {
    if (!nfc_tag_14a_4_base_handler(&m_tcl_session_state, data, szBytes)) return;

    uint8_t *apdu = m_tcl_session_state.m_apdu_buf;
    uint16_t len = m_tcl_session_state.m_apdu_len;
    m_tcl_session_state.m_resp_len = 0;

    if (len < 4) { respond_sw(0x67, 0x00); return; }
    uint8_t cla = apdu[0], ins = apdu[1];

    if (cla == 0x00) {
        switch (ins) {
            case 0xA4: do_select(apdu, len); break;
            case 0xB0: do_read(apdu, len, false); break;
            case 0xD6: do_update(apdu, len); break;
            case 0x20: do_verify(apdu, len); break;
            case 0x24: do_change_ref(apdu, len); break;
            case 0x28: do_access_change(apdu, true, false); break;
            case 0x26: do_access_change(apdu, false, false); break;
            default:   respond_sw(0x6D, 0x00); break;
        }
    } else if (cla == 0xA2) {
        switch (ins) {
            case 0xB0: do_read(apdu, len, true); break;
            case 0x28: do_access_change(apdu, true, true); break;
            case 0xD6: do_update_file_type(apdu, len); break;
            default:   respond_sw(0x6D, 0x00); break;
        }
    } else {
        respond_sw(0x6E, 0x00);
    }
}

static void nfc_tag_st25ta_reset_handler(void) {
    nfc_tag_14a_4_reset_state(&m_tcl_session_state);
    session_reset();
}

static int info_size(const nfc_tag_st25ta_information_t *info) {
    return (int)(ST25TA_HDR_SIZE + info->ndef_size);
}

int nfc_tag_st25ta_data_loadcb(tag_specific_type_t type, tag_data_buffer_t *buffer) {
    if (buffer->length < ST25TA_HDR_SIZE + 2) {
        NRF_LOG_ERROR("ST25TA loadcb: buffer too small");
        return 0;
    }
    m_tag_information = (nfc_tag_st25ta_information_t *)buffer->buffer;
    if (m_tag_information->ndef_size < 2 || m_tag_information->ndef_size > NFC_TAG_ST25TA_NDEF_MAX) {
        m_tag_information->ndef_size = NFC_TAG_ST25TA_NDEF_DEFAULT;
    }
    nfc_tag_st25ta_sync_cc(m_tag_information);
    session_reset();

    nfc_tag_14a_handler_t handler = {
        .get_coll_res = nfc_tag_st25ta_get_coll_res,
        .cb_state     = nfc_tag_st25ta_state_handler,
        .cb_reset     = nfc_tag_st25ta_reset_handler,
    };
    nfc_tag_14a_set_handler(&handler);
    return info_size(m_tag_information);
}

int nfc_tag_st25ta_data_savecb(tag_specific_type_t type, tag_data_buffer_t *buffer) {
    if (m_tag_information == NULL) return 0;
    return info_size(m_tag_information);
}

bool nfc_tag_st25ta_data_factory(uint8_t slot, tag_specific_type_t tag_type) {
    if (tag_type != TAG_TYPE_ST25TA) return false;

    // Build in the shared HF buffer, the record is too big for the stack
    tag_data_buffer_t *buffer = get_buffer_by_tag_type(tag_type);
    if (buffer == NULL || buffer->length < sizeof(nfc_tag_st25ta_information_t)) {
        NRF_LOG_ERROR("ST25TA factory: no buffer for slot %d", slot);
        return false;
    }
    nfc_tag_st25ta_information_t *pinfo = (nfc_tag_st25ta_information_t *)buffer->buffer;
    memset(pinfo, 0, sizeof(*pinfo));

    // 7-byte UID: ST manufacturer code, ST25TA64K product code
    pinfo->res_coll.size    = NFC_TAG_14A_UID_DOUBLE_SIZE;
    pinfo->res_coll.atqa[0] = 0x42;
    pinfo->res_coll.atqa[1] = 0x00;
    pinfo->res_coll.sak[0]  = 0x20;
    static const uint8_t default_uid[] = {0x02, 0xC4, 0x12, 0x34, 0x56, 0x78, 0x9A};
    memcpy(pinfo->res_coll.uid, default_uid, sizeof(default_uid));

    static const uint8_t default_ats[] = {0x05, 0x78, 0x80, 0x90, 0x02};
    pinfo->res_coll.ats.length = sizeof(default_ats);
    memcpy(pinfo->res_coll.ats.data, default_ats, sizeof(default_ats));

    static const uint8_t default_cc[NFC_TAG_ST25TA_CC_SIZE] = {
        0x00, 0x0F, 0x20, 0x00, 0xF6, 0x00, 0xF6,
        0x04, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    };
    memcpy(pinfo->cc, default_cc, sizeof(default_cc));
    pinfo->ndef_size = NFC_TAG_ST25TA_NDEF_DEFAULT;
    nfc_tag_st25ta_sync_cc(pinfo);
    // Empty NDEF message, passwords all zero (delivery state)

    fds_slot_record_map_t map_info;
    get_fds_map_by_slot_sense_type_for_dump(slot, TAG_SENSE_HF, &map_info);
    bool ret = fds_write_sync(map_info.id, map_info.key, info_size(pinfo), pinfo);
    NRF_LOG_INFO("ST25TA factory slot %d: %s", slot, ret ? "OK" : "FAIL");
    return ret;
}
