// SPDX-License-Identifier: Unlicense
// Somfy RTS -> Matter-over-Thread bridge for ESP32-C6 + CC1101.
// Exposes BLIND_MAX_COUNT WindowCovering (lift-only) endpoints; each maps to a
// Somfy RTS remote address. Config/backup is done over the USB serial console
// (the contract driven by the WebSerial page). No on-device web server.
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"

#include <esp_matter.h>
#include <esp_matter_console.h>
#include <app/clusters/window-covering-server/window-covering-server.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <app-common/zap-generated/attributes/Accessors.h>
#include <app/server/OnboardingCodesUtil.h>
#include <setup_payload/SetupPayload.h>
#include <setup_payload/QRCodeSetupPayloadGenerator.h>
#include <app/server/Server.h>
#include <app/server/CommissioningWindowManager.h>
#include <credentials/FabricTable.h>
#include <platform/PlatformManager.h>
#include <lib/support/Span.h>
#include <system/SystemClock.h>
#include <esp_openthread_types.h>
#include <platform/ESP32/OpenthreadLauncher.h>

extern "C" {
#include "board.h"
#include "cc1101.h"
#include "somfy_rts.h"
#include "somfy_rx.h"
#include "somfy_frame.h"
#include "blind_store.h"
}

static const char *TAG = "somfy_thread";

using namespace esp_matter;
using namespace esp_matter::endpoint;
namespace WC = chip::app::Clusters::WindowCovering;

static cc1101_t   s_cc;
static somfy_rts_t s_rts;
static bool       s_rf_ok = false;

typedef struct { int idx; uint8_t cmd; } rf_job_t;
static QueueHandle_t s_rf_q;
static uint16_t s_wc_ep_ids[BLIND_MAX_COUNT];
static volatile int64_t s_last_tx_us = 0;

/**
 * Worker that drains the RF queue and transmits. A send is several frames over
 * roughly a second, so it runs here off the Matter/CHIP thread. PROG pairing
 * repeats the frame long enough to emulate a held button; normal commands are a
 * short press. The rolling code is advanced and persisted per send.
 */
static void rf_task(void *arg)
{
    rf_job_t job;
    while (xQueueReceive(s_rf_q, &job, portMAX_DELAY)) {
        shade_t *s = blind_store_get(job.idx);
        if (!s || !s_rf_ok) continue;
        int repeats = (job.cmd == SOMFY_PROG) ? 12 : 3;
        uint16_t rolling = blind_store_next_rolling(job.idx);
        s_last_tx_us = esp_timer_get_time();
        somfy_rts_send(&s_rts, s->addr, rolling, s->freq_mhz, job.cmd, repeats);
        somfy_rx_resume();
    }
}

/**
 * Enqueue an RF command for shade `idx`. Non-blocking; drops if the queue is
 * full or not yet created.
 */
extern "C" void app_rf_submit(int idx, uint8_t cmd)
{
    if (!s_rf_q) return;
    rf_job_t job = { idx, cmd };
    xQueueSend(s_rf_q, &job, 0);
}

/**
 * Delegate for a lift-only WindowCovering. Somfy RTS has no position feedback,
 * so movement is driven by direction inferred from target vs current position,
 * and Current is set equal to Target so the Home Assistant UI settles. CHIP
 * emits HandleStopMotion immediately after a movement; WC_STOP_GUARD_US
 * distinguishes that auto-call from a genuine user stop.
 */
#define WC_STOP_GUARD_US (500 * 1000)
static int64_t  s_last_move_us = 0;
static uint16_t s_last_move_ep = 0xFFFF;

/**
 * Map a Matter endpoint id back to its shade index.
 * @return The index, or -1 if the endpoint is not a shade.
 */
static int ep_to_idx(uint16_t ep)
{
    for (int i = 0; i < BLIND_MAX_COUNT; i++)
        if (s_wc_ep_ids[i] == ep) return i;
    return -1;
}

