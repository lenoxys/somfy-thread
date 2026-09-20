// SPDX-License-Identifier: Unlicense
// Host-test stub: silence ESP logging off-target.
#pragma once
#define ESP_LOGI(tag, ...) ((void)0)
#define ESP_LOGW(tag, ...) ((void)0)
#define ESP_LOGE(tag, ...) ((void)0)
