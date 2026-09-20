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
 * Count the slots in use (addr != 0), for logging only.
 */
static int used_count(void)
{
    int n = 0;
    for (int i = 0; i < BLIND_MAX_COUNT; i++)
        if (s_shades[i].addr) n++;
    return n;
}

/**
 * Initialise NVS and load the slot table. Erases and reinitialises NVS if it
 * reports no free pages or a version mismatch. Starts empty (all slots zeroed)
 * whenever no blob of the current layout is found — including after a firmware
 * upgrade that changed the shade struct, since the blob size then differs.
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
            ESP_LOGI(TAG, "loaded %d shades from NVS (freq %.3f MHz)", used_count(), s_freq_mhz);
            return;
        }
    }
    ESP_LOGI(TAG, "no valid store — starting empty");
    memset(s_shades, 0, sizeof(s_shades));
    blind_store_save();
}

int blind_store_count(void) { return BLIND_MAX_COUNT; }

bool blind_store_used(int idx)
{
    return idx >= 0 && idx < BLIND_MAX_COUNT && s_shades[idx].addr != 0;
}

int blind_store_add(uint32_t addr, uint16_t rolling, const char *name)
{
    for (int i = 0; i < BLIND_MAX_COUNT; i++) {
        if (s_shades[i].addr) continue;
        shade_t *s = &s_shades[i];
        s->addr    = addr & 0xFFFFFF;
        s->rolling = rolling;
        s->ep_id   = 0;
        s->enabled = true;
        s->name[0] = 0;
        if (name) strncat(s->name, name, sizeof(s->name) - 1);
        blind_store_save();
        return i;
    }
    return -1;
}

void blind_store_remove(int idx)
{
    if (idx < 0 || idx >= BLIND_MAX_COUNT) return;
    memset(&s_shades[idx], 0, sizeof(s_shades[idx]));
    blind_store_save();
}

/**
 * Derive a 24-bit base from the low three eFuse-MAC bytes (unique and stable)
 * and return the first base+offset not already used by any slot.
 */
uint32_t blind_store_gen_addr(void)
{
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    uint32_t base = ((uint32_t)mac[3] << 16 | (uint32_t)mac[4] << 8 | mac[5]) & 0xFFFFFF;
    for (uint32_t off = 0; off < 0x10000; off++) {
        uint32_t cand = (base + off) & 0xFFFFFF;
        if (!cand) continue;
        bool taken = false;
        for (int i = 0; i < BLIND_MAX_COUNT; i++)
            if (s_shades[i].addr == cand) { taken = true; break; }
        if (!taken) return cand;
    }
    return base ? base : 1;
}

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