class SomfyWCDelegate : public WC::Delegate {
public:
    /**
     * Translate a lift movement into a Somfy command. When a current position
     * is known, target above current closes (DOWN) and below opens (UP);
     * otherwise near-fully-closed maps to DOWN, near-fully-open to UP, and mid
     * to MY (favourite). Submits the command and mirrors Current to Target.
     */
    CHIP_ERROR HandleMovement(WC::WindowCoveringType type) override
    {
        if (type != WC::WindowCoveringType::Lift) return CHIP_NO_ERROR;
        chip::app::DataModel::Nullable<chip::Percent100ths> tgt, cur;
        if (WC::Attributes::TargetPositionLiftPercent100ths::Get(mEndpoint, tgt)
                != chip::Protocols::InteractionModel::Status::Success || tgt.IsNull())
            return CHIP_NO_ERROR;
        uint16_t v = tgt.Value();
        bool hasCur = WC::Attributes::CurrentPositionLiftPercent100ths::Get(mEndpoint, cur)
                          == chip::Protocols::InteractionModel::Status::Success && !cur.IsNull();
        uint8_t cmd;
        if (hasCur && cur.Value() != v) cmd = (v > cur.Value()) ? SOMFY_DOWN : SOMFY_UP;
        else if (v >= 9500)             cmd = SOMFY_DOWN;
        else if (v <= 500)              cmd = SOMFY_UP;
        else                            cmd = SOMFY_MY;

        s_last_move_us = esp_timer_get_time();
        s_last_move_ep = mEndpoint;
        int idx = ep_to_idx(mEndpoint);
        ESP_LOGI(TAG, "[WC] ep=%u idx=%d lift=%u%% cmd=0x%X", mEndpoint, idx, v / 100, cmd);
        app_rf_submit(idx, cmd);
        WC::Attributes::CurrentPositionLiftPercent100ths::Set(mEndpoint, tgt);
        return CHIP_NO_ERROR;
    }

    /**
     * Send the Somfy MY (stop) command for a genuine user stop, ignoring the
     * auto-call CHIP emits right after a movement (see WC_STOP_GUARD_US).
     */
    CHIP_ERROR HandleStopMotion() override
    {
        int64_t since = esp_timer_get_time() - s_last_move_us;
        if (s_last_move_ep == mEndpoint && since < WC_STOP_GUARD_US)
            return CHIP_NO_ERROR;
        int idx = ep_to_idx(mEndpoint);
        ESP_LOGI(TAG, "[WC] ep=%u idx=%d stop -> MY", mEndpoint, idx);
        app_rf_submit(idx, SOMFY_MY);
        return CHIP_NO_ERROR;
    }
};
static SomfyWCDelegate s_wc_delegates[BLIND_MAX_COUNT];

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    if (event->Type == chip::DeviceLayer::DeviceEventType::kCommissioningComplete)
        ESP_LOGI(TAG, "Commissioning complete");
}

static esp_err_t app_identification_cb(identification::callback_type_t, uint16_t, uint8_t, uint8_t, void *)
{
    return ESP_OK;
}

/**
 * Attribute-update hook. WindowCovering commands are serviced by the delegate,
 * so nothing is done here.
 */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t, uint16_t, uint32_t, uint32_t,
                                         esp_matter_attr_val_t *, void *)
{
    return ESP_OK;
}

/**
 * @return The Matter QR-code payload string, or "" if it cannot be generated.
 */
extern "C" const char *app_matter_qr(void)
{
    static char qr[chip::QRCodeBasicSetupPayloadGenerator::kMaxQRCodeBase38RepresentationLength + 1] = {0};
    chip::MutableCharSpan span(qr);
    if (::GetQRCode(span, chip::RendezvousInformationFlag::kBLE) == CHIP_NO_ERROR) return qr;
    return "";
}

/**
 * @return The Matter manual pairing code, or "" if it cannot be generated.
 */
extern "C" const char *app_matter_manual(void)
{
    static char code[chip::kManualSetupLongCodeCharLength + 1] = {0};
    chip::MutableCharSpan span(code);
    if (::GetManualPairingCode(span, chip::RendezvousInformationFlag::kBLE) == CHIP_NO_ERROR) return code;
    return "";
}

static void open_cw_work(intptr_t)
{
    auto &cwm = chip::Server::GetInstance().GetCommissioningWindowManager();
    if (!cwm.IsCommissioningWindowOpen())
        cwm.OpenBasicCommissioningWindow(chip::System::Clock::Seconds16(15 * 60));
}

