// SPDX-License-Identifier: Unlicense
// See somfy_rx.h. A GPIO any-edge interrupt on GDO2 timestamps each edge of the
// CC1101's demodulated OOK stream; the ISR accumulates edge-to-edge durations
// into a bounded burst buffer and hands each burst to the level-agnostic decoder
// (somfy_decode_pulses). This mirrors ESPSomfy-RTS: continuous band noise never
// forms a valid sync run so it is silently discarded, and an edge-timed burst
// buffer has no fixed capture window to overflow.
#include "somfy_rx.h"
#include "somfy_frame.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "somfy_rx";

// One 56-bit frame is a few sync pulses plus ~112 Manchester half-symbols, so
// 160 edge durations cover it; a burst ends on a gap or when the buffer fills.
#define RX_MAXPULSE 160
// Ignore anything shorter than a valid frame (< ~40 durations is a noise
// fragment) so the decoder is not run on obvious junk.
#define RX_MINPULSE 40
// A gap longer than this (us) ends a burst. Somfy's inter-frame gap is ~30ms and
// the widest in-frame interval (software sync) is ~4.8ms, so 6ms cleanly splits
// consecutive frames without cutting one. Calibration knob.
#define RX_GAP_US 6000
// Edges closer together than this (us) are RF glitches — below the half-symbol
// minimum (~448us). Mirroring ESPSomfy, a glitch is merged into the following
// interval (we drop the edge without restarting the timer). Calibration knob.
#define RX_GLITCH_US 200
#define RX_QDEPTH 4

typedef struct {
    int      n;
    uint16_t dur[RX_MAXPULSE];
} rx_burst_t;

static cc1101_t     *s_cc;
static somfy_rx_cb_t s_cb;
static QueueHandle_t s_q;
static rx_burst_t    s_isr;
static int64_t       s_last_us;

/**
 * Any-edge ISR on GDO2. Times the interval since the previous kept edge, merges
 * sub-glitch intervals, and appends the duration to the current burst. On a gap
 * or a full buffer it ships a burst worth decoding and starts a fresh one.
 */
static void IRAM_ATTR rx_isr(void *arg)
{
    int64_t now = esp_timer_get_time();
    uint32_t d = (uint32_t)(now - s_last_us);
    if (d < RX_GLITCH_US) return;
    s_last_us = now;

    if (d > RX_GAP_US || s_isr.n >= RX_MAXPULSE) {
        if (s_isr.n >= RX_MINPULSE) {
            BaseType_t hp = pdFALSE;
            xQueueSendFromISR(s_q, &s_isr, &hp);
            if (hp) portYIELD_FROM_ISR();
        }
        s_isr.n = 0;
    }
    if (s_isr.n < RX_MAXPULSE) s_isr.dur[s_isr.n++] = d > 0xFFFF ? 0xFFFF : (uint16_t)d;
}

/**
 * Drain completed bursts and decode. Runs off the ISR so decode never blocks the
 * edge interrupt; noise bursts simply fail somfy_decode_pulses and are dropped.
 */
static void rx_task(void *arg)
{
    rx_burst_t b;
    while (xQueueReceive(s_q, &b, portMAX_DELAY)) {
        uint32_t addr; uint16_t code; uint8_t cmd;
        if (somfy_decode_pulses(b.dur, b.n, &addr, &code, &cmd) && s_cb)
            s_cb(addr, code, cmd);
    }
}

bool somfy_rx_init(cc1101_t *cc, somfy_rx_cb_t cb)
{
    s_cc = cc;
    s_cb = cb;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CC1101_PIN_GD2,
        .mode         = GPIO_MODE_INPUT,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    if (gpio_config(&io) != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(GD2) failed — RX disabled");
        return false;
    }
    s_q = xQueueCreate(RX_QDEPTH, sizeof(rx_burst_t));
    if (!s_q || xTaskCreate(rx_task, "somfy_rx", 4096, NULL, 6, NULL) != pdPASS) {
        ESP_LOGE(TAG, "rx task/queue create failed — RX disabled");
        return false;
    }
    esp_err_t e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service: %s — RX disabled", esp_err_to_name(e));
        return false;
    }
    if (gpio_isr_handler_add(CC1101_PIN_GD2, rx_isr, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "gpio_isr_handler_add(GD2) failed — RX disabled");
        return false;
    }
    s_last_us = esp_timer_get_time();
    cc1101_enter_rx_mode(cc);
    ESP_LOGI(TAG, "init OK (GD2=IO%d, GPIO-ISR listening)", CC1101_PIN_GD2);
    return true;
}

void somfy_rx_resume(void)
{
    if (s_cc) cc1101_enter_rx_mode(s_cc);
}
