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
#define NVS_LINK "links"
#define NVS_POWER "power"
#define NVS_RXBW  "rxbw"
#define NVS_AGG   "agg_ep"

/** One monitored physical remote per shade (RX-only; 0 = none). */
typedef struct {
    uint32_t addr;
    uint16_t roll;
} link_t;

static shade_t s_shades[BLIND_MAX_COUNT];
static link_t  s_links[BLIND_MAX_COUNT];
static float   s_freq_mhz = BOARD_DEFAULT_FREQ_MHZ;
static uint8_t s_tx_power  = 7;  /* +10 dBm: top index of the CC1101 power table */
static uint8_t s_rxbw      = 2;  /* ~203 kHz: matches the init MDMCFG4 = 0x8A */
static uint16_t s_agg_ep   = 0;

/**
 * Persist the linked-remote table to its own NVS blob, independent of the shade
 * table so the two layouts never interfere.
 */
static void save_links(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed — link save skipped");
        return;
    }
    nvs_set_blob(h, NVS_LINK, s_links, sizeof(s_links));
    nvs_commit(h);
    nvs_close(h);
}

/**
 * Erase the whole Somfy NVS namespace (shades, links, radio settings). The next
 * boot re-inits it empty. Matter/Thread state is cleared separately.
 */
void blind_store_factory_erase(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
}

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
        nvs_get_u8(h, NVS_POWER, &s_tx_power);
        nvs_get_u8(h, NVS_RXBW, &s_rxbw);
        nvs_get_u16(h, NVS_AGG, &s_agg_ep);
        size_t llen = sizeof(s_links);
        if (nvs_get_blob(h, NVS_LINK, s_links, &llen) != ESP_OK || llen != sizeof(s_links))
            memset(s_links, 0, sizeof(s_links));
        nvs_close(h);
        if (e == ESP_OK && len == sizeof(s_shades)) {
            ESP_LOGI(TAG, "loaded %d shades from NVS (freq %.3f MHz)", used_count(), s_freq_mhz);
            return;
        }
    }
    ESP_LOGI(TAG, "no valid store — starting empty");
    memset(s_shades, 0, sizeof(s_shades));
    memset(s_links, 0, sizeof(s_links));
    blind_store_save();
}

bool blind_store_used(int idx)
{
    return idx >= 0 && idx < BLIND_MAX_COUNT && s_shades[idx].addr != 0;
}

int blind_store_add(uint32_t addr, uint16_t rolling, const char *name)
{
    for (int i = 0; i < BLIND_MAX_COUNT; i++) {
        if (s_shades[i].addr) continue;
        shade_t *s = &s_shades[i];
        *s = (shade_t) {
            .addr = addr & 0xFFFFFF,
            .rolling = rolling,
            .enabled = true,
            .my_pct = SHADE_MY_UNSET,
        };
        if (name) strncat(s->name, name, sizeof(s->name) - 1);
        return i;
    }
    return -1;
}

void blind_store_remove(int idx)
{
    if (idx < 0 || idx >= BLIND_MAX_COUNT) return;
    memset(&s_shades[idx], 0, sizeof(s_shades[idx]));
    memset(&s_links[idx], 0, sizeof(s_links[idx]));
    blind_store_save();
    save_links();
}

uint32_t blind_store_link_addr(int idx)
{
    if (idx < 0 || idx >= BLIND_MAX_COUNT) return 0;
    return s_links[idx].addr;
}

void blind_store_set_link(int idx, uint32_t addr, uint16_t rolling)
{
    if (idx < 0 || idx >= BLIND_MAX_COUNT) return;
    s_links[idx].addr = addr & 0xFFFFFF;
    s_links[idx].roll = rolling;
    save_links();
}

void blind_store_link_seen(int idx, uint16_t code)
{
    if (idx < 0 || idx >= BLIND_MAX_COUNT || !s_links[idx].addr) return;
    if (code > s_links[idx].roll) { s_links[idx].roll = code; save_links(); }
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

uint8_t blind_store_tx_power(void) { return s_tx_power; }

void blind_store_set_tx_power(uint8_t idx)
{
    s_tx_power = idx;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_POWER, idx);
    nvs_commit(h);
    nvs_close(h);
}

uint16_t blind_store_agg_ep(void) { return s_agg_ep; }

void blind_store_set_agg_ep(uint16_t ep_id)
{
    s_agg_ep = ep_id;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u16(h, NVS_AGG, ep_id);
    nvs_commit(h);
    nvs_close(h);
}

uint8_t blind_store_rxbw(void) { return s_rxbw; }

void blind_store_set_rxbw(uint8_t idx)
{
    s_rxbw = idx;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_RXBW, idx);
    nvs_commit(h);
    nvs_close(h);
}
