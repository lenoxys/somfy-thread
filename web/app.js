// SPDX-License-Identifier: Unlicense
// somfy-thread setup wizard: connect to the board over Web Serial, detect the
// firmware and its version (offer update / continue / flash), then configure
// (shades, motor PROG/test, Matter pairing, backup). One page, all client-side;
// only the firmware .bin is fetched (from GitHub Releases) when flashing.

import { t, applyI18n, languages, getLang, setLang } from "./i18n.js";
import { flashRanges } from "./fwslice.mjs";

const $ = (id) => document.getElementById(id);
const logEl = $("log");

let esptoolMod = null;
/** Lazily import the vendored esptool-js bundle (218 KB) only when flashing. */
function esptool() {
  return esptoolMod || (esptoolMod = import("./vendor/esptool.js"));
}

let lastPort = null;

// Serial console contract version this site targets. Must match the firmware's
// SOMFY_PROTO (main/app_main.cpp); a board reporting a lower proto is refused
// and prompted to update. Bump both together when the command set changes.
const REQUIRED_PROTO = 8;

/** Append a line to the on-screen serial log. */
function log(s) {
  logEl.textContent += s + "\n";
  logEl.scrollTop = logEl.scrollHeight;
}

/* ── GitHub releases (firmware source) ────────────────────────────────── */

/**
 * Derive "owner/repo" from the GitHub Pages URL (owner.github.io/repo/...).
 * Falls back to a placeholder when served from anywhere else.
 */
function repoSlug() {
  const m = location.hostname.match(/^([^.]+)\.github\.io$/);
  if (m) {
    const seg = location.pathname.split("/").filter(Boolean)[0];
    if (seg) return `${m[1]}/${seg}`;
  }
  return "OWNER/somfy-thread";
}

/** Find the flashable firmware asset (a merged *.bin) in a release. */
function firmwareAsset(release) {
  return (release.assets || []).find((x) => x.name.endsWith(".bin")) || null;
}

let releases = [];

/** Show the selected release's changelog and manual-download fallback link. */
function selectRelease(idx) {
  const r = releases[idx];
  const asset = firmwareAsset(r);
  const fb = $("fallback");
  $("changelog").textContent = r.body || "(no notes)";
  $("changelogBox").hidden = false;
  $("flash").disabled = !asset;
  fb.textContent = "";
  if (!asset) { fb.textContent = t("release.noBin"); return; }
  const link = document.createElement("a");
  link.href = asset.browser_download_url;
  link.textContent = t("release.download", { name: asset.name });
  fb.append(t("release.fallbackPrefix"), link, t("release.fallbackSuffix"));
}

let flashing = false;

/**
 * Flash the selected release in-page with esptool-js, reusing the serial port
 * the user already granted (no second port picker, no external flasher dialog).
 * The firmware is a same-origin copy the Pages deploy mirrors under fw/<tag>/
 * (GitHub's release-download URL redirects to a host with no CORS headers, so
 * fetching it in the browser is otherwise blocked). The merged image is split
 * around the nvs partition (fwslice) so a normal flash keeps the fleet; the
 * "erase everything" checkbox instead wipes all of flash (fresh/first install).
 * The post-flash reboot uses the USB-JTAG reset sequence for the ESP32-C6's
 * native USB Serial/JTAG (PID 0x1001) and the classic RTS-pin reset for a
 * USB-to-UART bridge; the RTS reset does not reboot the native port, which would
 * leave the chip in the flasher stub and silent to every serial command.
 */
async function flashSelected() {
  if (flashing) return;
  const r = releases[Number($("release").value) || 0];
  const asset = r && firmwareAsset(r);
  if (!asset) return;
  if (!("serial" in navigator)) { $("unsupported").hidden = false; return; }
  flashing = true;
  $("flash").disabled = true;
  $("flashClose").hidden = true;
  $("flashSpin").hidden = false;
  $("flashModal").hidden = false;
  const bar = (pct) => { $("flashBar").style.width = pct + "%"; };
  const status = (key, vars) => { $("flashStatus").textContent = t(key, vars); };
  const term = { clean() {}, writeLine(d) { log(d); }, write(d) { log(String(d).replace(/\r?\n$/, "")); } };
  bar(0);
  status("flash.connecting");
  let transport, ok = false;
  try {
    const src = new URL(`fw/${encodeURIComponent(r.tag_name)}/${asset.name}`, location.href).href;
    const buf = new Uint8Array(await (await fetch(src)).arrayBuffer());
    const wipe = $("wipeAll").checked;
    const fileArray = flashRanges(buf.length).map((rg) => ({ data: buf.subarray(rg.from, rg.to), address: rg.offset }));
    const dev = lastPort || (await navigator.serial.getPorts())[0] || await navigator.serial.requestPort();
    await releasePort();
    const mod = await esptool();
    transport = new mod.Transport(dev, false);
    const loader = new mod.ESPLoader({ transport, baudrate: 460800, romBaudrate: 115200, terminal: term });
    await loader.main();
    status("flash.writing");
    await loader.writeFlash({
      fileArray, flashMode: "keep", flashFreq: "keep", flashSize: "keep",
      eraseAll: wipe, compress: true,
      reportProgress: (i, written, total) => bar(Math.round(((i + (total ? written / total : 0)) / fileArray.length) * 100)),
    });
    status("flash.resetting");
    const pid = transport.getPid && transport.getPid();
    if (pid === 0x1001) { log("reset: USB-JTAG sequence"); await new mod.UsbJtagSerialReset(transport).reset(); }
    else { log("reset: classic hard reset"); await loader.after("hard_reset"); }
    bar(100);
    ok = true;
  } catch (e) {
    const msg = e && e.message ? e.message : String(e);
    log("flash error: " + msg);
    status("flash.failed", { err: msg });
    $("flashSpin").hidden = true;
    $("flashClose").hidden = false;
  } finally {
    try { if (transport) await transport.disconnect(); } catch (e2) { /* port re-enumerated on reset */ }
    flashing = false;
  }
  if (ok) await waitReconnect(status);
  else $("flash").disabled = false;
}

/**
 * After a successful flash the board hard-resets and its USB re-enumerates.
 * Hide the flash controls and reconnect to the same already-granted port (no
 * picker), retrying while it comes back up; connect() then re-fingerprints the
 * firmware and re-enables Next. Falls back to the manual "recheck" button if it
 * doesn't reappear.
 */
async function waitReconnect(status) {
  $("flashControls").hidden = true;
  status("flash.reconnecting");
  for (let i = 0; i < 15; i++) {
    await sleep(1000);
    const dev = lastPort || (await navigator.serial.getPorts())[0];
    if (!dev) { log(`reconnect: no granted port yet (${i + 1}/15)`); continue; }
    try { await connect(dev); return; } catch (e) { log(`reconnect: port not back (${i + 1}/15) — ${e.message}`); }
  }
  log("reconnect: gave up after 15s — use Recheck board");
  status("flash.reconnectManual");
  $("flashSpin").hidden = true;
  $("flashClose").hidden = false;
}

/** Fetch published releases (newest first) into `releases`; empty on failure. */
async function fetchReleases() {
  try {
    const resp = await fetch(`https://api.github.com/repos/${repoSlug()}/releases`);
    if (!resp.ok) throw new Error(`GitHub API ${resp.status}`);
    releases = (await resp.json()).filter((r) => !r.draft);
  } catch (e) {
    releases = [];
    log("releases: " + e.message);
  }
}

