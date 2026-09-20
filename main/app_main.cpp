// SPDX-License-Identifier: Unlicense
// Somfy RTS -> Matter-over-Thread bridge for ESP32-C6 + CC1101.
// Exposes one lift-only WindowCovering endpoint per shade, created on demand as
// shades are added (up to BLIND_MAX_COUNT). Each shade owns a persisted Matter
// endpoint id resumed on every boot, so its identity is stable across reboots
// and across removal of other shades. Config/backup is done over the USB serial
// console (the contract driven by the WebSerial page). No on-device web server.
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
static node_t     *s_node;
static uint16_t    s_wc_ep_ids[BLIND_MAX_COUNT];
static endpoint_t *s_wc_eps[BLIND_MAX_COUNT];
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
        somfy_rts_send(&s_rts, s->addr, rolling, blind_store_freq(), job.cmd, repeats);
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

/**
 * Attach the lift-only WindowCovering device type (Identify/Groups/Scenes/WC
 * plus the lift + position-aware-lift features) to a bare endpoint, binding
 * shade `idx`'s delegate. Mirrors the feature set the firmware has always used.
 */
static void wc_add_clusters(endpoint_t *ep, int idx)
{
    window_covering_device::config_t wc;
    wc.window_covering.type = 0x00;
    wc.window_covering.delegate = &s_wc_delegates[idx];
    window_covering_device::add(ep, &wc);

    cluster_t *wc_cluster = cluster::get(ep, WC::Id);
    cluster::window_covering::feature::lift::config_t lift_cfg;
    cluster::window_covering::feature::lift::add(wc_cluster, &lift_cfg);
    cluster::window_covering::feature::position_aware_lift::config_t pal_cfg;
    pal_cfg.current_position_lift_percent_100ths = nullable<uint16_t>(0);
    pal_cfg.target_position_lift_percent_100ths = nullable<uint16_t>(0);
    cluster::window_covering::feature::position_aware_lift::add(wc_cluster, &pal_cfg);
}

/**
 * Bring shade `idx`'s Matter endpoint online: resume its persisted endpoint id
 * if it has one (stable identity across reboots and removals), otherwise create
 * a fresh id and persist it. Records the endpoint pointer/id, binds the
 * delegate, and enables it so the controller sees the new cover. Caller must
 * hold the CHIP stack lock. Does NOT persist the store — caller decides when.
 * @return true on success.
 */
static bool wc_endpoint_up(int idx)
{
    shade_t *s = blind_store_get(idx);
    if (!s) return false;
    endpoint_t *ep = s->ep_id
        ? endpoint::resume(s_node, ENDPOINT_FLAG_DESTROYABLE, s->ep_id, NULL)
        : endpoint::create(s_node, ENDPOINT_FLAG_DESTROYABLE, NULL);
    if (!ep) return false;
    wc_add_clusters(ep, idx);
    uint16_t id = endpoint::get_id(ep);
    s->ep_id          = id;
    s_wc_eps[idx]     = ep;
    s_wc_ep_ids[idx]  = id;
    s_wc_delegates[idx].SetEndpoint(id);
    return endpoint::enable(ep) == ESP_OK;
}

/**
 * Take shade `idx`'s endpoint off Thread by destroying it. The persisted
 * `ep_id` is kept in the store so a later re-enable resumes the same identity.
 * Caller must hold the CHIP stack lock.
 */
static void wc_endpoint_down(int idx)
{
    if (!s_wc_eps[idx]) return;
    endpoint::destroy(s_node, s_wc_eps[idx]);
    s_wc_eps[idx]    = NULL;
    s_wc_ep_ids[idx] = 0;
}

/**
 * Take the CHIP stack lock and run wc_endpoint_up / _down. Endpoint lifecycle
 * ops must not race the Matter thread, and the console runs off it.
 */