/**
 * Open a 15-minute basic commissioning window, scheduled on the Matter thread.
 */
extern "C" void app_matter_open_window(void)
{
    chip::DeviceLayer::PlatformMgr().ScheduleWork(open_cw_work, 0);
}

static void factory_reset_work(intptr_t)
{
    auto &ft = chip::Server::GetInstance().GetFabricTable();
    chip::FabricIndex idxs[CHIP_CONFIG_MAX_FABRICS];
    uint8_t n = 0;
    for (const auto &fb : ft)
        if (n < CHIP_CONFIG_MAX_FABRICS) idxs[n++] = fb.GetFabricIndex();
    for (uint8_t i = 0; i < n; i++) ft.Delete(idxs[i]);
    esp_restart();
}

/**
 * Delete every Matter fabric and reboot, scheduled on the Matter thread.
 */
extern "C" void app_matter_factory_reset(void)
{
    chip::DeviceLayer::PlatformMgr().ScheduleWork(factory_reset_work, 0);
}

/**
 * Parse a command word into a somfy_cmd; "stop" is an alias for MY.
 * @return The command byte, or 0 if unrecognised.
 */
static uint8_t parse_cmd(const char *s)
{
    if (!strcasecmp(s, "up"))   return SOMFY_UP;
    if (!strcasecmp(s, "down")) return SOMFY_DOWN;
    if (!strcasecmp(s, "my") || !strcasecmp(s, "stop")) return SOMFY_MY;
    if (!strcasecmp(s, "prog")) return SOMFY_PROG;
    return 0;
}

/**
 * Print the whole shade table as a JSON array on one line. This is what `list`
 * and `export` emit, and what the WebSerial page parses for display and backup.
 */
static void print_shades_json(void)
{
    printf("[");
    for (int i = 0; i < BLIND_MAX_COUNT; i++) {
        shade_t *s = blind_store_get(i);
        printf("%s{\"idx\":%d,\"name\":\"%s\",\"addr\":\"%06lX\",\"rolling\":%u,\"freq\":%.3f,\"active\":%s}",
               i ? "," : "", i, s->name, (unsigned long)s->addr, s->rolling, s->freq_mhz,
               s->active ? "true" : "false");
    }
    printf("]\n");
}

static int cmd_list(int, char **) { print_shades_json(); return 0; }

static int cmd_tx(int argc, char **argv)
{
    if (argc < 3) { printf("ERR usage: tx <idx> <up|down|my|stop|prog>\n"); return 1; }
    int idx = atoi(argv[1]);
    uint8_t cmd = parse_cmd(argv[2]);
    if (!blind_store_get(idx) || !cmd) { printf("ERR bad idx/cmd\n"); return 1; }
    app_rf_submit(idx, cmd);
    printf("OK\n");
    return 0;
}

/**
 * `name <idx> <text...>` — set a shade's display name, joining the remaining
 * arguments with spaces so multi-word names work.
 */
static int cmd_name(int argc, char **argv)
{
    if (argc < 3) { printf("ERR usage: name <idx> <text>\n"); return 1; }
    shade_t *s = blind_store_get(atoi(argv[1]));
    if (!s) { printf("ERR bad idx\n"); return 1; }
    s->name[0] = 0;
    for (int i = 2; i < argc; i++) {
        if (i > 2) strncat(s->name, " ", sizeof(s->name) - strlen(s->name) - 1);
        strncat(s->name, argv[i], sizeof(s->name) - strlen(s->name) - 1);
    }
    blind_store_save();
    printf("OK\n");
    return 0;
}

static int cmd_freq(int argc, char **argv)
{
    if (argc < 3) { printf("ERR usage: freq <idx> <mhz>\n"); return 1; }
    shade_t *s = blind_store_get(atoi(argv[1]));
    if (!s) { printf("ERR bad idx\n"); return 1; }
    float f = strtof(argv[2], NULL);
    if (f < BOARD_FREQ_MIN_MHZ || f > BOARD_FREQ_MAX_MHZ) { printf("ERR freq out of band\n"); return 1; }
    s->freq_mhz = f;
    blind_store_save();
    printf("OK\n");
    return 0;
}