/** Fill the version chooser from `releases` and wire it to selectRelease. */
function populateReleaseSelect() {
  const sel = $("release");
  if (!releases.length) { sel.innerHTML = ""; sel.append(new Option(t("release.none"))); return; }
  sel.innerHTML = "";
  releases.forEach((r, i) => {
    const o = document.createElement("option");
    o.value = String(i);
    o.textContent = r.tag_name + (r.prerelease ? t("release.prerelease") : "");
    sel.append(o);
  });
  sel.onchange = () => selectRelease(Number(sel.value));
  selectRelease(0);
}

/* ── serial layer ─────────────────────────────────────────────────────── */

let port = null;
let writer = null;
let reader = null;
let pipeAbort = null;
let pipeDone = null;
let connected = false;
const pending = [];

/** Feed one received line to the oldest matching pending request. */
function dispatch(line) {
  for (let i = 0; i < pending.length; i++) {
    if (pending[i].match(line)) {
      clearTimeout(pending[i].timer);
      const p = pending.splice(i, 1)[0];
      p.resolve(line);
      return;
    }
  }
}

/**
 * Continuously read the port, split into lines, log and dispatch them. Returns
 * (or throws) when the stream closes — a closed reader means the board is gone
 * (USB unplug or a dead port), so we flip the UI to disconnected.
 */
async function readLoop() {
  const dec = new TextDecoderStream();
  pipeAbort = new AbortController();
  pipeDone = port.readable.pipeTo(dec.writable, { signal: pipeAbort.signal }).catch(() => {});
  const r = dec.readable.getReader();
  reader = r;
  let buf = "";
  try {
    for (;;) {
      const { value, done } = await r.read();
      if (done) break;
      buf += value;
      let nl;
      while ((nl = buf.indexOf("\n")) >= 0) {
        const line = buf.slice(0, nl).replace(/\r$/, "").trim();
        buf = buf.slice(nl + 1);
        if (line) { log(line); dispatch(line); watchMatter(line); }
      }
    }
  } catch (e) {
    /* stream errored — handled as a disconnect below */
  }
  if (connected) onDisconnect();
}

/** Send a command line to the device. */
async function send(cmd) {
  log("> " + cmd);
  if (!writer) { log("send: no writer (port not open)"); throw new Error("no writer"); }
  try {
    await writer.write(new TextEncoder().encode(cmd + "\n"));
  } catch (e) {
    log("send: write failed — " + e.message);
    throw e;
  }
}

/**
 * Send a command and wait for the first response line matching `match`.
 * Rejects after `timeout` ms so a lost line never hangs the UI.
 */
function request(cmd, match, timeout = 3000) {
  return new Promise((resolve, reject) => {
    const p = { match, resolve };
    p.timer = setTimeout(() => {
      const i = pending.indexOf(p);
      if (i >= 0) pending.splice(i, 1);
      reject(new Error("timeout: " + cmd));
    }, timeout);
    pending.push(p);
    send(cmd).catch(reject);
  });
}

/** Resolve after ms — a plain delay for letting the radio settle between steps. */
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/** Wait for an unsolicited line matching `match` (no command sent), e.g. an [RX] frame. */
function waitFor(match, timeout) {
  return new Promise((resolve, reject) => {
    const p = { match, resolve };
    p.timer = setTimeout(() => {
      const i = pending.indexOf(p);
      if (i >= 0) pending.splice(i, 1);
      reject(new Error("timeout"));
    }, timeout);
    pending.push(p);
  });
}

/* ── wizard navigation ────────────────────────────────────────────────── */

const STEPS = 5;
const CONNECT_STEP = 0;
const RADIO_STEP = 1;
const SHADES_STEP = 2;
const MATTER_STEP = 3;
let step = 0;
let radioReady = false;
let skipRadio = false;

/** Next visible step in direction dir (±1), hopping the radio step once it's been cleared. */
function stepIn(dir) {
  let n = step + dir;
  if (n === RADIO_STEP && skipRadio) n += dir;
  return n;
}

/** Show one step, update the stepper, and gate the nav buttons. */
function setStep(n) {
  step = Math.max(0, Math.min(STEPS - 1, n));
  document.querySelectorAll("section.step").forEach((s) => {
    s.hidden = Number(s.dataset.step) !== step;
  });
  document.querySelectorAll("#stepper li").forEach((li, i) => {
    const done = i < step;
    li.classList.toggle("active", i === step);
    li.classList.toggle("done", done);
    li.tabIndex = done ? 0 : -1;
    if (done) li.setAttribute("role", "button");
    else li.removeAttribute("role");
  });
  $("back").hidden = step === 0;
  $("next").hidden = step === STEPS - 1;
  gateNext();
  if (step === RADIO_STEP) loadRadio().catch((e) => log("ERR " + e.message));
  if (step === SHADES_STEP && connected) refresh().catch((e) => log("ERR " + e.message));
  if (step === MATTER_STEP) loadMatter().catch((e) => log("ERR " + e.message));
  if (step === SHADES_STEP && connected) startPosPoll(); else stopPosPoll();
  if (step !== MATTER_STEP) stopMatterPoll();
}

/** Gate the Next button: needs a connection, and a working radio to leave the radio step. */
function gateNext() {
  $("next").disabled =
    (step === CONNECT_STEP && !connected) || (step === RADIO_STEP && !radioReady);
}

/* ── radio step ───────────────────────────────────────────────────────── */

/**
 * Query radio status: reflect CC1101 presence + frequency, and set radioReady
 * so the wizard gates the shade step on a working radio.
 */
async function loadRadio() {
  const line = await request("radio", (l) => l.startsWith("{"));
  const st = JSON.parse(line);
  radioReady = !!st.rf;
  $("radioFreq").value = Number(st.freq).toFixed(3);
  fillRadioOpts(st);
  const el = $("radioStatus");
  el.textContent = radioReady ? t("radio.ok") : t("radio.absent");
  el.classList.toggle("bad", !radioReady);
  gateNext();
}

/** Populate the TX-power / RX-bandwidth selects and the RSSI readout from radio JSON. */
function fillRadioOpts(st) {
  const pow = $("radioPower");
  if (Array.isArray(st.power_opts)) {
    pow.innerHTML = "";
    st.power_opts.forEach((dbm, i) => pow.append(new Option(`${dbm} dBm`, i)));
  }
  if (st.power != null) pow.value = st.power;
  const bw = $("radioRxbw");
  if (Array.isArray(st.rxbw_opts)) {
    bw.innerHTML = "";
    st.rxbw_opts.forEach((khz, i) => bw.append(new Option(`${khz} kHz`, i)));
  }
  if (st.rxbw != null) bw.value = st.rxbw;
  $("radioRssi").textContent = st.rssi != null ? t("radio.rssi", { dbm: st.rssi }) : "";
}

/**
 * Sweep the allowed band, sampling RSSI at each step, and list frequency → signal
 * so an advanced user can pick a clean-ish carrier. Passive (RX only, no transmit);
 * restores the configured frequency when done. A row click adopts that frequency.
 */