static bool locked_endpoint_up(int idx)
{
    if (esp_matter::lock::chip_stack_lock(portMAX_DELAY) != esp_matter::lock::SUCCESS) return false;
    bool ok = wc_endpoint_up(idx);
    esp_matter::lock::chip_stack_unlock();
    return ok;
}
static void locked_endpoint_down(int idx)
{
    if (esp_matter::lock::chip_stack_lock(portMAX_DELAY) != esp_matter::lock::SUCCESS) return;
    wc_endpoint_down(idx);
    esp_matter::lock::chip_stack_unlock();
}

/**
 * After esp_matter::start(), resume+enable an endpoint for every used, enabled
 * shade so covers reappear with their stable ids. Persists once at the end in
 * case any shade had no id yet (first-ever expose).
 */
static void restore_endpoints(void)
{
    int n = 0;
    for (int i = 0; i < BLIND_MAX_COUNT; i++) {
        shade_t *s = blind_store_get(i);
        if (!s->addr || !s->enabled) continue;
        if (locked_endpoint_up(i)) n++;
        else ESP_LOGW(TAG, "shade %d endpoint restore failed", i);
    }
    blind_store_save();
    ESP_LOGI(TAG, "restored %d shade endpoint(s)", n);
}

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
    bool first = true;
    for (int i = 0; i < BLIND_MAX_COUNT; i++) {
        if (!blind_store_used(i)) continue;
        shade_t *s = blind_store_get(i);
        printf("%s{\"idx\":%d,\"name\":\"%s\",\"addr\":\"%06lX\",\"rolling\":%u,\"on\":%s}",
               first ? "" : ",", i, s->name, (unsigned long)s->addr, s->rolling,
               s->enabled ? "true" : "false");
        first = false;
    }
    printf("]\n");
}

static int cmd_list(int, char **) { print_shades_json(); return 0; }

static int cmd_tx(int argc, char **argv)
{
    if (argc < 3) { printf("ERR usage: tx <idx> <up|down|my|stop|prog>\n"); return 1; }
    int idx = atoi(argv[1]);
    uint8_t cmd = parse_cmd(argv[2]);
    if (!blind_store_used(idx) || !cmd) { printf("ERR bad idx/cmd\n"); return 1; }
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
    int idx = atoi(argv[1]);
    shade_t *s = blind_store_get(idx);
    if (!blind_store_used(idx)) { printf("ERR bad idx\n"); return 1; }
    s->name[0] = 0;
    for (int i = 2; i < argc; i++) {
        if (i > 2) strncat(s->name, " ", sizeof(s->name) - strlen(s->name) - 1);
        strncat(s->name, argv[i], sizeof(s->name) - strlen(s->name) - 1);
    }
    blind_store_save();
    printf("OK\n");
    return 0;
}

/**
 * `freq` prints the device-wide carrier frequency; `freq <mhz>` sets it. The
 * frequency is a single radio setting shared by every shade (EU Somfy RTS
 * motors all use the 433 band), so there is no per-shade index. Setting it also
 * live-retunes the radio and re-arms RX, so the web wizard can sweep the band
 * and listen for a remote at each step.
 */
static int cmd_freq(int argc, char **argv)
{
    if (argc < 2) { printf("%.3f\n", blind_store_freq()); return 0; }
    float f = strtof(argv[1], NULL);
    if (f < BOARD_FREQ_MIN_MHZ || f > BOARD_FREQ_MAX_MHZ) { printf("ERR freq out of band\n"); return 1; }
    blind_store_set_freq(f);
    if (s_rf_ok) { cc1101_set_frequency(&s_cc, f); somfy_rx_resume(); }
    printf("OK\n");
    return 0;
}

static int cmd_addr(int argc, char **argv)
{
    if (argc < 3) { printf("ERR usage: addr <idx> <hex24>\n"); return 1; }
    int idx = atoi(argv[1]);
    shade_t *s = blind_store_get(idx);
    if (!blind_store_used(idx)) { printf("ERR bad idx\n"); return 1; }
    s->addr = (uint32_t)strtoul(argv[2], NULL, 16) & 0xFFFFFF;
    blind_store_save();
    printf("OK\n");
    return 0;
}