static int cmd_addr(int argc, char **argv)
{
    if (argc < 3) { printf("ERR usage: addr <idx> <hex24>\n"); return 1; }
    shade_t *s = blind_store_get(atoi(argv[1]));
    if (!s) { printf("ERR bad idx\n"); return 1; }
    s->addr = (uint32_t)strtoul(argv[2], NULL, 16) & 0xFFFFFF;
    blind_store_save();
    printf("OK\n");
    return 0;
}

static int cmd_roll(int argc, char **argv)
{
    if (argc < 3) { printf("ERR usage: roll <idx> <value>\n"); return 1; }
    shade_t *s = blind_store_get(atoi(argv[1]));
    if (!s) { printf("ERR bad idx\n"); return 1; }
    s->rolling = (uint16_t)strtoul(argv[2], NULL, 10);
    blind_store_save();
    printf("OK\n");
    return 0;
}

static int cmd_active(int argc, char **argv)
{
    if (argc < 3) { printf("ERR usage: active <idx> <0|1>\n"); return 1; }
    shade_t *s = blind_store_get(atoi(argv[1]));
    if (!s) { printf("ERR bad idx\n"); return 1; }
    s->active = atoi(argv[2]) != 0;
    blind_store_save();
    printf("OK\n");
    return 0;
}

static int cmd_version(int, char **) { printf("somfy-thread %s\n", esp_app_get_description()->version); return 0; }
static int cmd_export(int, char **) { print_shades_json(); return 0; }
static int cmd_qr(int, char **)     { printf("%s\n", app_matter_qr()); return 0; }
static int cmd_pair(int, char **)   { app_matter_open_window(); printf("%s\n", app_matter_manual()); return 0; }
static int cmd_reset(int, char **)  { printf("OK resetting\n"); app_matter_factory_reset(); return 0; }

/**
 * Register the serial console commands that form the WebSerial contract.
 */
