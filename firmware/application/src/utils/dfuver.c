#include "dfuver.h"

#include <string.h>

#include "bsp_wdt.h"
#include "nrf_delay.h"
#include "nrf_fstorage.h"
#include "nrf_fstorage_sd.h"

#define NRF_LOG_MODULE_NAME dfuver
#include "nrf_log.h"
#include "nrf_log_ctrl.h"
NRF_LOG_MODULE_REGISTER();

/* nrf_dfu_settings_t layout (bl-settings-version 2): crc@0, app_version@8,
 * bootloader_version@0xC; CRC covers [4, offsetof(init_command)) = 0x58.
 * On boot the BL copies the version region from the BACKUP page (0xFE000, =
 * MBR-params page) over the main page, so both pages must carry the change. */
#define SETTINGS_ADDR   0x000FF000UL
#define BACKUP_ADDR     0x000FE000UL
#define PAGE_SIZE       4096UL
#define APP_VER_OFF     0x08UL
#define BL_VER_OFF      0x0CUL
#define CRC_LEN         0x58UL
#define WRITE_STEP      256UL
#define WAIT_US         5000000UL

static void ev(nrf_fstorage_evt_t *p_evt);

NRF_FSTORAGE_DEF(nrf_fstorage_t m_dv_fs) = {
    .evt_handler = ev,
    .start_addr  = BACKUP_ADDR,
    .end_addr    = SETTINGS_ADDR + PAGE_SIZE,
};

static volatile bool m_done;
static volatile ret_code_t m_res;
static bool m_inited;
static uint32_t m_page[PAGE_SIZE / 4];

static void ev(nrf_fstorage_evt_t *p_evt) {
    m_res = p_evt->result;
    m_done = true;
}

static bool wait_done(void) {
    for (uint32_t t = 0; !m_done; t += 100) {
        if (t >= WAIT_US) {
            return false;
        }
        bsp_wdt_feed();
        nrf_delay_us(100);
    }
    return m_res == NRF_SUCCESS;
}

static uint32_t crc32(const uint8_t *p, uint32_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return ~c;
}

static bool write_page(uint32_t addr) {
    m_done = false;
    if (nrf_fstorage_erase(&m_dv_fs, addr, 1, NULL) != NRF_SUCCESS || !wait_done()) {
        return false;
    }
    for (uint32_t off = 0; off < PAGE_SIZE; off += WRITE_STEP) {
        m_done = false;
        if (nrf_fstorage_write(&m_dv_fs, addr + off,
                               (const uint8_t *)m_page + off, WRITE_STEP, NULL) != NRF_SUCCESS ||
                !wait_done()) {
            return false;
        }
    }
    return true;
}

bool dfuver_get(uint32_t *app_ver, uint32_t *bl_ver) {
    const volatile uint32_t *s = (const volatile uint32_t *)SETTINGS_ADDR;
    if (app_ver) {
        *app_ver = s[APP_VER_OFF / 4];
    }
    if (bl_ver) {
        *bl_ver = s[BL_VER_OFF / 4];
    }
    return true;
}

bool dfuver_set(uint8_t mask, uint32_t app_ver, uint32_t bl_ver) {
    if (!(mask & 3u)) {
        return false;
    }
    if (!m_inited) {
        if (nrf_fstorage_init(&m_dv_fs, &nrf_fstorage_sd, NULL) != NRF_SUCCESS) {
            return false;
        }
        m_inited = true;
    }

    /* Preserve the whole page; change only the two counters and the CRC. */
    memcpy(m_page, (const void *)SETTINGS_ADDR, PAGE_SIZE);
    if (mask & 1u) {
        m_page[APP_VER_OFF / 4] = app_ver;
    }
    if (mask & 2u) {
        m_page[BL_VER_OFF / 4] = bl_ver;
    }
    m_page[0] = crc32((const uint8_t *)m_page + 4, CRC_LEN);

    /* Both pages must match or the BL copies the old version back from backup. */
    if (!write_page(SETTINGS_ADDR) || !write_page(BACKUP_ADDR)) {
        return false;
    }
    NRF_LOG_INFO("dfuver: settings versions updated (mask 0x%x)", mask);
    return true;
}
