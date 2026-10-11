#ifndef DFUVER_H
#define DFUVER_H

#include <stdint.h>
#include <stdbool.h>

/* Read the Secure-DFU version counters from the bootloader settings page. */
bool dfuver_get(uint32_t *app_ver, uint32_t *bl_ver);

/* Set the settings-page version counters (mask b0=app, b1=bl), recompute the
 * settings CRC and rewrite both the main (0xFF000) and backup (0xFE000) pages.
 * Both are required: on boot the BL copies the version region from the backup
 * over the main page, so a main-only write reverts on the next reset. */
bool dfuver_set(uint8_t mask, uint32_t app_ver, uint32_t bl_ver);

#endif
