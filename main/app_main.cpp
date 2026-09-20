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
#include <esp_openthread_lock.h>
#include <openthread/logging.h>
#include <platform/ESP32/OpenthreadLauncher.h>
#include <lib/support/logging/CHIPLogging.h>

extern "C" {
#include "board.h"
#include "cc1101.h"
#include "somfy_rts.h"
#include "somfy_rx.h"
#include "somfy_frame.h"
#include "blind_store.h"
#include "wc_motion.h"
#include "json_escape.h"
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
 * so movement is driven by direction inferred from target vs current position and
 * the reported position is estimated from travel time (see the timed motion model
 * above). CHIP emits HandleStopMotion immediately after a movement; WC_STOP_GUARD_US
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

/**
 * Timed position estimate. Somfy RTS gives no feedback, so once a shade starts
 * moving we ramp its reported CurrentPositionLiftPercent100ths linearly toward
 * the target over the configured travel time, reporting intermediate values so
 * controllers show motion. All motion state and every attribute write is funnelled
 * onto the Matter/CHIP thread (the delegate runs there; the RX task and the
 * periodic timer hop over via ScheduleWork), so no locking is needed here.
 */
typedef struct {
    bool     active;
    uint16_t from;
    uint16_t target;
    int64_t  start_us;
    int64_t  dur_us;
    int64_t  lag_us;
    bool     send_stop;
} motion_t;
static motion_t          s_motion[BLIND_MAX_COUNT];
static esp_timer_handle_t s_motion_timer;
static bool               s_motion_timer_on;

/**
 * Write a shade's CurrentPositionLiftPercent100ths. Setting the attribute directly
 * (not through the delegate) does not re-trigger HandleMovement, so there is no
 * feedback loop. No-op if the shade has no live endpoint.
 */
static void wc_set_current(int idx, uint16_t pos)
{
    if (!s_wc_ep_ids[idx]) return;
    chip::app::DataModel::Nullable<chip::Percent100ths> p(pos);
    WC::Attributes::CurrentPositionLiftPercent100ths::Set(s_wc_ep_ids[idx], p);
}

/**
 * Persist a shade's settled position so it survives a reboot. Called only when a
 * move settles (completes, freezes, or snaps), never mid-ramp, keeping NVS writes
 * to roughly one per move. A blocking flash write on the Matter thread, but rare.
 */
static void motion_persist(int idx, uint16_t pos)
{
    shade_t *s = blind_store_get(idx);
    if (!s || s->pos == pos) return;
    s->pos = pos;
    blind_store_save();
}

/**
 * @return The shade's last reported position, or 0 (fully open) if unknown. The
 *         persisted estimate is restored on boot; a full open/close re-zeros it.
 */
static uint16_t wc_get_current(int idx)
{
    chip::app::DataModel::Nullable<chip::Percent100ths> cur;
    if (s_wc_ep_ids[idx]
            && WC::Attributes::CurrentPositionLiftPercent100ths::Get(s_wc_ep_ids[idx], cur)
                   == chip::Protocols::InteractionModel::Status::Success && !cur.IsNull())
        return cur.Value();
    return 0;
}

/**
 * @return The live estimated position: the mid-ramp interpolation while moving,
 *         else the last reported position.
 */
static uint16_t motion_current(int idx)
{
    motion_t *m = &s_motion[idx];
    if (m->active)
        return wc_motion_lerp(m->from, m->target, esp_timer_get_time() - m->start_us, m->dur_us, m->lag_us);
    return wc_get_current(idx);
}

static void motion_timer_stop(void)
{
    if (!s_motion_timer_on) return;
    esp_timer_stop(s_motion_timer);
    s_motion_timer_on = false;
}

/**
 * Periodic tick (on the Matter thread): advance every active shade's reported
 * position, retire those that reached target — sending a Somfy MY to physically
 * stop a mid-travel move — and stop the timer once nothing is moving.
 */
static void motion_tick_work(intptr_t)
{
    int64_t now = esp_timer_get_time();
    int active = 0;
    for (int i = 0; i < BLIND_MAX_COUNT; i++) {
        motion_t *m = &s_motion[i];
        if (!m->active) continue;
        if (now - m->start_us >= m->dur_us) {
            wc_set_current(i, m->target);
            motion_persist(i, m->target);
            m->active = false;
            if (m->send_stop) app_rf_submit(i, SOMFY_MY);
        } else {
            wc_set_current(i, wc_motion_lerp(m->from, m->target, now - m->start_us, m->dur_us, m->lag_us));
            active++;
        }
    }
    if (!active) motion_timer_stop();
}

static void motion_timer_cb(void *)
{
    chip::DeviceLayer::PlatformMgr().ScheduleWork(motion_tick_work, 0);
}

static void motion_timer_start(void)
{
    if (s_motion_timer_on) return;
    if (!s_motion_timer) {
        esp_timer_create_args_t a = {};
        a.callback = motion_timer_cb;
        a.name = "wc_motion";
        if (esp_timer_create(&a, &s_motion_timer) != ESP_OK) return;
    }
    esp_timer_start_periodic(s_motion_timer, 250 * 1000);
    s_motion_timer_on = true;
}

/**
 * Start a timed move of shade `idx` to `target`: hold through the direction's
 * startup dead-time `lag_ms`, then ramp the reported position over the fraction
 * of `travel_ms` the distance covers. With no travel time (or nothing to move) it
 * snaps to target — the pre-estimate behaviour. `send_stop` asks for a Somfy MY at
 * the end, used for a mid-travel target the motor would otherwise run past to its
 * end stop. Matter thread only.
 */
static void motion_go(int idx, uint16_t target, uint16_t travel_ms, uint16_t lag_ms, bool send_stop)
{
    uint16_t cur = motion_current(idx);
    if (travel_ms == 0 || target == cur) {
        s_motion[idx].active = false;
        wc_set_current(idx, target);
        motion_persist(idx, target);
        return;
    }
    motion_t *m = &s_motion[idx];
    m->from      = cur;
    m->target    = target;
    m->start_us  = esp_timer_get_time();
    m->lag_us    = (int64_t)lag_ms * 1000;
    m->dur_us    = wc_motion_dur_us(cur, target, travel_ms, lag_ms);
    m->send_stop = send_stop;
    m->active    = true;
    motion_timer_start();
}

/**
 * Freeze a moving shade's estimate where it is now (a mid-travel Stop).
 */
static void motion_stop(int idx)
{
    motion_t *m = &s_motion[idx];
    if (!m->active) return;
    uint16_t pos = wc_motion_lerp(m->from, m->target, esp_timer_get_time() - m->start_us, m->dur_us, m->lag_us);
    m->active = false;
    wc_set_current(idx, pos);
    motion_persist(idx, pos);
}

/**
 * Model a Somfy MY, which the motor interprets by context: while moving it stops;
 * while idle it drives to the stored favourite. We only send the frame, so we
 * replicate both here — freeze if we think it is moving, else run a timed move
 * toward `my_pct` (skipped when the favourite is unknown, since we cannot guess
 * it). Matter thread only.
 */
static void motion_my(int idx)
{
    if (s_motion[idx].active) { motion_stop(idx); return; }
    shade_t *s = blind_store_get(idx);
    if (!s || s->my_pct == SHADE_MY_UNSET) return;
    uint16_t target = (uint16_t)s->my_pct * 100;
    uint16_t cur = wc_get_current(idx);
    bool closing = target > cur;
    motion_go(idx, target, closing ? s->down_ms : s->up_ms, closing ? s->down_lag_ms : s->up_lag_ms, false);
}

class SomfyWCDelegate : public WC::Delegate {
public:
    /**
     * Translate a lift movement into a Somfy command and start the timed position
     * estimate. Target above the current estimate closes, below opens; `invert`
     * swaps which Somfy direction that is for reversed installs. A mid-travel
     * target schedules a Somfy MY stop when the estimate reaches it (the motor
     * would otherwise run to its end stop); an end-stop target lets the motor stop
     * itself and is always re-sent so a full open/close can re-zero a drifted
     * estimate.
     */
    CHIP_ERROR HandleMovement(WC::WindowCoveringType type) override
    {
        if (type != WC::WindowCoveringType::Lift) return CHIP_NO_ERROR;
        chip::app::DataModel::Nullable<chip::Percent100ths> tgt;
        if (WC::Attributes::TargetPositionLiftPercent100ths::Get(mEndpoint, tgt)
                != chip::Protocols::InteractionModel::Status::Success || tgt.IsNull())
            return CHIP_NO_ERROR;
        int idx = ep_to_idx(mEndpoint);
        shade_t *s = blind_store_get(idx);
        if (!s) return CHIP_NO_ERROR;
        uint16_t v = tgt.Value();
        uint16_t cur = motion_current(idx);
        bool endstop = (v == 0 || v == 10000);
        if (v == cur && !endstop) return CHIP_NO_ERROR;

        bool closing = (v == cur) ? (v == 10000) : (v > cur);
        uint8_t cmd = (closing != s->invert) ? SOMFY_DOWN : SOMFY_UP;

        s_last_move_us = esp_timer_get_time();
        s_last_move_ep = mEndpoint;
        ESP_LOGI(TAG, "[WC] ep=%u idx=%d lift=%u%% cmd=0x%X", mEndpoint, idx, v / 100, cmd);
        app_rf_submit(idx, cmd);
        motion_go(idx, v, closing ? s->down_ms : s->up_ms, closing ? s->down_lag_ms : s->up_lag_ms, !endstop);
        return CHIP_NO_ERROR;
    }

    /**
     * Handle a genuine user Stop, ignoring the auto-call CHIP emits right after a
     * movement (see WC_STOP_GUARD_US). Sends Somfy MY and models it: a moving
     * shade freezes, an idle one drives to its favourite (the motor's own MY
     * behaviour — see motion_my).
     */
    CHIP_ERROR HandleStopMotion() override
    {
        int64_t since = esp_timer_get_time() - s_last_move_us;
        if (s_last_move_ep == mEndpoint && since < WC_STOP_GUARD_US)
            return CHIP_NO_ERROR;
        int idx = ep_to_idx(mEndpoint);
        ESP_LOGI(TAG, "[WC] ep=%u idx=%d stop -> MY", mEndpoint, idx);
        app_rf_submit(idx, SOMFY_MY);
        motion_my(idx);
        return CHIP_NO_ERROR;
    }
};
static SomfyWCDelegate s_wc_delegates[BLIND_MAX_COUNT];

/**
 * Attach the lift-only WindowCovering device type (Identify/Groups/Scenes/WC
 * plus the lift + position-aware-lift features) to a bare endpoint, binding
 * shade `idx`'s delegate. Seeds current/target lift from the shade's persisted
 * position so the restored estimate is reported at boot.
 * @return ESP_OK, or the error from window_covering_device::add.
 */
static esp_err_t wc_add_clusters(endpoint_t *ep, int idx)
{
    shade_t *s = blind_store_get(idx);
    uint16_t pos = s ? s->pos : 0;
    window_covering_device::config_t wc;
    wc.window_covering.type = 0x00;
    wc.window_covering.delegate = &s_wc_delegates[idx];
    esp_err_t err = window_covering_device::add(ep, &wc);
    if (err != ESP_OK) return err;

    cluster_t *wc_cluster = cluster::get(ep, WC::Id);
    cluster::window_covering::feature::lift::config_t lift_cfg;
    cluster::window_covering::feature::lift::add(wc_cluster, &lift_cfg);
    cluster::window_covering::feature::position_aware_lift::config_t pal_cfg;
    pal_cfg.current_position_lift_percent_100ths = nullable<uint16_t>(pos);
    pal_cfg.target_position_lift_percent_100ths = nullable<uint16_t>(pos);
    cluster::window_covering::feature::position_aware_lift::add(wc_cluster, &pal_cfg);
    return ESP_OK;
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
        : NULL;
    if (!ep && s->ep_id) {
        ESP_LOGW(TAG, "shade %d: resume of ep_id %u failed (Matter min_unused counter reset by a factory_reset that kept shades) — assigning a fresh id", idx, s->ep_id);
        s->ep_id = 0;
    }
    if (!ep) ep = endpoint::create(s_node, ENDPOINT_FLAG_DESTROYABLE, NULL);
    if (!ep) { ESP_LOGE(TAG, "shade %d: endpoint create returned NULL", idx); return false; }
    esp_err_t cerr = wc_add_clusters(ep, idx);
    if (cerr != ESP_OK) { ESP_LOGE(TAG, "shade %d: wc clusters failed (0x%x)", idx, cerr); return false; }
    uint16_t id = endpoint::get_id(ep);
    s->ep_id          = id;
    s_wc_eps[idx]     = ep;
    s_wc_ep_ids[idx]  = id;
    s_wc_delegates[idx].SetEndpoint(id);
    esp_err_t eerr = endpoint::enable(ep);
    if (eerr != ESP_OK) ESP_LOGE(TAG, "shade %d: endpoint enable failed (0x%x)", idx, eerr);
    return eerr == ESP_OK;
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
 * ops must not race the Matter thread, and the console runs off it. chip_stack_lock
 * returns ALREADY_TAKEN (not SUCCESS) when the calling task already holds the lock;
 * in that case we proceed but must not unlock, or we would drop a lock we did not
 * take. Only FAILED is a real error.
 */
static bool locked_endpoint_up(int idx)
{
    esp_matter::lock::status_t ls = esp_matter::lock::chip_stack_lock(portMAX_DELAY);
    if (ls == esp_matter::lock::FAILED) { ESP_LOGE(TAG, "shade %d: chip_stack_lock FAILED", idx); return false; }
    bool ok = wc_endpoint_up(idx);
    if (ls != esp_matter::lock::ALREADY_TAKEN) esp_matter::lock::chip_stack_unlock();
    return ok;
}
static void locked_endpoint_down(int idx)
{
    esp_matter::lock::status_t ls = esp_matter::lock::chip_stack_lock(portMAX_DELAY);
    if (ls == esp_matter::lock::FAILED) return;
    wc_endpoint_down(idx);
    if (ls != esp_matter::lock::ALREADY_TAKEN) esp_matter::lock::chip_stack_unlock();
}

static bool s_diag_logs = true;

/**
 * Gate the chatty Thread (OpenThread) and Matter (CHIP) diagnostic logs. `on`
 * keeps them at INFO/detail — wanted while bringing an un-onboarded device up;
 * off drops to warnings/errors so a commissioned device runs quiet. Driven at
 * boot from the commissioning state and by the `log` console command.
 */
static void set_diag_logs(bool on)
{
    if (esp_openthread_lock_acquire(portMAX_DELAY)) {
        otLoggingSetLevel(on ? OT_LOG_LEVEL_INFO : OT_LOG_LEVEL_WARN);
        esp_openthread_lock_release();
    }
#if CONFIG_CHIP_LOG_FILTERING
    chip::Logging::SetLogFilter(on ? chip::Logging::kLogCategory_Detail
                                   : chip::Logging::kLogCategory_Error);
#endif
}

/**
 * Report the running firmware version as the Matter Basic Information
 * SoftwareVersionString (endpoint 0), so controllers show the real build id
 * instead of CHIP's "1.0" default. Best-effort: if the attribute is absent the
 * update just no-ops. Call after esp_matter::start().
 */
static void set_matter_version(void)
{
    const char *ver = esp_app_get_description()->version;
    esp_matter_attr_val_t val = esp_matter_char_str((char *)ver, strlen(ver));
    attribute::update(0, chip::app::Clusters::BasicInformation::Id,
                      chip::app::Clusters::BasicInformation::Attributes::SoftwareVersionString::Id, &val);
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

/**
 * @return The number of Matter fabrics the device is commissioned to (0 = not
 *         yet paired). Lets the web branch initial pairing vs adding another
 *         ecosystem. Read directly like app_matter_qr/manual (console thread).
 */
extern "C" int app_matter_fabric_count(void)
{
    return chip::Server::GetInstance().GetFabricTable().FabricCount();
}

static void factory_reset_work(intptr_t full)
{
    if (full) blind_store_factory_erase();
    esp_matter::factory_reset();
}

/**
 * Reset Matter + Thread and reboot, scheduled on the Matter thread. When `full`,
 * also wipe the Somfy store (shades, links, radio) first — esp_matter's reset
 * only clears CHIP's own namespaces, leaving ours intact otherwise.
 */
extern "C" void app_matter_factory_reset(int full)
{
    chip::DeviceLayer::PlatformMgr().ScheduleWork(factory_reset_work, full);
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
        char nm[sizeof(s->name) * 6 + 1];
        json_escape(s->name, nm, sizeof(nm));
        printf("%s{\"idx\":%d,\"name\":\"%s\",\"addr\":\"%06lX\",\"rolling\":%u,\"on\":%s,\"remote\":%s,\"link\":\"%06lX\",\"up_ms\":%u,\"down_ms\":%u,\"my\":%d,\"invert\":%s,\"up_lag\":%u,\"down_lag\":%u,\"pos\":%u}",
               first ? "" : ",", i, nm, (unsigned long)s->addr, s->rolling,
               s->enabled ? "true" : "false", s->remote ? "true" : "false",
               (unsigned long)blind_store_link_addr(i),
               s->up_ms, s->down_ms, s->my_pct, s->invert ? "true" : "false",
               s->up_lag_ms, s->down_lag_ms, s->pos);
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
 * `pos <idx> <up_ms> <down_ms> [my_pct 0-100|255] [invert 0|1] [up_lag_ms] [down_lag_ms]`
 * — set the position-estimate parameters. `up_ms`/`down_ms` are the
 * full-open/full-close travel times (0 = snap to target instead of estimating);
 * `my_pct` is our copy of the motor's favourite position (255 = unknown); `invert`
 * swaps open/close for reversed installs; `up_lag_ms`/`down_lag_ms` are the
 * per-direction startup dead-times before the shade visibly moves. Omitted
 * trailing args are left unchanged.
 */
static int cmd_pos(int argc, char **argv)
{
    if (argc < 4) { printf("ERR usage: pos <idx> <up_ms> <down_ms> [my_pct] [invert] [up_lag_ms] [down_lag_ms]\n"); return 1; }
    int idx = atoi(argv[1]);
    shade_t *s = blind_store_get(idx);
    if (!blind_store_used(idx)) { printf("ERR bad idx\n"); return 1; }
    s->up_ms   = (uint16_t)strtoul(argv[2], NULL, 10);
    s->down_ms = (uint16_t)strtoul(argv[3], NULL, 10);
    if (argc >= 5) s->my_pct = (uint8_t)strtoul(argv[4], NULL, 10);
    if (argc >= 6) s->invert = atoi(argv[5]) != 0;
    if (argc >= 7) s->up_lag_ms   = (uint16_t)strtoul(argv[6], NULL, 10);
    if (argc >= 8) s->down_lag_ms = (uint16_t)strtoul(argv[7], NULL, 10);
    blind_store_save();
    printf("OK\n");
    return 0;
}

/**
 * `add [hexaddr] [rolling] [name...]` — register a shade in the first free slot
 * and bring its Matter endpoint online. With no address the firmware invents a
 * MAC-derived one (the PROG "add a motor without a remote" path); rolling
 * defaults to 1. An explicit address marks the shade remote-linked (cloned from
 * a physical remote); the invented-address path leaves it PROG-paired. Prints
 * `OK <idx>` or an error, and rolls the slot back if the endpoint could not be
 * created.
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
    blind_store_get(idx)->remote = (argc >= 2);
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
 * `link <idx> <hexaddr> [rolling]` — associate a physical Somfy remote we only
 * listen for, so pressing that wall remote mirrors the shade's position. Never
 * transmitted as; the shade keeps its own address for TX. `unlink <idx>` clears
 * it (`link <idx> 0` also clears).
 */
static int cmd_link(int argc, char **argv)
{
    if (argc < 3) { printf("ERR usage: link <idx> <hexaddr> [rolling]\n"); return 1; }
    int idx = atoi(argv[1]);
    if (!blind_store_used(idx)) { printf("ERR bad idx\n"); return 1; }
    uint32_t addr = (uint32_t)strtoul(argv[2], NULL, 16);
    uint16_t roll = (argc >= 4) ? (uint16_t)atoi(argv[3]) : 0;
    blind_store_set_link(idx, addr, roll);
    printf("OK\n");
    return 0;
}

static int cmd_unlink(int argc, char **argv)
{
    if (argc < 2) { printf("ERR usage: unlink <idx>\n"); return 1; }
    int idx = atoi(argv[1]);
    if (!blind_store_used(idx)) { printf("ERR bad idx\n"); return 1; }
    blind_store_set_link(idx, 0, 0);
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
    printf("{\"rf\":%s,\"freq\":%.3f", s_rf_ok ? "true" : "false", blind_store_freq());
    if (s_rf_ok) printf(",\"rssi\":%d", cc1101_rssi_dbm(&s_cc));
    printf(",\"power\":%u,\"power_opts\":[", blind_store_tx_power());
    for (int i = 0; i < CC1101_TX_POWER_COUNT; i++) printf("%s%d", i ? "," : "", cc1101_tx_power_dbm[i]);
    printf("],\"rxbw\":%u,\"rxbw_opts\":[", blind_store_rxbw());
    for (int i = 0; i < CC1101_RXBW_COUNT; i++) printf("%s%u", i ? "," : "", cc1101_rxbw_khz[i]);
    printf("]}\n");
    return 0;
}

/**
 * Get/set the device-wide TX power by index into the CC1101 power table. A tuning
 * knob for range vs. regulatory headroom; applied live and persisted.
 */
static int cmd_power(int argc, char **argv)
{
    if (argc < 2) { printf("%u\n", blind_store_tx_power()); return 0; }
    int idx = atoi(argv[1]);
    if (idx < 0 || idx >= CC1101_TX_POWER_COUNT) { printf("ERR power idx 0..%d\n", CC1101_TX_POWER_COUNT - 1); return 1; }
    blind_store_set_tx_power((uint8_t)idx);
    if (s_rf_ok) cc1101_set_power(&s_cc, (uint8_t)idx);
    printf("OK\n");
    return 0;
}

/**
 * Get/set the device-wide RX bandwidth by index into the CC1101 bandwidth table.
 * Applied live (re-enters RX) and persisted; trades sensitivity for selectivity.
 */
static int cmd_rxbw(int argc, char **argv)
{
    if (argc < 2) { printf("%u\n", blind_store_rxbw()); return 0; }
    int idx = atoi(argv[1]);
    if (idx < 0 || idx >= CC1101_RXBW_COUNT) { printf("ERR rxbw idx 0..%d\n", CC1101_RXBW_COUNT - 1); return 1; }
    blind_store_set_rxbw((uint8_t)idx);
    if (s_rf_ok) { cc1101_set_rxbw(&s_cc, (uint8_t)idx); somfy_rx_resume(); }
    printf("OK\n");
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

/**
 * Serial console contract version. Bump whenever a command is added, removed, or
 * its input/output changes in a way an older configuration site cannot handle.
 * The site refuses to configure a board whose proto is below the one it targets.
 */
#define SOMFY_PROTO 8

static int cmd_version(int, char **) { printf("somfy-thread %s proto %d\n", esp_app_get_description()->version, SOMFY_PROTO); return 0; }
static int cmd_export(int, char **) { print_shades_json(); return 0; }
static int cmd_qr(int, char **)     { printf("%s\n", app_matter_qr()); return 0; }
static int cmd_pair(int, char **)   { app_matter_open_window(); printf("%s\n", app_matter_manual()); return 0; }
static int cmd_mstat(int, char **)  { printf("{\"fabrics\":%d}\n", app_matter_fabric_count()); return 0; }
static int cmd_reset(int, char **)   { printf("OK resetting\n"); app_matter_factory_reset(0); return 0; }
static int cmd_factory(int, char **) { printf("OK factory\n");   app_matter_factory_reset(1); return 0; }
static int cmd_reboot(int, char **)  { printf("OK rebooting\n"); esp_restart(); return 0; }
static int cmd_log(int argc, char **argv)
{
    if (argc >= 2) { s_diag_logs = atoi(argv[1]) != 0; set_diag_logs(s_diag_logs); }
    printf("{\"log\":%d}\n", s_diag_logs ? 1 : 0);
    return 0;
}

/**
 * Register the serial console commands that form the WebSerial contract.
 */
static void register_console(void)
{
    const esp_console_cmd_t cmds[] = {
        {"version","Print firmware id and version",          NULL, &cmd_version, NULL},
        {"log",    "log [0|1] — get/set Thread+Matter diagnostic logging", NULL, &cmd_log, NULL},
        {"radio",  "Print radio status (rf, freq, rssi, power, rxbw + options) as JSON", NULL, &cmd_radio, NULL},
        {"list",   "List shades as JSON",                    NULL, &cmd_list,   NULL},
        {"add",    "add [hexaddr] [rolling] [name...] — register a shade", NULL, &cmd_add, NULL},
        {"remove", "remove <idx> — delete a shade",          NULL, &cmd_remove, NULL},
        {"on",     "on <idx> <0|1> — expose shade over Thread", NULL, &cmd_on,  NULL},
        {"link",   "link <idx> <hexaddr> [rolling] — monitor a physical remote", NULL, &cmd_link,   NULL},
        {"unlink", "unlink <idx> — stop monitoring the linked remote", NULL, &cmd_unlink, NULL},
        {"tx",     "tx <idx> <up|down|my|stop|prog>",        NULL, &cmd_tx,     NULL},
        {"name",   "name <idx> <text>",                      NULL, &cmd_name,   NULL},
        {"freq",   "freq [mhz] — get/set device radio frequency", NULL, &cmd_freq, NULL},
        {"power",  "power [idx] — get/set TX power (index into radio power_opts)", NULL, &cmd_power, NULL},
        {"rxbw",   "rxbw [idx] — get/set RX bandwidth (index into radio rxbw_opts)", NULL, &cmd_rxbw, NULL},
        {"reg",    "reg [hexaddr] [hexval] — dump/read/write CC1101 registers", NULL, &cmd_reg, NULL},
        {"addr",   "addr <idx> <hex24>",                     NULL, &cmd_addr,   NULL},
        {"roll",   "roll <idx> <value>",                     NULL, &cmd_roll,   NULL},
        {"pos",    "pos <idx> <up_ms> <down_ms> [my_pct] [invert] [up_lag_ms] [down_lag_ms]", NULL, &cmd_pos, NULL},
        {"export", "Dump full shade table (backup) as JSON", NULL, &cmd_export, NULL},
        {"qr",     "Print Matter QR payload",                NULL, &cmd_qr,     NULL},
        {"pair",   "Open commissioning window, print code",  NULL, &cmd_pair,   NULL},
        {"mstat",  "Matter status (commissioned fabric count) as JSON", NULL, &cmd_mstat, NULL},
        {"reset",  "Reset Matter+Thread (keeps shades) and reboot", NULL, &cmd_reset,  NULL},
        {"factory","Full factory reset: erase shades + Matter+Thread, reboot", NULL, &cmd_factory, NULL},
        {"reboot", "Reboot the device (no data change)",     NULL, &cmd_reboot, NULL},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        esp_console_cmd_register(&cmds[i]);
}

/**
 * Feed an observed remote press into the timed position model on the Matter
 * thread, so a physical up/down/My keeps the reported position in sync. Argument
 * packs the shade index in the high bits and the Somfy command in the low byte.
 * `invert` decides which physical direction opens; My defers to motion_my's
 * moving-vs-idle branch (stop vs favourite).
 */
static void rx_motion_work(intptr_t arg)
{
    int idx = (int)(arg >> 8);
    uint8_t cmd = (uint8_t)(arg & 0xFF);
    shade_t *s = blind_store_get(idx);
    if (!s || !s->enabled || !s_wc_ep_ids[idx]) return;
    if (cmd == SOMFY_MY) { motion_my(idx); return; }
    bool opening = (cmd == SOMFY_UP) != s->invert;
    motion_go(idx, opening ? 0 : 10000, opening ? s->up_ms : s->down_ms,
              opening ? s->up_lag_ms : s->down_lag_ms, false);
}

/**
 * Receive-frame handler (called from the RX task). Logs every decoded frame —
 * this is the sniffer, and unknown addresses reveal remotes to pair/import. For
 * a known active shade it advances the rolling-code floor (so our next transmit
 * is not stale-rejected) and feeds up/down/My into the timed position model (via
 * rx_motion_work) so the Matter controller reflects a remote used outside it;
 * other commands (PROG and the like) carry no position change and are dropped.
 * Frames within WC_RX_ECHO_GUARD_US of our own transmit are ignored as
 * self-reception.
 */
#define WC_RX_ECHO_GUARD_US (1500 * 1000)
extern "C" void app_on_rx_frame(uint32_t addr, uint16_t code, uint8_t cmd)
{
    int idx = -1;
    bool via_link = false;
    for (int i = 0; i < BLIND_MAX_COUNT; i++) {
        if (blind_store_used(i) && blind_store_get(i)->addr == addr) { idx = i; break; }
    }
    if (idx < 0) {
        for (int i = 0; i < BLIND_MAX_COUNT; i++) {
            if (blind_store_used(i) && blind_store_link_addr(i) == addr) { idx = i; via_link = true; break; }
        }
    }
    ESP_LOGI(TAG, "[RX] addr=0x%06lX code=%u cmd=0x%X idx=%d%s",
             (unsigned long)addr, code, cmd, idx, via_link ? " (linked)" : "");
    if (idx < 0) return;  // unknown address — a remote the web discovery step can add
    if (esp_timer_get_time() - s_last_tx_us < WC_RX_ECHO_GUARD_US) return;

    shade_t *s = blind_store_get(idx);
    if (via_link) blind_store_link_seen(idx, code);
    else if (code > s->rolling) { s->rolling = code; blind_store_save(); }

    if (!s->enabled || !s_wc_ep_ids[idx]) return;  // not exposed — no endpoint to mirror to
    if (cmd != SOMFY_UP && cmd != SOMFY_DOWN && cmd != SOMFY_MY) return;
    chip::DeviceLayer::PlatformMgr().ScheduleWork(rx_motion_work, ((intptr_t)idx << 8) | cmd);
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
    if (s_rf_ok) {
        cc1101_set_power(&s_cc, blind_store_tx_power());
        cc1101_set_rxbw(&s_cc, blind_store_rxbw());
    }
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

    set_matter_version();
    restore_endpoints();

    esp_matter::console::init();
    register_console();

    s_diag_logs = app_matter_fabric_count() == 0;
    set_diag_logs(s_diag_logs);

    ESP_LOGI(TAG, "Matter QR: %s", app_matter_qr());
    ESP_LOGI(TAG, "Matter manual code: %s", app_matter_manual());
}