async function scanBand() {
  if (scanning) return;
  scanning = true;
  const out = $("radioScanOut");
  const body = out.querySelector("tbody");
  body.innerHTML = "";
  out.hidden = false;
  const btn = $("radioScan");
  btn.disabled = true;
  const prev = $("radioFreq").value;
  try {
    for (let f = 433.05; f <= 434.79; f += 0.1) {
      const fs = f.toFixed(2);
      await send(`freq ${fs}`);
      await sleep(400);
      const st = JSON.parse(await request("radio", (l) => l.startsWith("{")));
      const tr = document.createElement("tr");
      const fc = document.createElement("td");
      fc.textContent = `${fs} MHz`;
      const rc = document.createElement("td");
      rc.textContent = st.rssi != null ? `${st.rssi} dBm` : "—";
      tr.append(fc, rc);
      tr.style.cursor = "pointer";
      tr.addEventListener("click", () => { save(`freq ${fs}`); $("radioFreq").value = fs; });
      body.append(tr);
    }
  } finally {
    await send(`freq ${prev}`);
    btn.disabled = false;
    scanning = false;
  }
}

// Carrier candidates to sweep, nominal 433.42 first then out to the crystal-drift
// edges (a real Somfy remote is decoded at whichever step its signal lands in).
const SCAN_FREQS = [433.42, 433.40, 433.44, 433.38, 433.46, 433.36];
let scanning = false;

/**
 * Scan the band while listening: retune the radio to each candidate frequency
 * and wait for the board to decode a frame ([RX] log line). The first frequency
 * that hears the remote proves the radio works, is locked in as the carrier,
 * and reveals the remote's address. Ask the user to hold their remote.
 */
async function scanAndListen() {
  if (scanning) return;
  scanning = true;
  const heard = $("radioHeard");
  heard.hidden = false;
  heard.classList.remove("bad");
  try {
    for (const f of SCAN_FREQS) {
      const fs = f.toFixed(3);
      await send(`freq ${fs}`);
      heard.textContent = t("radio.scanning", { freq: fs });
      try {
        const line = await waitFor((l) => l.includes("[RX] addr="), 3000);
        const m = line.match(/addr=0x([0-9A-Fa-f]+)/);
        heard.textContent = t("radio.heard", { addr: m ? m[1] : "?", freq: fs });
        $("radioFreq").value = fs;
        radioReady = true;
        gateNext();
        return;
      } catch (e) { /* nothing at this step — try the next */ }
    }
    heard.classList.add("bad");
    heard.textContent = t("radio.notHeard");
  } finally {
    scanning = false;
  }
}

/* ── shade rendering ──────────────────────────────────────────────────── */

let shades = [];

/** Query the shade table (JSON array line) and re-render it. */
async function refresh() {
  const line = await request("list", (l) => l.startsWith("["));
  shades = JSON.parse(line);
  renderManage();
}

let posPolling = false;

/**
 * While on the shades step, poll `list` (~1.2 s) and patch only the position
 * cells, so a linked-remote press animates live. Patching in place (not a full
 * renderManage) keeps an open detail editor and its focused inputs intact.
 * Self-reschedules after each reply so polls never overlap on a slow link.
 */
function startPosPoll() {
  if (posPolling) return;
  posPolling = true;
  const tick = async () => {
    if (!posPolling) return;
    try {
      const arr = JSON.parse(await request("list", (l) => l.startsWith("[")));
      for (const s of arr) {
        const cell = document.querySelector(`#manageBody [data-pos="${s.idx}"]`);
        if (cell) cell.textContent = `${Math.round((s.pos || 0) / 100)}%`;
      }
    } catch (e) { /* transient (timeout / disconnect) — try again next tick */ }
    if (posPolling) setTimeout(tick, 1200);
  };
  setTimeout(tick, 1200);
}

/** Stop the position poll (leaving the shades step or on disconnect). */
function stopPosPoll() { posPolling = false; }

let matterPolling = false;

/**
 * While on the Matter step, re-check the board's fabric count every ~1.5 s so
 * the view follows a pairing (or removal) completed on the hub. Commissioning
 * never produces a command reply, so polling is the only way the UI learns it
 * finished. Self-reschedules and stops when the step, connection, or a manual
 * stop goes away.
 */
function startMatterPoll() {
  if (matterPolling) return;
  matterPolling = true;
  const tick = async () => {
    if (!matterPolling || step !== MATTER_STEP || !connected) { matterPolling = false; return; }
    await pollMatterOnce();
    if (matterPolling) setTimeout(tick, 1500);
  };
  setTimeout(tick, 1500);
}

/** Stop the Matter poll (leaving the Matter step or on disconnect). */
function stopMatterPoll() { matterPolling = false; }

/**
 * Send a setter command and confirm it against the board's reply: every firmware
 * setter answers `OK` or `ERR …`. Flash a transient toast so a settings edit or a
 * calibration write (which routes through savePos → save) visibly lands.
 */
function save(cmd) {
  request(cmd, (l) => l === "OK" || l.startsWith("ERR"))
    .then((l) => { const bad = l.startsWith("ERR"); toast(t(bad ? "save.failed" : "save.ok"), !bad); })
    .catch((e) => { log("ERR " + e.message); toast(t("save.failed"), false); });
}

/** Flash a brief status toast (bottom-center), auto-hiding after 2 s. */
let toastTimer = 0;
function toast(msg, ok = true) {
  const el = $("toast");
  el.textContent = msg;
  el.classList.toggle("bad", !ok);
  el.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.hidden = true; }, 2000);
}

/**
 * Show the confirmation modal with `msg` and resolve true (proceed) or false
 * (cancel). Used for the destructive reset actions instead of a native dialog.
 */
function askConfirm(msg, okLabel = t("confirm.proceed")) {
  return new Promise((resolve) => {
    const modal = $("confirmModal");
    $("confirmMsg").textContent = msg;
    $("confirmOk").textContent = okLabel;
    modal.hidden = false;
    const ok = $("confirmOk"), cancel = $("confirmCancel");
    const done = (v) => {
      modal.hidden = true;
      ok.removeEventListener("click", onOk);
      cancel.removeEventListener("click", onCancel);
      resolve(v);
    };
    const onOk = () => done(true);
    const onCancel = () => done(false);
    ok.addEventListener("click", onOk);
    cancel.addEventListener("click", onCancel);
  });
}

/**
 * Run a mutating command (add/remove) and reload the list, since it changes the
 * set of slots. Errors go to the log.
 */
function mutate(cmd) {
  send(cmd).then(() => refresh()).catch((e) => log("ERR " + e.message));
}

/**
 * Shades step: the primary list of configured shades. One row per shade with a
 * name (click to expand its settings), an On switch (exposure over Thread), the
 * last position, motor controls, and Remove. Each row is followed by a hidden
 * detail row holding that shade's full settings — address, rolling, linked remote,
 * travel times, favourite, invert — so nothing lives in a separate menu.
 */
