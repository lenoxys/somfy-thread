// SPDX-License-Identifier: Unlicense
// See somfy_rx.h. RMT RX delivers {level,duration} symbol pairs; the decoder is
// level-agnostic (like ESPSomfy), so we flatten each capture to a duration list
// and hand it to somfy_decode_pulses.
#include "somfy_rx.h"
#include "somfy_frame.h"
#include "esp_log.h"
#include "driver/rmt_rx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "somfy_rx";

#define RX_MEM_SYMBOLS 64
#define RX_BUF_SYMBOLS 256

static cc1101_t            *s_cc;
static somfy_rx_cb_t        s_cb;
static rmt_channel_handle_t s_chan;
static QueueHandle_t        s_q;
static rmt_symbol_word_t    s_buf[RX_BUF_SYMBOLS];

// A short signal (<150us) is a glitch; a gap over 7ms ends a capture, so each
// delivered buffer holds one frame's worth of pulses (inter-frame gap is 30ms).
static const rmt_receive_config_t s_rxcfg = {
    .signal_range_min_ns = 150000,
    .signal_range_max_ns = 7000000,
};

static bool IRAM_ATTR on_recv_done(rmt_channel_handle_t chan,
                                   const rmt_rx_done_event_data_t *edata, void *user)
{
    BaseType_t hp = pdFALSE;
    xQueueSendFromISR(s_q, edata, &hp);
    return hp == pdTRUE;
}

/**
 * Drain completed captures, flatten to microsecond durations, decode, and
 * re-arm the receiver. Runs off the ISR so decode never blocks capture.
 */
static void rx_task(void *arg)
{
    static uint16_t durs[RX_BUF_SYMBOLS * 2];
    rmt_receive(s_chan, s_buf, sizeof(s_buf), &s_rxcfg);
    rmt_rx_done_event_data_t e;
    while (xQueueReceive(s_q, &e, portMAX_DELAY)) {
        int n = 0;
        for (size_t i = 0; i < e.num_symbols; i++) {
            if (e.received_symbols[i].duration0) durs[n++] = e.received_symbols[i].duration0;
            if (e.received_symbols[i].duration1) durs[n++] = e.received_symbols[i].duration1;
        }
        uint32_t addr; uint16_t code; uint8_t cmd;
        if (somfy_decode_pulses(durs, n, &addr, &code, &cmd) && s_cb)
            s_cb(addr, code, cmd);
        rmt_receive(s_chan, s_buf, sizeof(s_buf), &s_rxcfg);
    }
}

bool somfy_rx_init(cc1101_t *cc, somfy_rx_cb_t cb)
{
    s_cc = cc;
    s_cb = cb;

    rmt_rx_channel_config_t cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,
        .mem_block_symbols = RX_MEM_SYMBOLS,
        .gpio_num = CC1101_PIN_GD2,
    };
    if (rmt_new_rx_channel(&cfg, &s_chan) != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_rx_channel failed — RX disabled");
        return false;
    }
    rmt_rx_event_callbacks_t cbs = { .on_recv_done = on_recv_done };
    if (rmt_rx_register_event_callbacks(s_chan, &cbs, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "rx callback register failed — RX disabled");
        return false;
    }
    if (rmt_enable(s_chan) != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable(rx) failed — RX disabled");
        return false;
    }
    s_q = xQueueCreate(4, sizeof(rmt_rx_done_event_data_t));
    if (!s_q || xTaskCreate(rx_task, "somfy_rx", 4096, NULL, 6, NULL) != pdPASS) {
        ESP_LOGE(TAG, "rx task/queue create failed — RX disabled");
        return false;
    }
    cc1101_enter_rx_mode(cc);
    ESP_LOGI(TAG, "init OK (GD2=IO%d, listening)", CC1101_PIN_GD2);
    return true;
}

void somfy_rx_resume(void)
{
    if (s_cc) cc1101_enter_rx_mode(s_cc);
}
