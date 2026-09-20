// SPDX-License-Identifier: Unlicense
// Host-test stub: an NVS that always "fails to open", so blind_store keeps its
// table purely in RAM (load starts empty, save is a no-op). Enough to exercise
// the slot logic on the host without a flash backend.
#pragma once
#include <stddef.h>
#include "esp_err.h"

typedef unsigned nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;

#define ESP_ERR_NVS_NO_FREE_PAGES     0x1100
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1101

static inline esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h) { (void)ns; (void)m; (void)h; return ESP_FAIL; }
static inline esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *o, size_t *l) { (void)h; (void)k; (void)o; (void)l; return ESP_FAIL; }
static inline esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *o, size_t l) { (void)h; (void)k; (void)o; (void)l; return ESP_OK; }
static inline esp_err_t nvs_get_u8(nvs_handle_t h, const char *k, uint8_t *o) { (void)h; (void)k; (void)o; return ESP_FAIL; }
static inline esp_err_t nvs_set_u8(nvs_handle_t h, const char *k, uint8_t v) { (void)h; (void)k; (void)v; return ESP_OK; }
static inline esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
static inline void      nvs_close(nvs_handle_t h) { (void)h; }