function renderManage() {
  const tb = $("manageBody");
  tb.textContent = "";
  $("shadeTable").hidden = shades.length === 0;
  $("shadeEmpty").hidden = shades.length !== 0;
  for (const s of shades) {
    const tr = document.createElement("tr");
    tr.classList.toggle("disabled", !s.on);
    const detail = shadeDetailRow(s);
    tr.append(nameTd(s, detail));
    tr.append(switchTd(s.on, (on) => save(`on ${s.idx} ${on ? 1 : 0}`)));
    tr.append(posTd(s));
    tr.append(motorTd(s.idx));
    const rm = document.createElement("td");
    rm.append(mkBtn(t("shades.remove"), async () => {
      if (await askConfirm(t("confirm.remove", { name: s.name || s.idx }), t("shades.remove")))
        mutate(`remove ${s.idx}`);
    }, "danger small"));
    tr.append(rm);
    tb.append(tr, detail);
  }
}

/**
 * The per-shade settings row, hidden until its name is clicked. Fields wrap in a
 * responsive grid so the panel fits any width. `pos` fields go through savePos()
 * (one `pos` command carries them all); the rest are independent setters.
 */
function shadeDetailRow(s) {
  const row = document.createElement("tr");
  row.className = "detailrow";
  row.hidden = true;
  const cell = document.createElement("td");
  cell.colSpan = 5;
  const help = document.createElement("p");
  help.className = "muted";
  help.textContent = t("pos.advHelp");
  const grid = document.createElement("div");
  grid.className = "detailgrid";
  grid.append(
    field("shades.col.name", textInput(s.name, (v) => save(`name ${s.idx} ${v}`), 15)),
    field("shades.col.address", textInput(String(s.addr), (v) => save(`addr ${s.idx} ${v}`))),
    field("shades.col.rolling", textInput(String(s.rolling), (v) => save(`roll ${s.idx} ${v}`))),
    field("shades.col.linked", linkControl(s)),
    ...posFields(s),
    field("pos.col.my", myControl(s)),
    field("pos.col.invert", switchEl(!!s.invert, (on) => savePos(s, { invert: on }))),
    field("motor.progLabel", mkBtn(t("motor.prog"), () => send(`tx ${s.idx} prog`), "small")),
  );
  cell.append(help, grid);
  row.append(cell);
  return row;
}

/** A labelled field for the detail grid: an uppercase caption above its control. */
function field(labelKey, control) {
  const f = document.createElement("div");
  f.className = "field";
  const cap = document.createElement("span");
  cap.className = "fieldlabel";
  cap.textContent = t(labelKey);
  f.append(cap, control);
  return f;
}

/** A committing text input (returns the bare element, for use inside a field). */
function textInput(val, onCommit, maxLen) {
  const inp = document.createElement("input");
  inp.type = "text";
  inp.className = "num";
  inp.value = val;
  if (maxLen) inp.maxLength = maxLen;
  inp.addEventListener("change", () => onCommit(inp.value.trim()));
  return inp;
}

/**
 * Commit shade `s`'s position-estimate params. The `pos` command takes every
 * field at once (travel times, favourite, invert, per-direction startup lag), so
 * merge the patch over the current values and send the lot, updating the local
 * copy so a follow-up edit builds on fresh state without a full refresh. `my` 255
 * means unset.
 */
function savePos(s, patch) {
  Object.assign(s, patch);
  const my = (s.my === undefined || s.my === "") ? 255 : s.my;
  save(`pos ${s.idx} ${s.up_ms || 0} ${s.down_ms || 0} ${my} ${s.invert ? 1 : 0} ${s.up_lag || 0} ${s.down_lag || 0}`);
}

/** A millisecond number input (0+), blank when unset. */
function numInput(val) {
  const inp = document.createElement("input");
  inp.type = "number";
  inp.className = "num";
  inp.min = "0";
  inp.value = val || "";
  return inp;
}

/**
 * The four travel-time inputs (open/close time + startup lag) plus a per-direction
 * assisted-calibration button. Editing any input commits all four at once via
 * savePos(); the calibrate buttons fill their direction's lag + time live.
 */
function posFields(s) {
  const upMs = numInput(s.up_ms), upLag = numInput(s.up_lag);
  const downMs = numInput(s.down_ms), downLag = numInput(s.down_lag);
  const iv = (i) => parseInt(i.value, 10) || 0;
  const commit = () => savePos(s, {
    up_ms: iv(upMs), up_lag: iv(upLag), down_ms: iv(downMs), down_lag: iv(downLag),
  });
  const inputs = { up_ms: upMs, up_lag: upLag, down_ms: downMs, down_lag: downLag };
  [upMs, upLag, downMs, downLag].forEach((i) => i.addEventListener("change", commit));
  return [
    field("pos.col.up", upMs),
    field("pos.col.upLag", upLag),
    field("pos.col.down", downMs),
    field("pos.col.downLag", downLag),
    field("cal.title", mkBtn(t("cal.launch"), () => openCalibration(s, inputs), "small primary")),
  ];
}

/* ── calibration modal (gamified, assisted travel-time measurement) ─────── */

// Two rounds: open then close. Each is a 3-tap sequence (go → first movement →
// end stop) that transmits and times the real motor, capturing startup lag and
// travel time for that direction.
const CAL_ROUNDS = [
  { dir: "up", lagKey: "up_lag", msKey: "up_ms", labelKey: "cal.roundOpen", readyKey: "cal.readyOpen" },
  { dir: "down", lagKey: "down_lag", msKey: "down_ms", labelKey: "cal.roundClose", readyKey: "cal.readyClose" },
];

// Startup lag (command → first movement) is a global metric, measured once and
// reused for every round and every shade. Cached in localStorage; each calibrated
// shade still persists it into its own up_lag/down_lag on the board.
const CAL_LAG_KEY = "calLagMs";
const getCalLag = () => { const v = parseInt(localStorage.getItem(CAL_LAG_KEY), 10); return Number.isFinite(v) ? v : null; };
const setCalLag = (ms) => localStorage.setItem(CAL_LAG_KEY, String(ms));

/**
 * Run the gamified calibration wizard for shade `s`. Walks the two rounds, each
 * with phases ready → timing-lag → timing-travel → scored, then writes the four
 * measured values to `inputs` and persists them via savePos(). Transmits `tx`
 * frames (moves the shade), so it lives behind an explicit launch button.
 */
