// SPDX-License-Identifier: Unlicense
#include "blind_store.h"
#include <string.h>
#include <stdio.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "esp_log.h"

static const char *TAG = "blind_store";
#define NVS_NS   "somfy"
#define NVS_KEY  "shades"
#define NVS_FREQ "freq"

static shade_t s_shades[BLIND_MAX_COUNT];
static float   s_freq_mhz = BOARD_DEFAULT_FREQ_MHZ;

/**
 * Populate the table with factory defaults: a per-device 24-bit address base
 * derived from the low three eFuse-MAC bytes (unique and stable), rolling code
 * 1, active, and name "Shade N". The user PROGs their motors onto these
 * addresses, then may override them via the console.
 */
static void seed_defaults(void)
{
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    uint32_t base = ((uint32_t)mac[3] << 16 | (uint32_t)mac[4] << 8 | mac[5]) & 0xFFFFFF;
    for (int i = 0; i < BLIND_MAX_COUNT; i++) {
        s_shades[i].addr     = (base + i) & 0xFFFFFF;
        s_shades[i].rolling  = 1;
        s_shades[i].active   = true;
        snprintf(s_shades[i].name, sizeof(s_shades[i].name), "Shade %d", i + 1);
    }
}

/**
 * Initialise NVS and load the shade table. Erases and reinitialises NVS if it
 * reports no free pages or a version mismatch. Seeds and persists defaults when
 * no valid blob of the expected size is found.
 */
void blind_store_init(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_shades);
        e = nvs_get_blob(h, NVS_KEY, s_shades, &len);
        size_t flen = sizeof(s_freq_mhz);
        nvs_get_blob(h, NVS_FREQ, &s_freq_mhz, &flen);
        nvs_close(h);
        if (e == ESP_OK && len == sizeof(s_shades)) {
            ESP_LOGI(TAG, "loaded %d shades from NVS (freq %.3f MHz)", BLIND_MAX_COUNT, s_freq_mhz);
            return;
        }
    }
    ESP_LOGI(TAG, "no valid store — seeding defaults");
    seed_defaults();
    blind_store_save();
}

int blind_store_count(void) { return BLIND_MAX_COUNT; }

shade_t *blind_store_get(int idx)
{
    if (idx < 0 || idx >= BLIND_MAX_COUNT) return NULL;
    return &s_shades[idx];
}

/**
 * Write the whole table to NVS and commit. Silently skips if NVS cannot open.
 */
void blind_store_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed — save skipped");
        return;
    }
    nvs_set_blob(h, NVS_KEY, s_shades, sizeof(s_shades));
    nvs_commit(h);
    nvs_close(h);
}

/**
 * Increment a shade's rolling code and persist the table before returning, so a
 * reboot mid-transmit can never rewind below what the motor has already seen.
 * @return The new rolling code, or 0 for an out-of-range index.
 */
uint16_t blind_store_next_rolling(int idx)
{
    shade_t *s = blind_store_get(idx);
    if (!s) return 0;
    s->rolling++;
    blind_store_save();
    return s->rolling;
}

float blind_store_freq(void) { return s_freq_mhz; }

void blind_store_set_freq(float mhz)
{
    s_freq_mhz = mhz;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed — freq save skipped");
        return;
    }
    nvs_set_blob(h, NVS_FREQ, &s_freq_mhz, sizeof(s_freq_mhz));
    nvs_commit(h);
    nvs_close(h);
}
