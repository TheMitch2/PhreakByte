#ifndef MF1_DICT_H
#define MF1_DICT_H

#include <stdint.h>
#include <stddef.h>

// Common MIFARE Classic keys shared by the standalone reader modes (FF first).
extern const uint8_t MF1_DICT[][6];
extern const size_t  MF1_DICT_COUNT;

#endif /* MF1_DICT_H */
