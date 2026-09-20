// SPDX-License-Identifier: Unlicense
// Host-test stub: just enough of esp_err for blind_store.c to compile off-target.
#pragma once
typedef int esp_err_t;
#define ESP_OK    0
#define ESP_FAIL -1