function openCalibration(s, inputs) {
  const modal = $("calModal");
  const results = {};
  let round = 0, phase = "ready", tCmd = 0, tMove = 0, ticker = 0, action = null;

  const stopTicker = () => { clearInterval(ticker); ticker = 0; };
  const onKey = (e) => {
    if (e.code === "Space" || e.key === " ") { e.preventDefault(); if (action) action.click(); }
  };
  const close = () => {
    stopTicker();
    document.removeEventListener("keydown", onKey);
    modal.hidden = true;
  };

  const secs = (ms) => (ms / 1000).toFixed(1);

  function render() {
    const r = CAL_ROUNDS[round];
    const body = $("calBody");
    body.textContent = "";

    const dots = document.createElement("div");
    dots.className = "cal-dots";
    CAL_ROUNDS.forEach((_, i) => {
      const d = document.createElement("span");
      d.className = "cal-dot" + (i < round ? " done" : i === round ? " active" : "");
      dots.append(d);
    });

    const title = document.createElement("h3");
    title.className = "cal-title";
    title.textContent = `${t("cal.heading", { name: s.name || s.idx })} — ${t(r.labelKey)}`;

    const timer = document.createElement("div");
    timer.className = "cal-timer";
    timer.textContent = "0.0";

    const hint = document.createElement("p");
    hint.className = "cal-hint";

    action = mkBtn("", () => {}, "primary cal-tap");
    const cancel = mkBtn(t("cal.cancel"), close, "small");
    let extra = null;

    const runTimer = (from) => {
      stopTicker();
      ticker = setInterval(() => { timer.textContent = secs(Math.max(0, performance.now() - from)); }, 50);
    };

    if (phase === "ready") {
      hint.textContent = t(r.readyKey);
      action.textContent = t("cal.go");
      action.onclick = () => {
        send(`tx ${s.idx} ${r.dir}`);
        tCmd = performance.now();
        const lag = getCalLag();
        if (lag == null) { phase = "lag"; runTimer(tCmd); }
        else { tMove = tCmd + lag; phase = "travel"; runTimer(tMove); }
        render();
      };
      if (getCalLag() != null) {
        extra = mkBtn(t("cal.remeasureLag"), () => { localStorage.removeItem(CAL_LAG_KEY); render(); }, "small");
      }
    } else if (phase === "lag") {
      hint.textContent = t("cal.tapMove");
      action.textContent = t("cal.tapMoveBtn");
      runTimer(tCmd);
      action.onclick = () => { tMove = performance.now(); setCalLag(Math.round(tMove - tCmd)); phase = "travel"; render(); };
    } else if (phase === "travel") {
      hint.textContent = t("cal.tapEnd");
      action.textContent = t("cal.tapEndBtn");
      runTimer(tMove);
      action.onclick = () => {
        stopTicker();
        results[r.lagKey] = Math.round(tMove - tCmd);
        results[r.msKey] = Math.round(performance.now() - tMove);
        phase = "scored"; render();
      };
    } else {
      stopTicker();
      timer.textContent = "";
      icon("check").then((svg) => { timer.innerHTML = svg; });
      hint.innerHTML = t("cal.scored", { lag: secs(results[r.lagKey]), travel: secs(results[r.msKey]) });
      const last = round === CAL_ROUNDS.length - 1;
      action.textContent = last ? t("cal.finish") : t("cal.next");
      action.onclick = () => {
        if (last) {
          for (const k in results) if (inputs[k]) inputs[k].value = results[k];
          savePos(s, results);
          close();
        } else { round++; phase = "ready"; render(); }
      };
    }

    body.append(dots, title, timer, hint, action, cancel);
    if (extra) body.append(extra);
  }

  modal.hidden = false;
  document.addEventListener("keydown", onKey);
  render();
}

/** Favourite-position control: a 0–100 percent input, blank when unset (255). */
function myControl(s) {
  const inp = document.createElement("input");
  inp.type = "number";
  inp.className = "num";
  inp.min = "0";
  inp.max = "100";
  inp.value = (s.my === 255 || s.my === undefined) ? "" : s.my;
  inp.addEventListener("change", () => {
    const v = inp.value === "" ? 255 : Math.max(0, Math.min(100, parseInt(inp.value, 10) || 0));
    savePos(s, { my: v });
  });
  return inp;
}

/**
 * Position cell: the shade's estimated position as percent closed (0 = open,
 * 100 = closed), read-only. Tagged with data-pos so the shades-step poll
 * (startPosPoll) can patch it live as a move — including a linked-remote
 * press — ramps.
 */
function posTd(s) {
  const cell = document.createElement("td");
  cell.className = "muted";
  cell.dataset.pos = s.idx;
  cell.textContent = `${Math.round((s.pos || 0) / 100)}%`;
  return cell;
}

/**
 * Name cell: a remote-link indicator followed by the name as a disclosure button
 * that toggles the shade's detail row. The badge is lit when a physical wall
 * remote is linked (its presses mirror into the position) and muted otherwise.
 */
function nameTd(s, detail) {
  const cell = document.createElement("td");
  const wrap = document.createElement("div");
  wrap.className = "namecell";
  const linked = s.link && s.link !== "000000";
  const badge = document.createElement("span");
  badge.className = "remote-badge" + (linked ? " on" : "");
  badge.title = t(linked ? "shades.remoteLinked" : "shades.remoteNone");
  icon("remote").then((svg) => { badge.innerHTML = svg; });
  const toggle = document.createElement("button");
  toggle.type = "button";
  toggle.className = "namebtn";
  toggle.textContent = s.name || String(s.idx);
  toggle.setAttribute("aria-expanded", "false");
  toggle.addEventListener("click", () => {
    detail.hidden = !detail.hidden;
    toggle.setAttribute("aria-expanded", String(!detail.hidden));
  });
  wrap.append(badge, toggle);
  cell.append(wrap);
  return cell;
}

/** Motor cell: Open / Stop / Close as icon buttons (Somfy "My" is the stop button). */
function motorTd(idx) {
  const cell = document.createElement("td");
  const acts = document.createElement("div");
  acts.className = "ctrls";
  acts.append(iconBtn("open", t("motor.open"), () => send(`tx ${idx} up`)));
  acts.append(iconBtn("stop", t("motor.stop"), () => send(`tx ${idx} stop`)));
  acts.append(iconBtn("close", t("motor.close"), () => send(`tx ${idx} down`)));
  cell.append(acts);
  return cell;
}

/** Icon-only button: labelled for a11y via title + aria-label, glyph loaded async. */
function iconBtn(name, label, onClick) {
  const b = mkBtn("", onClick, "icon small");
  b.title = label;
  b.setAttribute("aria-label", label);
  icon(name).then((svg) => { b.innerHTML = svg; });
  return b;
}

/** A styled toggle switch (a checkbox) — the bare control, for use in a cell or field. */
function switchEl(on, onToggle) {
  const lab = document.createElement("label");
  lab.className = "switch";
  const inp = document.createElement("input");
  inp.type = "checkbox";
  inp.checked = on;
  inp.addEventListener("change", () => onToggle(inp.checked));
  const slider = document.createElement("span");
  slider.className = "slider";
  lab.append(inp, slider);
  return lab;
}

/** Cell wrapping a toggle switch for the On state. */
function switchTd(on, onToggle) {
  const cell = document.createElement("td");
  cell.append(switchEl(on, onToggle));
  return cell;
}

function mkBtn(label, onClick, cls = "") {
  const b = document.createElement("button");
  b.className = ("btn " + cls).trim();
  b.textContent = label;
  b.addEventListener("click", onClick);
  return b;
}

/* ── discovery ────────────────────────────────────────────────────────── */

let discovering = false;
let linking = false;
const seen = new Set();

/** @return the set of shade addresses already configured, upper-case hex. */
function knownAddrs() {
  return new Set(shades.map((s) => String(s.addr).toUpperCase()));
}

/**
 * Discovery mode: listen for Somfy remote frames and offer each newly heard
 * address as a shade to name and add. Loops on unsolicited [RX] lines until the
 * user clicks Done — the guided way to onboard a whole fleet from its remotes,
 * no motor re-pairing needed.
 */