static int cmd_roll(int argc, char **argv)
{
    if (argc < 3) { printf("ERR usage: roll <idx> <value>\n"); return 1; }
    int idx = atoi(argv[1]);
    shade_t *s = blind_store_get(idx);
    if (!blind_store_used(idx)) { printf("ERR bad idx\n"); return 1; }
    s->rolling = (uint16_t)strtoul(argv[2], NULL, 10);
    blind_store_save();
    printf("OK\n");
    return 0;
}

/**
 * `add [hexaddr] [rolling] [name...]` — register a shade in the first free slot
 * and bring its Matter endpoint online. With no address the firmware invents a
 * MAC-derived one (the PROG "add a motor without a remote" path); rolling
 * defaults to 1. Prints `OK <idx>` or an error, and rolls the slot back if the
 * endpoint could not be created.
 */
static int cmd_add(int argc, char **argv)
{
    uint32_t addr    = (argc >= 2) ? (uint32_t)strtoul(argv[1], NULL, 16) & 0xFFFFFF
                                   : blind_store_gen_addr();
    uint16_t rolling = (argc >= 3) ? (uint16_t)strtoul(argv[2], NULL, 10) : 1;
    char name[16] = {0};
    for (int i = 3; i < argc; i++) {
        if (i > 3) strncat(name, " ", sizeof(name) - strlen(name) - 1);
        strncat(name, argv[i], sizeof(name) - strlen(name) - 1);
    }
    int idx = blind_store_add(addr, rolling, name[0] ? name : NULL);
    if (idx < 0) { printf("ERR full\n"); return 1; }
    if (!locked_endpoint_up(idx)) { blind_store_remove(idx); printf("ERR endpoint\n"); return 1; }
    blind_store_save();
    printf("OK %d\n", idx);
    return 0;
}

/**
 * `remove <idx>` — take the shade's endpoint off Thread and free its slot.
 */
static int cmd_remove(int argc, char **argv)
{
    if (argc < 2) { printf("ERR usage: remove <idx>\n"); return 1; }
    int idx = atoi(argv[1]);
    if (!blind_store_used(idx)) { printf("ERR bad idx\n"); return 1; }
    locked_endpoint_down(idx);
    blind_store_remove(idx);
    printf("OK\n");
    return 0;
}

/**
 * `on <idx> <0|1>` — the exposure switch. 1 resumes the shade's endpoint on
 * Thread (same stable id); 0 destroys it, keeping the persisted id for a later
 * re-enable. No-op if already in the requested state.
 */
static int cmd_on(int argc, char **argv)
{
    if (argc < 3) { printf("ERR usage: on <idx> <0|1>\n"); return 1; }
    int idx = atoi(argv[1]);
    shade_t *s = blind_store_get(idx);
    if (!blind_store_used(idx)) { printf("ERR bad idx\n"); return 1; }
    bool on = atoi(argv[2]) != 0;
    if (on == s->enabled) { printf("OK\n"); return 0; }
    if (on) { if (!locked_endpoint_up(idx)) { printf("ERR endpoint\n"); return 1; } }
    else    { locked_endpoint_down(idx); }
    s->enabled = on;
    blind_store_save();
    printf("OK\n");
    return 0;
}

/**
 * Print radio status as one-line JSON: whether the CC1101 is present (so RF
 * works at all) and the current device-wide carrier frequency. The web wizard
 * gates the shade step on `rf` — no point configuring shades a dead radio can't
 * drive.
 */
static int cmd_radio(int, char **)
{
    printf("{\"rf\":%s,\"freq\":%.3f}\n", s_rf_ok ? "true" : "false", blind_store_freq());
    return 0;
}

/**
 * Read, write, or dump CC1101 registers for on-air RF tuning. `reg` alone prints
 * the modem/AGC registers that govern OOK idle-noise behaviour; `reg <hex>`
 * reads one; `reg <hex> <hex>` writes one and re-enters RX so the change applies
 * live. A bring-up calibration knob: the demodulator's response to the real
 * antenna and band noise can only be tuned against the physical setup.
 */