static void register_console(void)
{
    const esp_console_cmd_t cmds[] = {
        {"version","Print firmware id and version",          NULL, &cmd_version, NULL},
        {"list",   "List shades as JSON",                    NULL, &cmd_list,   NULL},
        {"tx",     "tx <idx> <up|down|my|stop|prog>",        NULL, &cmd_tx,     NULL},
        {"name",   "name <idx> <text>",                      NULL, &cmd_name,   NULL},
        {"freq",   "freq <idx> <mhz>",                       NULL, &cmd_freq,   NULL},
        {"addr",   "addr <idx> <hex24>",                     NULL, &cmd_addr,   NULL},
        {"roll",   "roll <idx> <value>",                     NULL, &cmd_roll,   NULL},
        {"active", "active <idx> <0|1>",                     NULL, &cmd_active, NULL},
        {"export", "Dump full shade table (backup) as JSON", NULL, &cmd_export, NULL},
        {"qr",     "Print Matter QR payload",                NULL, &cmd_qr,     NULL},
        {"pair",   "Open commissioning window, print code",  NULL, &cmd_pair,   NULL},
        {"reset",  "Factory-reset Matter and reboot",        NULL, &cmd_reset,  NULL},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        esp_console_cmd_register(&cmds[i]);
}

/**
 * Set a shade's lift position on the Matter thread. Argument packs endpoint id
 * in the high 16 bits and position (0..10000) in the low 16.
 */
static void rx_pos_work(intptr_t arg)
{
    uint16_t ep = (uint16_t)((uint32_t)arg >> 16);
    chip::app::DataModel::Nullable<chip::Percent100ths> pos((uint16_t)(arg & 0xFFFF));
    WC::Attributes::CurrentPositionLiftPercent100ths::Set(ep, pos);
    WC::Attributes::TargetPositionLiftPercent100ths::Set(ep, pos);
}

/**
 * Receive-frame handler (called from the RX task). Logs every decoded frame —
 * this is the sniffer, and unknown addresses reveal remotes to pair/import. For
 * a known active shade it advances the rolling-code floor (so our next transmit
 * is not stale-rejected) and mirrors the manual movement into Matter so Home
 * Assistant reflects a remote used outside the automation. Frames within
 * WC_RX_ECHO_GUARD_US of our own transmit are ignored as self-reception.
 */
#define WC_RX_ECHO_GUARD_US (1500 * 1000)
extern "C" void app_on_rx_frame(uint32_t addr, uint16_t code, uint8_t cmd)
{
    int idx = -1;
    for (int i = 0; i < BLIND_MAX_COUNT; i++) {
        shade_t *s = blind_store_get(i);
        if (s && s->active && s->addr == addr) { idx = i; break; }
    }
    ESP_LOGI(TAG, "[RX] addr=0x%06lX code=%u cmd=0x%X idx=%d",
             (unsigned long)addr, code, cmd, idx);
    if (idx < 0) return;
    if (esp_timer_get_time() - s_last_tx_us < WC_RX_ECHO_GUARD_US) return;

    shade_t *s = blind_store_get(idx);
    if (code > s->rolling) { s->rolling = code; blind_store_save(); }

    uint16_t pos;
    if (cmd == SOMFY_UP)        pos = 0;
    else if (cmd == SOMFY_DOWN) pos = 10000;
    else                        return;  // MY/PROG/other: position unknown
    intptr_t arg = ((intptr_t)s_wc_ep_ids[idx] << 16) | pos;
    chip::DeviceLayer::PlatformMgr().ScheduleWork(rx_pos_work, arg);
}

/**
 * Bring up storage, RF, the RF worker, the Matter node with one lift-only
 * WindowCovering endpoint per shade, and the serial console; then log the
 * onboarding codes. RF failure is non-fatal — Matter and config still run.
 */
extern "C" void app_main(void)
{
    blind_store_init();

    s_rf_ok = cc1101_init(&s_cc);
    if (s_rf_ok) s_rf_ok = somfy_rts_init(&s_rts, &s_cc);
    if (!s_rf_ok) ESP_LOGW(TAG, "RF disabled (CC1101 absent?) — Matter/config still run");
    else if (!somfy_rx_init(&s_cc, app_on_rx_frame))
        ESP_LOGW(TAG, "RX disabled — manual-remote sync unavailable, TX still works");

    s_rf_q = xQueueCreate(8, sizeof(rf_job_t));
    xTaskCreate(rf_task, "rf", 4096, NULL, 5, NULL);

    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    for (int i = 0; i < BLIND_MAX_COUNT; i++) {
        window_covering_device::config_t wc;
        wc.window_covering.type = 0x00;
        wc.window_covering.delegate = &s_wc_delegates[i];
        endpoint_t *ep = window_covering_device::create(node, &wc, ENDPOINT_FLAG_NONE, NULL);
        if (!ep) { ESP_LOGE(TAG, "endpoint %d create failed", i); continue; }

        cluster_t *wc_cluster = cluster::get(ep, WC::Id);
        cluster::window_covering::feature::lift::config_t lift_cfg;
        cluster::window_covering::feature::lift::add(wc_cluster, &lift_cfg);
        cluster::window_covering::feature::position_aware_lift::config_t pal_cfg;
        pal_cfg.current_position_lift_percent_100ths = nullable<uint16_t>(0);
        pal_cfg.target_position_lift_percent_100ths = nullable<uint16_t>(0);
        cluster::window_covering::feature::position_aware_lift::add(wc_cluster, &pal_cfg);
        uint16_t id = endpoint::get_id(ep);
        s_wc_ep_ids[i] = id;
        s_wc_delegates[i].SetEndpoint(id);
    }

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    esp_openthread_platform_config_t ot_config = {
        .radio_config = { .radio_mode = RADIO_MODE_NATIVE },
        .host_config  = { .host_connection_mode = HOST_CONNECTION_MODE_NONE },
        .port_config  = { .storage_partition_name = "nvs", .netif_queue_size = 10, .task_queue_size = 10 },
    };
    set_openthread_platform_config(&ot_config);
#endif

    esp_matter::start(app_event_cb);

    esp_matter::console::init();
    register_console();

    ESP_LOGI(TAG, "Matter QR: %s", app_matter_qr());
    ESP_LOGI(TAG, "Matter manual code: %s", app_matter_manual());
}