async function startDiscover() {
  if (discovering) return;
  discovering = true;
  seen.clear();
  $("discoverCards").textContent = "";
  $("discoverPanel").hidden = false;
  const known = knownAddrs();
  while (discovering) {
    let line;
    try {
      line = await waitFor((l) => l.includes("[RX] addr="), 60000);
    } catch (e) {
      continue;
    }
    const ma = line.match(/addr=0x([0-9A-Fa-f]+)/);
    const mc = line.match(/code=(\d+)/);
    if (!ma) continue;
    const addr = ma[1].toUpperCase().padStart(6, "0");
    if (known.has(addr) || seen.has(addr)) continue;
    seen.add(addr);
    addDiscoverCard(addr, mc ? Number(mc[1]) : 0);
  }
}

/** Stop the discovery loop and hide its panel. */
function stopDiscover() {
  discovering = false;
  $("discoverPanel").hidden = true;
}

/**
 * Show a card for a newly heard remote: its address, a name field, and
 * Add/Ignore. Add registers the shade seeding rolling from the heard code + 1
 * so our first transmit is not stale-rejected.
 */
function addDiscoverCard(addr, code) {
  const card = document.createElement("div");
  card.className = "dcard";
  const title = document.createElement("div");
  title.className = "dcard-addr";
  title.textContent = t("shades.heardAddr", { addr });
  const name = document.createElement("input");
  name.type = "text";
  name.className = "name";
  name.maxLength = 15;
  name.placeholder = t("shades.namePlaceholder");
  const add = mkBtn(t("shades.addHeard"), () => {
    mutate(`add ${addr} ${code + 1} ${name.value.trim()}`);
    card.remove();
  }, "primary small");
  const ignore = mkBtn(t("shades.ignore"), () => card.remove(), "small");
  const row = document.createElement("div");
  row.className = "bar";
  row.append(name, add, ignore);
  card.append(title, row);
  $("discoverCards").append(card);
}

/**
 * Advanced cell for the monitored physical remote: shows the linked address (or
 * a dash) with a Link/Unlink action. Link arms a one-shot RF capture; Unlink
 * clears it.
 */
function linkControl(s) {
  const wrap = document.createElement("div");
  wrap.className = "ctrl";
  const has = s.link && s.link !== "000000";
  const label = document.createElement("span");
  label.textContent = has ? s.link : t("shades.linkNone");
  const btn = mkBtn(t(has ? "shades.unlink" : "shades.linkRemote"),
    () => (has ? mutate(`unlink ${s.idx}`) : linkRemote(s.idx)), "small");
  wrap.append(label, btn);
  return wrap;
}

/**
 * Associate a physical wall remote with shade `idx`: open a blocking modal, then
 * listen for the next RF frame whose address is not already a shade's own
 * address and store it as the monitored linked remote (seeding rolling from the
 * heard code). The modal's Cancel button aborts via cancelLink().
 */
async function linkRemote(idx) {
  if (linking) return;
  linking = true;
  const known = knownAddrs();
  $("linkModal").hidden = false;
  try {
    while (linking && connected) {
      let line;
      try {
        line = await waitFor((l) => l.includes("[RX] addr="), 30000);
      } catch (e) {
        continue;
      }
      if (!linking) return;
      const ma = line.match(/addr=0x([0-9A-Fa-f]+)/);
      const mc = line.match(/code=(\d+)/);
      if (!ma) continue;
      const addr = ma[1].toUpperCase().padStart(6, "0");
      if (known.has(addr)) continue;
      mutate(`link ${idx} ${addr} ${mc ? mc[1] : 0}`);
      return;
    }
  } finally {
    linking = false;
    $("linkModal").hidden = true;
  }
}

/** Cancel a pending linkRemote() capture and close its modal. */
function cancelLink() {
  linking = false;
  $("linkModal").hidden = true;
}

/** Add a motor without a remote: the firmware invents an address; then PROG it. */
function addMotor() {
  mutate(`add`);
}

/* ── actions ──────────────────────────────────────────────────────────── */

/**
 * Open the serial port, start reading, then fingerprint the firmware. Pass a
 * pre-granted port (from getPorts, e.g. the post-flash reconnect) to reuse it
 * without a second picker; omit to prompt the browser's port chooser.
 */
async function connect(existing) {
  if (!("serial" in navigator)) { alert(t("alert.webserial")); return; }
  port = existing || await navigator.serial.requestPort();
  lastPort = port;
  await port.open({ baudRate: 115200 });
  writer = port.writable.getWriter();
  readLoop();
  connected = true;
  $("dot").classList.add("on");
  $("statusText").textContent = t("status.connected");
  $("connect").disabled = true;
  $("portHint").hidden = true;
  $("logWrap").open = true;
  $("disconnModal").hidden = true;
  await detect();
}

/**
 * Handle an unexpected loss of the board (USB unplug, or a serial port that
 * died). Tears down the connection, stops any listening loop, flips the UI to
 * disconnected, and shows a reconnect banner. Idempotent: a no-op once already
 * disconnected, so the serial event and the read-loop end don't double-fire.
 */
function onDisconnect() {
  if (!connected) return;
  connected = false;
  discovering = false;
  linking = false;
  scanning = false;
  stopPosPoll();
  stopMatterPoll();
  try { if (writer) writer.releaseLock(); } catch (e) { /* already released */ }
  try { if (port) port.close(); } catch (e) { /* already closing */ }
  reader = null; pipeAbort = null; pipeDone = null; writer = null; port = null;
  $("dot").classList.remove("on");
  $("statusText").textContent = t("status.disconnected");
  $("connect").disabled = false;
  $("connect").textContent = t("board.recheck");
  $("portHint").hidden = false;
  $("discoverPanel").hidden = true;
  $("linkModal").hidden = true;
  $("disconnModal").hidden = false;
  gateNext();
}

/**
 * Release the serial port so esptool-js can reopen the same device for flashing,
 * and reset the connection UI. The read loop pipes port.readable into a decoder,
 * which *locks* port.readable — so port.close() would reject while that pipe is
 * live. Cancel the reader and abort the pipe first (unlocking port.readable),
 * then close; otherwise the port stays open and esptool's Transport hits "Port
 * is already open" when it reopens the same device.
 */
async function releasePort() {
  connected = false;
  try { if (reader) await reader.cancel(); } catch (e) { /* already gone */ }
  try { if (pipeAbort) pipeAbort.abort(); } catch (e) { /* already aborted */ }
  try { if (pipeDone) await pipeDone; } catch (e) { /* settled via its own catch */ }
  try { if (writer) writer.releaseLock(); } catch (e) { /* already released */ }
  try { if (port) await port.close(); } catch (e) { /* already closing */ }
  reader = null; pipeAbort = null; pipeDone = null; writer = null; port = null;
  $("dot").classList.remove("on");
  $("statusText").textContent = t("status.disconnected");
  $("connect").disabled = false;
  $("connect").textContent = t("board.recheck");
  $("next").disabled = true;
}

/**
 * Ask the board for its firmware id/version and branch the wizard: no reply →
 * reveal the flasher (blank board); proto older than this site → require an
 * update, Next stays disabled; otherwise the board is compatible → show the
 * installed version and a reflash/update button, Next advances. The version
 * probe is retried up to five times because a just-connected or freshly
 * rebooted board takes a second or two before its console answers. The proto
 * number is the compatibility gate; the version string is the firmware's git
 * tag, flagged as outdated only when it differs from the latest release tag.
 */