static int cmd_reg(int argc, char **argv)
{
    if (!s_rf_ok) { printf("no radio\n"); return 1; }
    static const uint8_t dump[] = {0x10, 0x11, 0x12, 0x15, 0x1B, 0x1C, 0x1D,
                                   0x21, 0x23, 0x24, 0x25};
    if (argc < 2) {
        for (size_t i = 0; i < sizeof(dump); i++)
            printf("0x%02X=0x%02X\n", dump[i], cc1101_read_reg(&s_cc, dump[i]));
        return 0;
    }
    uint8_t a = (uint8_t)strtoul(argv[1], NULL, 16);
    if (argc >= 3) {
        cc1101_write_reg(&s_cc, a, (uint8_t)strtoul(argv[2], NULL, 16));
        somfy_rx_resume();
    }
    printf("0x%02X=0x%02X\n", a, cc1101_read_reg(&s_cc, a));
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
        {"radio",  "Print radio status (rf present, freq) as JSON", NULL, &cmd_radio, NULL},
        {"list",   "List shades as JSON",                    NULL, &cmd_list,   NULL},
        {"add",    "add [hexaddr] [rolling] [name...] — register a shade", NULL, &cmd_add, NULL},
        {"remove", "remove <idx> — delete a shade",          NULL, &cmd_remove, NULL},
        {"on",     "on <idx> <0|1> — expose shade over Thread", NULL, &cmd_on,  NULL},
        {"tx",     "tx <idx> <up|down|my|stop|prog>",        NULL, &cmd_tx,     NULL},
        {"name",   "name <idx> <text>",                      NULL, &cmd_name,   NULL},
        {"freq",   "freq [mhz] — get/set device radio frequency", NULL, &cmd_freq, NULL},
        {"reg",    "reg [hexaddr] [hexval] — dump/read/write CC1101 registers", NULL, &cmd_reg, NULL},
        {"addr",   "addr <idx> <hex24>",                     NULL, &cmd_addr,   NULL},
        {"roll",   "roll <idx> <value>",                     NULL, &cmd_roll,   NULL},
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
        if (blind_store_used(i) && blind_store_get(i)->addr == addr) { idx = i; break; }
    }
    ESP_LOGI(TAG, "[RX] addr=0x%06lX code=%u cmd=0x%X idx=%d",
             (unsigned long)addr, code, cmd, idx);
    if (idx < 0) return;  // unknown address — a remote the web discovery step can add
    if (esp_timer_get_time() - s_last_tx_us < WC_RX_ECHO_GUARD_US) return;

    shade_t *s = blind_store_get(idx);
    if (code > s->rolling) { s->rolling = code; blind_store_save(); }

    if (!s->enabled || !s_wc_ep_ids[idx]) return;  // not exposed — no endpoint to mirror to
    uint16_t pos;
    if (cmd == SOMFY_UP)        pos = 0;
    else if (cmd == SOMFY_DOWN) pos = 10000;
    else                        return;  // MY/PROG/other: position unknown
    intptr_t arg = ((intptr_t)s_wc_ep_ids[idx] << 16) | pos;
    chip::DeviceLayer::PlatformMgr().ScheduleWork(rx_pos_work, arg);
}

/**
 * Bring up storage, RF, the RF worker, and the Matter node (no endpoints yet),
 * start Matter, then restore an endpoint for each exposed shade and start the
 * serial console; finally log the onboarding codes. RF failure is non-fatal —
 * Matter and config still run.
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
    s_node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    esp_openthread_platform_config_t ot_config = {
        .radio_config = { .radio_mode = RADIO_MODE_NATIVE },
        .host_config  = { .host_connection_mode = HOST_CONNECTION_MODE_NONE },
        .port_config  = { .storage_partition_name = "nvs", .netif_queue_size = 10, .task_queue_size = 10 },
    };
    set_openthread_platform_config(&ot_config);
#endif

    esp_matter::start(app_event_cb);

    restore_endpoints();

    esp_matter::console::init();
    register_console();

    ESP_LOGI(TAG, "Matter QR: %s", app_matter_qr());
    ESP_LOGI(TAG, "Matter manual code: %s", app_matter_manual());
}
