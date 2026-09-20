// SPDX-License-Identifier: Unlicense
// somfy-thread setup wizard: connect to the board over Web Serial, detect the
// firmware and its version (offer update / continue / flash), then configure
// (shades, motor PROG/test, Matter pairing, backup). One page, all client-side;
// only the firmware .bin is fetched (from GitHub Releases) when flashing.

import { t, applyI18n } from "./i18n.js";

const $ = (id) => document.getElementById(id);
const logEl = $("log");

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

/**
 * Build a one-part ESP Web Tools manifest pointing at an absolute .bin URL and
 * return a blob: URL for it. The absolute part path lets the manifest live at a
 * blob: URL while the binary is fetched from GitHub.
 */
function manifestUrl(binUrl) {
  const manifest = {
    name: "somfy-thread",
    builds: [{ chipFamily: "ESP32-C6", parts: [{ path: binUrl, offset: 0 }] }],
  };
  return URL.createObjectURL(new Blob([JSON.stringify(manifest)], { type: "application/json" }));
}

/** Find the flashable firmware asset (a merged *.bin) in a release. */
function firmwareAsset(release) {
  return (release.assets || []).find((x) => x.name.endsWith(".bin")) || null;
}

let releases = [];

/** Apply the selected release to the install button, changelog, and fallback. */
function selectRelease(idx) {
  const r = releases[idx];
  const asset = firmwareAsset(r);
  const installer = $("installer");
  const fb = $("fallback");
  if (!asset) {
    installer.removeAttribute("manifest");
    fb.textContent = t("release.noBin");
  } else {
    installer.setAttribute("manifest", manifestUrl(asset.browser_download_url));
    fb.textContent = "";
    const link = document.createElement("a");
    link.href = asset.browser_download_url;
    link.textContent = t("release.download", { name: asset.name });
    fb.append(t("release.fallbackPrefix"), link, t("release.fallbackSuffix"));
  }
  $("changelog").textContent = r.body || "(no notes)";
  $("changelogBox").hidden = false;
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
  port.readable.pipeTo(dec.writable).catch(() => {});
  const reader = dec.readable.getReader();
  let buf = "";
  try {
    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      buf += value;
      let nl;
      while ((nl = buf.indexOf("\n")) >= 0) {
        const line = buf.slice(0, nl).replace(/\r$/, "").trim();
        buf = buf.slice(nl + 1);
        if (line) { log(line); dispatch(line); }
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
  await writer.write(new TextEncoder().encode(cmd + "\n"));
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
    li.classList.toggle("active", i === step);
    li.classList.toggle("done", i < step);
  });
  $("back").hidden = step === 0;
  $("next").hidden = step === STEPS - 1;
  gateNext();
  if (step === RADIO_STEP) loadRadio().catch((e) => log("ERR " + e.message));
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
  const el = $("radioStatus");
  el.textContent = radioReady ? t("radio.ok") : t("radio.absent");
  el.classList.toggle("bad", !radioReady);
  gateNext();
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

/** Fire-and-forget a setter command (device replies OK/ERR to the log). */
function save(cmd) { send(cmd).catch((e) => log("ERR " + e.message)); }

/**
 * Run a mutating command (add/remove) and reload the list, since it changes the
 * set of slots. Errors go to the log.
 */
function mutate(cmd) {
  send(cmd).then(() => refresh()).catch((e) => log("ERR " + e.message));
}

/**
 * Shades step: the primary list of configured shades. One row per shade with an
 * editable name, an On switch (exposure over Thread), motor controls, and a
 * Remove button. Rows switched off are greyed. Address and rolling code are not
 * shown here — they live in the Advanced (backup/restore) table below.
 */
function renderManage() {
  const tb = $("manageBody");
  const adv = $("advBody");
  tb.textContent = "";
  adv.textContent = "";
  $("shadeTable").hidden = shades.length === 0;
  $("shadeEmpty").hidden = shades.length !== 0;
  for (const s of shades) {
    const tr = document.createElement("tr");
    tr.classList.toggle("disabled", !s.on);
    tr.append(inputTd("name", s.name, (v) => save(`name ${s.idx} ${v}`)));
    tr.append(switchTd(s.on, (on) => save(`on ${s.idx} ${on ? 1 : 0}`)));
    tr.append(motorTd(s.idx));
    const rm = document.createElement("td");
    rm.append(mkBtn(t("shades.remove"), () => {
      if (confirm(t("confirm.remove", { name: s.name || s.idx }))) mutate(`remove ${s.idx}`);
    }, "danger small"));
    tr.append(rm);
    tb.append(tr);

    const ar = document.createElement("tr");
    ar.append(td(s.name || String(s.idx)));
    ar.append(inputTd("num", s.addr, (v) => save(`addr ${s.idx} ${v}`)));
    ar.append(inputTd("num", String(s.rolling), (v) => save(`roll ${s.idx} ${v}`)));
    adv.append(ar);
  }
}

/** Motor cell: Somfy RF PROG + movement controls for one shade. */
function motorTd(idx) {
  const cell = document.createElement("td");
  const acts = document.createElement("div");
  acts.className = "ctrls";
  acts.append(mkBtn(t("motor.prog"), () => send(`tx ${idx} prog`), "primary small"));
  acts.append(mkBtn(t("motor.open"), () => send(`tx ${idx} up`), "small"));
  acts.append(mkBtn(t("motor.close"), () => send(`tx ${idx} down`), "small"));
  acts.append(mkBtn(t("motor.my"), () => send(`tx ${idx} my`), "small"));
  acts.append(mkBtn(t("motor.stop"), () => send(`tx ${idx} stop`), "small"));
  cell.append(acts);
  return cell;
}

function td(text) { const el = document.createElement("td"); el.textContent = text; return el; }

function inputTd(cls, val, onCommit) {
  const cell = document.createElement("td");
  const inp = document.createElement("input");
  inp.type = "text";
  inp.className = cls;
  inp.value = val;
  inp.addEventListener("change", () => onCommit(inp.value.trim()));
  cell.append(inp);
  return cell;
}

/** Cell holding a labelled toggle switch (a styled checkbox) for the On state. */
function switchTd(on, onToggle) {
  const cell = document.createElement("td");
  const lab = document.createElement("label");
  lab.className = "switch";
  const inp = document.createElement("input");
  inp.type = "checkbox";
  inp.checked = on;
  inp.addEventListener("change", () => onToggle(inp.checked));
  const slider = document.createElement("span");
  slider.className = "slider";
  lab.append(inp, slider);
  cell.append(lab);
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

/** Add a motor without a remote: the firmware invents an address; then PROG it. */
function addMotor() {
  mutate(`add`);
}

/* ── actions ──────────────────────────────────────────────────────────── */

/** Open the serial port, start reading, then fingerprint the firmware. */
async function connect() {
  if (!("serial" in navigator)) { alert(t("alert.webserial")); return; }
  port = await navigator.serial.requestPort();
  await port.open({ baudRate: 115200 });
  writer = port.writable.getWriter();
  readLoop();
  connected = true;
  $("dot").classList.add("on");
  $("statusText").textContent = t("status.connected");
  $("connect").disabled = true;
  $("disconnBanner").hidden = true;
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
  scanning = false;
  try { if (writer) writer.releaseLock(); } catch (e) { /* already released */ }
  try { if (port) port.close(); } catch (e) { /* already closing */ }
  writer = null; port = null;
  $("dot").classList.remove("on");
  $("statusText").textContent = t("status.disconnected");
  $("connect").disabled = false;
  $("connect").textContent = t("board.recheck");
  $("discoverPanel").hidden = true;
  $("disconnBanner").hidden = false;
  gateNext();
}

/**
 * Release the serial port so ESP Web Tools can claim it for flashing, and reset
 * the connection UI. readLoop's pending pipeTo settles via its own catch.
 */
async function releasePort() {
  try { if (writer) writer.releaseLock(); } catch (e) { /* already released */ }
  try { if (port) await port.close(); } catch (e) { /* already closing */ }
  writer = null; port = null; connected = false;
  $("dot").classList.remove("on");
  $("statusText").textContent = t("status.disconnected");
  $("connect").disabled = false;
  $("connect").textContent = t("board.recheck");
  $("next").disabled = true;
}

/**
 * Ask the board for its firmware id/version and branch the wizard: up to date →
 * offer Continue; older than the latest release → offer Update or Continue;
 * unrecognized/no reply → reveal the flasher. Compare by tag equality only.
 */
async function detect() {
  const det = $("detect");
  det.hidden = false;
  det.textContent = t("detect.checking");
  let ver = null;
  try {
    const line = await request("version", (l) => l.startsWith("somfy-thread "), 2500);
    ver = line.slice("somfy-thread ".length).trim();
  } catch (e) { /* not a somfy-thread board (or blank) */ }

  if (!ver) {
    det.textContent = t("detect.none");
    await beginFlash();
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
  det.append(mkBtn(t("detect.continue"), () => setStep(stepIn(1)), "primary small"));
  if (outdated) det.append(mkBtn(t("detect.update"), () => beginFlash(), "small"));
  $("next").disabled = false;
}

/** Close the serial port and reveal the release picker + install button. */
async function beginFlash() {
  await releasePort();
  populateReleaseSelect();
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
 * frequency, then re-create each shade with `add` (address + rolling + name).
 * Shades come back exposed; any that were off in the backup are switched off
 * afterwards. Identity is by radio address, so the re-assigned slot index does
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
    if (m && s.on === false) await send(`on ${m[1]} 0`);
  }
  await refresh();
}

/** Open the commissioning window and show the Matter code + QR payload. */
async function getPairing() {
  const manual = await request("pair", (l) => /^\d{11,}$/.test(l.replace(/-/g, "")));
  let qr = "";
  try { qr = await request("qr", (l) => l.startsWith("MT:")); } catch (e) { /* payload optional */ }
  const code = $("paircode");
  code.hidden = false;
  code.textContent = manual;
  const img = $("qrimg");
  if (qr && window.qrcode) {
    const q = window.qrcode(0, "M");
    q.addData(qr);
    q.make();
    img.src = q.createDataURL(6, 16);
    img.hidden = false;
  } else {
    img.hidden = true;
  }
  $("qrpayload").textContent = qr ? t("matter.qr", { qr }) : "";
}

/* ── wiring ───────────────────────────────────────────────────────────── */

$("ack").addEventListener("change", (e) => { $("connect").disabled = !e.target.checked; });
$("connect").addEventListener("click", () => connect().catch((e) => log("ERR " + e.message)));
$("refresh").addEventListener("click", () => refresh().catch((e) => log("ERR " + e.message)));
$("discover").addEventListener("click", () => startDiscover().catch((e) => log("ERR " + e.message)));
$("discoverDone").addEventListener("click", () => stopDiscover());
$("addMotor").addEventListener("click", () => addMotor());
$("reconnect").addEventListener("click", () => connect().catch((e) => log("ERR " + e.message)));
if ("serial" in navigator)
  navigator.serial.addEventListener("disconnect", (e) => { if (e.target === port) onDisconnect(); });
$("refreshReleases").title = t("flasher.refresh");
$("refreshReleases").setAttribute("aria-label", t("flasher.refresh"));
$("refreshReleases").addEventListener("click", () =>
  fetchReleases().then(populateReleaseSelect).catch((e) => log("ERR " + e.message)));
$("next").addEventListener("click", () => setStep(stepIn(1)));
$("back").addEventListener("click", () => setStep(stepIn(-1)));
$("export").addEventListener("click", () => exportBackup().catch((e) => log("ERR " + e.message)));
$("import").addEventListener("click", () => $("importFile").click());
$("importFile").addEventListener("change", (e) => {
  if (e.target.files[0]) importBackup(e.target.files[0]).catch((err) => log("ERR " + err.message));
});
$("radioFreq").addEventListener("change", (e) => save(`freq ${e.target.value}`));
$("radioCheck").addEventListener("click", () => loadRadio().catch((e) => log("ERR " + e.message)));
$("radioListen").addEventListener("click", () => scanAndListen());
$("pairBtn").addEventListener("click", () => getPairing().catch((e) => log("ERR " + e.message)));
$("resetBtn").addEventListener("click", () => {
  if (confirm(t("confirm.reset"))) send("reset");
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

loadIcons();
applyI18n();
fetchReleases();
setStep(0);
