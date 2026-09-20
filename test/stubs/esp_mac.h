// SPDX-License-Identifier: Unlicense
// Host-test stub: a fixed eFuse MAC so gen_addr is deterministic on the host.
#pragma once
#include <stdint.h>
#include "esp_err.h"
static inline esp_err_t esp_efuse_mac_get_default(uint8_t *mac)
{
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)(0x10 + i);
    return ESP_OK;
}