async function detect() {
  const det = $("detect");
  $("flashModal").hidden = true;
  $("flasher").hidden = true;
  det.hidden = false;
  det.textContent = t("detect.checking");
  let ver = null, proto = 0;
  for (let i = 0; i < 5 && !ver; i++) {
    det.textContent = t("detect.checking") + ` (${i + 1}/5)`;
    try {
      const line = await request("version", (l) => l.startsWith("somfy-thread "), 1500);
      const m = line.match(/^somfy-thread (\S+)(?: proto (\d+))?/);
      if (m) { ver = m[1]; proto = m[2] ? parseInt(m[2], 10) : 0; }
    } catch (e) { log(`detect: no version reply (${i + 1}/5) — ${e.message}`); await sleep(400); }
  }
  if (!ver) log("detect: board did not identify as somfy-thread — treating as blank");
  else log(`detect: somfy-thread ${ver} proto ${proto}`);

  if (!ver) {
    det.textContent = t("detect.none");
    await beginFlash();
    return;
  }

  if (proto < REQUIRED_PROTO) {
    det.textContent = "";
    det.append(t("detect.incompatible", { ver }));
    det.append(mkBtn(t("detect.update"), () => beginFlash(), "primary small"));
    $("next").disabled = true;
    return;
  }

  await refresh().catch((e) => log("ERR " + e.message));
  // An already-set-up board (radio present + a shade configured) skips the radio
  // check — that step only exists to get a noob's radio working the first time.
  try {
    const st = JSON.parse(await request("radio", (l) => l.startsWith("{")));
    skipRadio = !!st.rf && shades.some((s) => s.on);
  } catch (e) { /* leave the radio step in place */ }
  const norm = (s) => (s || "").replace(/^v/, "");
  const latest = releases[0] ? releases[0].tag_name : null;
  const outdated = latest && !norm(ver).startsWith(norm(latest)) && !norm(latest).startsWith(norm(ver));
  det.textContent = "";
  det.append(outdated ? t("detect.outdated", { ver, latest }) : t("detect.current", { ver }));
  det.append(mkBtn(t(outdated ? "detect.update" : "detect.reflash"), () => beginFlash(), "small"));
  $("next").disabled = false;
}

/** Close the serial port and reveal the release picker + flash controls. */
async function beginFlash() {
  await releasePort();
  populateReleaseSelect();
  $("flashControls").hidden = false;
  $("flashModal").hidden = true;
  $("flashBar").style.width = "0%";
  $("flasher").hidden = false;
}

/** Download the current config (global radio freq + shade table) as JSON. */
async function exportBackup() {
  const line = await request("export", (l) => l.startsWith("["));
  const freq = await request("freq", (l) => /^\d+\.\d+$/.test(l.trim()));
  const data = JSON.stringify({ freq: parseFloat(freq), shades: JSON.parse(line) });
  const blob = new Blob([data], { type: "application/json" });
  const a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  a.download = "somfy-thread-backup.json";
  a.click();
  URL.revokeObjectURL(a.href);
}

/**
 * Restore a backup onto a fresh (or factory-reset) device: set the global radio
 * frequency, then re-create each shade with `add` (address + rolling + name) and
 * restore its linked remote and position-estimate params (travel times, favourite,
 * invert). Shades come back exposed; any that were off in the backup are switched
 * off afterwards. Identity is by radio address, so the re-assigned slot index does
 * not matter.
 */
async function importBackup(file) {
  const data = JSON.parse(await file.text());
  const list = Array.isArray(data) ? data : data.shades;
  if (data.freq) await send(`freq ${Number(data.freq).toFixed(3)}`);
  for (const s of list) {
    const line = await request(`add ${s.addr} ${s.rolling} ${s.name}`,
                               (l) => l.startsWith("OK") || l.startsWith("ERR"), 5000);
    const m = line.match(/^OK (\d+)/);
    if (!m) continue;
    const idx = m[1];
    if (s.link && s.link !== "000000") await send(`link ${idx} ${s.link}`);
    if (s.up_ms || s.down_ms || s.invert || s.up_lag || s.down_lag || (s.my !== undefined && s.my !== 255))
      await send(`pos ${idx} ${s.up_ms || 0} ${s.down_ms || 0} ${s.my ?? 255} ${s.invert ? 1 : 0} ${s.up_lag || 0} ${s.down_lag || 0}`);
    if (s.on === false) await send(`on ${idx} 0`);
  }
  await refresh();
}

let pairQr = "";

/** Hide the pairing code, QR image, and payload (paired state, before asked). */
function hidePairing() {
  $("paircode").hidden = true;
  $("qrimg").hidden = true;
  $("qrpayload").hidden = true;
  $("qrToggle").hidden = true;
}

/**
 * Passively watch the board's CHIP output while on the Matter step and flag a
 * failed pairing. Commissioning runs on the hub, so the outcome only appears in
 * the serial stream, never in a command reply — a "Commissioning failed" line
 * means the hub aborted (it refused the device, or the attempt timed out).
 */
function watchMatter(line) {
  if (step !== MATTER_STEP || !line.includes("Commissioning failed")) return;
  const st = $("matterStatus");
  st.hidden = false;
  st.classList.add("bad");
  st.textContent = t("matter.failed");
  toast(t("matter.failedToast"), false);
}

let matterFabrics = -1;

/**
 * Reflect the board's commissioning state. Not paired → open a commissioning
 * window and show the code; already paired → report it, hide the code/QR, and
 * offer "add another ecosystem" (multi-admin). Only a change in fabric count
 * acts, so the live poll never re-opens the window or clears the QR toggle mid
 * pairing — it just flips the view the moment the hub finishes (or drops) a
 * commission, without the user leaving the step.
 */
function renderMatterState(fabrics) {
  if (fabrics === matterFabrics) return;
  matterFabrics = fabrics;
  const st = $("matterStatus");
  const btn = $("pairBtn");
  st.hidden = false;
  st.classList.remove("bad");
  if (fabrics > 0) {
    st.textContent = t("matter.paired", { n: fabrics });
    btn.textContent = t("matter.addAnother");
    btn.hidden = false;
    hidePairing();
  } else {
    st.textContent = t("matter.unpaired");
    btn.hidden = true;
    showPairing().catch((e) => log("ERR " + e.message));
  }
}

/** Query the board's fabric count once and render it. */
async function pollMatterOnce() {
  let fabrics = 0;
  try { fabrics = JSON.parse(await request("mstat", (l) => l.startsWith("{"))).fabrics; }
  catch (e) { /* treat an unresponsive board as unpaired */ }
  renderMatterState(fabrics);
}

/**
 * Matter step: show a checking state, then render the commissioning state and
 * keep it live via startMatterPoll so a pairing done on the hub is reflected
 * without a back/next.
 */
async function loadMatter() {
  matterFabrics = -1;
  hidePairing();
  const st = $("matterStatus");
  st.hidden = false;
  st.classList.remove("bad");
  st.textContent = t("matter.checking");
  await pollMatterOnce();
  startMatterPoll();
}

/**
 * Open a commissioning window, show the manual code, and arm the "Show QR"
 * toggle. The QR payload is fetched now but only rendered when the user asks.
 */
