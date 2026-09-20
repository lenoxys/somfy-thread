// SPDX-License-Identifier: Unlicense
// Somfy RTS transmitter: builds the RMT symbol stream for one frame and keys the
// CC1101 carrier via OOK. Manchester encoding: '1' is LOW-then-HIGH, '0' is
// HIGH-then-LOW.
#include "somfy_rts.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

static const char *TAG = "somfy_rts";

#define MAX_SYMBOLS  96
#define RMT_HALF_MAX 32000u

/**
 * Append one two-phase RMT symbol.
 * @return The next free index.
 */
static int add_pair(rmt_symbol_word_t *b, int i,
                    uint32_t d0, uint8_t l0, uint32_t d1, uint8_t l1)
{
    b[i].duration0 = d0; b[i].level0 = l0;
    b[i].duration1 = d1; b[i].level1 = l1;
    return i + 1;
}

/**
 * Append a single-level span of `us` microseconds, split across symbol halves so
 * no duration field exceeds the 15-bit RMT limit. A trailing half of 1 us at the
 * same level is invisible on air.
 * @return The next free index.
 */
static int add_span(rmt_symbol_word_t *b, int i, uint8_t level, uint32_t us)
{
    while (us > 0) {
        uint32_t d0 = us > RMT_HALF_MAX ? RMT_HALF_MAX : us; us -= d0;
        uint32_t d1 = us > RMT_HALF_MAX ? RMT_HALF_MAX : us; us -= d1;
        b[i].duration0 = d0; b[i].level0 = level;
        b[i].duration1 = d1 ? d1 : 1; b[i].level1 = level;
        i++;
    }
    return i;
}

/**
 * Render one frame into `buf`: an optional wake-up pulse, `hw_sync` hardware-sync
 * cycles, the software sync, the 56 Manchester-encoded data bits (MSB first), and
 * the inter-frame gap.
 * @return The symbol count written.
 */
static int frame_to_rmt(const uint8_t frame[SOMFY_FRAME_BYTES],
                        rmt_symbol_word_t *buf, int hw_sync, bool wake)
{
    int n = 0;
    if (wake) {
        n = add_pair(buf, n, SOMFY_WAKE_HI_US, 1, RMT_HALF_MAX, 0);
        n = add_span(buf, n, 0, SOMFY_WAKE_LO_US - RMT_HALF_MAX);
    }
    for (int i = 0; i < hw_sync; i++)
        n = add_pair(buf, n, SOMFY_HWSYNC_US, 1, SOMFY_HWSYNC_US, 0);
    n = add_pair(buf, n, SOMFY_SWSYNC_HI_US, 1, SOMFY_SWSYNC_LO_US, 0);

    for (int byte = 0; byte < SOMFY_FRAME_BYTES; byte++) {
        for (int bit = 7; bit >= 0; bit--) {
            if ((frame[byte] >> bit) & 1)
                n = add_pair(buf, n, SOMFY_SYMBOL_US, 0, SOMFY_SYMBOL_US, 1);
            else
                n = add_pair(buf, n, SOMFY_SYMBOL_US, 1, SOMFY_SYMBOL_US, 0);
        }
    }
    n = add_span(buf, n, 0, SOMFY_INTERFRAME_US);
    return n;
}

/**
 * Create the RMT TX channel and copy encoder on the GDO0 pin.
 * The 48-symbol memory block is one RMT channel: the copy encoder streams the
 * frame through it in ping-pong halves, which is ample at Somfy's ~600us symbol
 * rate and keeps the channel to one of the C6's four (a 128-symbol buffer spans
 * three). RX no longer uses RMT — it runs off a GPIO edge interrupt.
 * @return false if any RMT resource cannot be created or enabled.
 */
bool somfy_rts_init(somfy_rts_t *ctx, cc1101_t *cc)
{
    ctx->cc = cc;
    rmt_tx_channel_config_t cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = CC1101_PIN_GD0,
        .mem_block_symbols = 48,
        .resolution_hz = 1000000,
        .trans_queue_depth = 4,
    };
    if (rmt_new_tx_channel(&cfg, &ctx->chan) != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_tx_channel failed — RF disabled");
        return false;
    }
    rmt_copy_encoder_config_t enc_cfg = {};
    if (rmt_new_copy_encoder(&enc_cfg, &ctx->enc) != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_copy_encoder failed — RF disabled");
        return false;
    }
    if (rmt_enable(ctx->chan) != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable failed — RF disabled");
        return false;
    }
    ESP_LOGI(TAG, "init OK (GD0=IO%d)", CC1101_PIN_GD0);
    return true;
}

/**
 * Transmit a single rendered frame and wait for completion.
 */
static void tx_frame(somfy_rts_t *ctx, const uint8_t *frame, int hw_sync, bool wake)
{
    static rmt_symbol_word_t buf[MAX_SYMBOLS];
    int n = frame_to_rmt(frame, buf, hw_sync, wake);
    rmt_transmit_config_t tc = { .loop_count = 0 };
    if (rmt_transmit(ctx->chan, ctx->enc, buf, n * sizeof(buf[0]), &tc) == ESP_OK)
        rmt_tx_wait_all_done(ctx->chan, 2000);
}

/**
 * Tune the carrier, key TX, and send the wake-up frame plus `repeats` frames,
 * all reusing the same rolling code so the motor sees one button press.
 */
void somfy_rts_send(somfy_rts_t *ctx, uint32_t addr, uint16_t rolling,
                    float freq_mhz, uint8_t cmd, int repeats)
{
    if (repeats < 1) repeats = 2;

    uint8_t frame[SOMFY_FRAME_BYTES];
    somfy_build_frame(frame, addr, rolling, cmd);
    ESP_LOGI(TAG, "tx addr=0x%06lX rolling=%u cmd=0x%X freq=%.3f",
             (unsigned long)addr, rolling, cmd, freq_mhz);

    cc1101_set_frequency(ctx->cc, freq_mhz);
    cc1101_enter_tx_mode(ctx->cc);
    esp_rom_delay_us(5000);

    tx_frame(ctx, frame, SOMFY_HWSYNC_FIRST, true);
    for (int i = 0; i < repeats; i++)
        tx_frame(ctx, frame, SOMFY_HWSYNC_REPEAT, false);

    cc1101_idle(ctx->cc);
}
