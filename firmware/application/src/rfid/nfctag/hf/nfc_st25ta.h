#ifndef NFC_ST25TA_H
#define NFC_ST25TA_H

#include "nfc_14a.h"
#include "tag_emulation.h"

#define NFC_TAG_ST25TA_CC_SIZE      15
#define NFC_TAG_ST25TA_PWD_SIZE     16
#define NFC_TAG_ST25TA_NDEF_MAX     7680    // file size incl. 2-byte length
#define NFC_TAG_ST25TA_NDEF_DEFAULT 256

// CC file offsets
#define ST25TA_CC_OFF_FILE_TYPE     7
#define ST25TA_CC_OFF_FILE_SIZE     11
#define ST25TA_CC_OFF_READ_ACCESS   13
#define ST25TA_CC_OFF_WRITE_ACCESS  14

// Access states
#define ST25TA_ACCESS_FREE          0x00
#define ST25TA_ACCESS_LOCKED        0x80
#define ST25TA_READ_FORBIDDEN       0xFE
#define ST25TA_WRITE_FORBIDDEN      0xFF

typedef struct __attribute__((packed)) {
    nfc_tag_14a_coll_res_entity_t res_coll;
    uint8_t  cc[NFC_TAG_ST25TA_CC_SIZE];
    uint8_t  pwd_read[NFC_TAG_ST25TA_PWD_SIZE];
    uint8_t  pwd_write[NFC_TAG_ST25TA_PWD_SIZE];
    uint16_t ndef_size;
    uint8_t  ndef[NFC_TAG_ST25TA_NDEF_MAX];
}
nfc_tag_st25ta_information_t;

nfc_tag_14a_coll_res_reference_t *nfc_tag_st25ta_get_coll_res(void);

// Rebuild the CC file size field from ndef_size
void nfc_tag_st25ta_sync_cc(nfc_tag_st25ta_information_t *info);

int  nfc_tag_st25ta_data_loadcb(tag_specific_type_t type, tag_data_buffer_t *buffer);
int  nfc_tag_st25ta_data_savecb(tag_specific_type_t type, tag_data_buffer_t *buffer);
bool nfc_tag_st25ta_data_factory(uint8_t slot, tag_specific_type_t tag_type);

#endif