async function showPairing() {
  const manual = await request("pair", (l) => /^\d{11,}$/.test(l.replace(/-/g, "")));
  const code = $("paircode");
  code.hidden = false;
  code.textContent = manual;
  pairQr = "";
  try { pairQr = await request("qr", (l) => l.startsWith("MT:")); } catch (e) { /* payload optional */ }
  $("qrToggle").hidden = !pairQr;
}

/** Render the QR image + payload on demand (from the "Show QR" toggle). */
function renderQr() {
  $("qrToggle").hidden = true;
  const img = $("qrimg");
  if (pairQr && window.qrcode) {
    const q = window.qrcode(0, "M");
    q.addData(pairQr);
    q.make();
    img.src = q.createDataURL(6, 16);
    img.hidden = false;
  }
  const pl = $("qrpayload");
  pl.hidden = false;
  pl.textContent = pairQr ? t("matter.qr", { qr: pairQr }) : "";
}

/* ── wiring ───────────────────────────────────────────────────────────── */

$("connect").addEventListener("click", () => connect().catch((e) => log("ERR " + e.message)));
$("discover").addEventListener("click", () => startDiscover().catch((e) => log("ERR " + e.message)));
$("discoverDone").addEventListener("click", () => stopDiscover());
$("linkCancel").addEventListener("click", () => cancelLink());
$("addMotor").addEventListener("click", () => addMotor());
$("reconnect").addEventListener("click", () => connect().catch((e) => log("ERR " + e.message)));
if ("serial" in navigator)
  navigator.serial.addEventListener("disconnect", (e) => { if (e.target === port) onDisconnect(); });
$("refreshReleases").title = t("flasher.refresh");
$("refreshReleases").setAttribute("aria-label", t("flasher.refresh"));
$("refreshReleases").addEventListener("click", () =>
  fetchReleases().then(populateReleaseSelect).catch((e) => log("ERR " + e.message)));
$("flash").addEventListener("click", () => flashSelected());
$("flashClose").addEventListener("click", () => { $("flashModal").hidden = true; $("flash").disabled = false; });
$("next").addEventListener("click", () => setStep(stepIn(1)));
$("back").addEventListener("click", () => setStep(stepIn(-1)));

/** Jump straight to an already-completed step by clicking (or Enter/Space on) its stepper item. */
function stepperNav(e) {
  if (e.type === "keydown" && e.key !== "Enter" && e.key !== " ") return;
  const li = e.target.closest("li");
  if (!li) return;
  const i = [...$("stepper").children].indexOf(li);
  if (i >= 0 && i < step) { e.preventDefault(); setStep(i); }
}
$("stepper").addEventListener("click", stepperNav);
$("stepper").addEventListener("keydown", stepperNav);
$("dlLog").addEventListener("click", () => {
  const blob = new Blob([logEl.textContent], { type: "text/plain" });
  const a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  a.download = "somfy-thread-serial.log";
  a.click();
  URL.revokeObjectURL(a.href);
});
$("export").addEventListener("click", () => exportBackup().catch((e) => log("ERR " + e.message)));
$("import").addEventListener("click", () => $("importFile").click());
$("importFile").addEventListener("change", (e) => {
  if (e.target.files[0]) importBackup(e.target.files[0]).catch((err) => log("ERR " + err.message));
});
$("radioFreq").addEventListener("change", (e) => save(`freq ${e.target.value}`));
$("radioListen").addEventListener("click", () => scanAndListen());
$("radioPower").addEventListener("change", (e) => save(`power ${e.target.value}`));
$("radioRxbw").addEventListener("change", (e) => save(`rxbw ${e.target.value}`));
$("radioScan").addEventListener("click", () => scanBand().catch((e) => log("ERR " + e.message)));
$("pairBtn").addEventListener("click", () => showPairing().catch((e) => log("ERR " + e.message)));
$("qrToggle").addEventListener("click", () => renderQr());
$("matterReset").addEventListener("click", async () => {
  if (await askConfirm(t("confirm.resetMatter"))) send("reset");
});
$("resetBtn").addEventListener("click", async () => {
  if (await askConfirm(t("confirm.factory"))) send("factory");
});

const iconCache = {};

/**
 * Fetch icons/<name>.svg once (cached), rewriting any hard-coded hex fill to
 * currentColor so the glyph follows the theme's text color (black/white) rather
 * than the color it was exported with.
 */
async function icon(name) {
  if (!(name in iconCache)) {
    const r = await fetch(`./icons/${name}.svg`);
    iconCache[name] = r.ok ? (await r.text()).replace(/fill="#[0-9A-Fa-f]{3,8}"/g, 'fill="currentColor"') : "";
  }
  return iconCache[name];
}

/** Inline each [data-icon] element's SVG. */
function loadIcons() {
  document.querySelectorAll("[data-icon]").forEach(async (el) => { el.innerHTML = await icon(el.dataset.icon); });
}

/**
 * Show the deployed site build in the footer. version.txt is stamped with the
 * commit by the Pages workflow at deploy; it stays "dev" when served locally or
 * unstamped.
 */
async function loadSiteVersion() {
  try {
    const r = await fetch("./version.txt");
    if (r.ok) $("siteVer").textContent = (await r.text()).trim() || "dev";
  } catch { /* keep the "dev" default */ }
}

const THEME_CYCLE = ["light", "dark", "system"];
const themeBtn = $("theme");

/** Apply a theme, persist it, and show its icon. "system" clears the override so the OS decides. */
async function setTheme(v) {
  if (v === "system") document.documentElement.removeAttribute("data-theme");
  else document.documentElement.dataset.theme = v;
  localStorage.setItem("theme", v);
  themeBtn.dataset.themeVal = v;
  const label = t("theme." + v);
  themeBtn.title = label;
  themeBtn.setAttribute("aria-label", label);
  themeBtn.innerHTML = await icon(v);
}
themeBtn.addEventListener("click", () => {
  const next = THEME_CYCLE[(THEME_CYCLE.indexOf(themeBtn.dataset.themeVal) + 1) % THEME_CYCLE.length];
  setTheme(next);
});
setTheme(localStorage.getItem("theme") || "system");

/** Wire the language picker: a translate-icon button opening a menu of the registered languages. */
const langBtn = $("lang");
const langMenu = $("langMenu");
langBtn.title = t("lang.choose");
langBtn.setAttribute("aria-label", t("lang.choose"));
for (const { code, name } of languages()) {
  const li = document.createElement("li");
  li.setAttribute("role", "option");
  li.setAttribute("aria-selected", code === getLang() ? "true" : "false");
  li.tabIndex = 0;
  li.textContent = name;
  const pick = () => setLang(code);
  li.addEventListener("click", pick);
  li.addEventListener("keydown", (e) => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); pick(); } });
  langMenu.append(li);
}
const closeLangMenu = () => { langMenu.hidden = true; langBtn.setAttribute("aria-expanded", "false"); };
langBtn.addEventListener("click", (e) => {
  e.stopPropagation();
  const opening = langMenu.hidden;
  langMenu.hidden = !opening;
  langBtn.setAttribute("aria-expanded", String(opening));
});
document.addEventListener("click", closeLangMenu);
document.addEventListener("keydown", (e) => { if (e.key === "Escape") closeLangMenu(); });

loadIcons();
loadSiteVersion();
applyI18n();
fetchReleases();
setStep(0);
