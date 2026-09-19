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
  sel.addEventListener("change", () => selectRelease(Number(sel.value)));
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

/** Continuously read the port, split into lines, log and dispatch them. */
async function readLoop() {
  const dec = new TextDecoderStream();
  port.readable.pipeTo(dec.writable).catch(() => {});
  const reader = dec.readable.getReader();
  let buf = "";
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

/* ── wizard navigation ────────────────────────────────────────────────── */

const STEPS = 4;
const CONNECT_STEP = 0;
let step = 0;

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
  $("next").disabled = step === CONNECT_STEP && !connected;
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

/** Shades step: editable table of every shade. */
function renderManage() {
  const tb = $("manageBody");
  tb.textContent = "";
  for (const s of shades) {
    const tr = document.createElement("tr");
    tr.append(td(String(s.idx)));
    tr.append(inputTd("name", s.name, (v) => save(`name ${s.idx} ${v}`)));
    tr.append(inputTd("num", s.freq.toFixed(3), (v) => save(`freq ${s.idx} ${v}`)));
    tr.append(inputTd("num", s.addr, (v) => save(`addr ${s.idx} ${v}`)));
    tr.append(inputTd("num", String(s.rolling), (v) => save(`roll ${s.idx} ${v}`)));
    tr.append(checkTd(s.active, (on) => save(`active ${s.idx} ${on ? 1 : 0}`)));
    tr.append(motorTd(s.idx));
    tb.append(tr);
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

function checkTd(on, onToggle) {
  const cell = document.createElement("td");
  const inp = document.createElement("input");
  inp.type = "checkbox";
  inp.checked = on;
  inp.addEventListener("change", () => onToggle(inp.checked));
  cell.append(inp);
  return cell;
}

function mkBtn(label, onClick, cls = "") {
  const b = document.createElement("button");
  b.className = ("btn " + cls).trim();
  b.textContent = label;
  b.addEventListener("click", onClick);
  return b;
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
  await detect();
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
  const norm = (s) => (s || "").replace(/^v/, "");
  const latest = releases[0] ? releases[0].tag_name : null;
  const outdated = latest && !norm(ver).startsWith(norm(latest)) && !norm(latest).startsWith(norm(ver));
  det.textContent = "";
  det.append(outdated ? t("detect.outdated", { ver, latest }) : t("detect.current", { ver }));
  det.append(mkBtn(t("detect.continue"), () => setStep(step + 1), "primary small"));
  if (outdated) det.append(mkBtn(t("detect.update"), () => beginFlash(), "small"));
  $("next").disabled = false;
}

/** Close the serial port and reveal the release picker + install button. */
async function beginFlash() {
  await releasePort();
  populateReleaseSelect();
  $("flasher").hidden = false;
}

/** Download the current table as a JSON backup file. */
async function exportBackup() {
  const line = await request("export", (l) => l.startsWith("["));
  const blob = new Blob([line], { type: "application/json" });
  const a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  a.download = "somfy-thread-backup.json";
  a.click();
  URL.revokeObjectURL(a.href);
}

/** Replay a backup file into the device (name/addr/roll/freq/active per shade). */
async function importBackup(file) {
  const list = JSON.parse(await file.text());
  for (const s of list) {
    await send(`name ${s.idx} ${s.name}`);
    await send(`addr ${s.idx} ${s.addr}`);
    await send(`roll ${s.idx} ${s.rolling}`);
    await send(`freq ${s.idx} ${s.freq.toFixed(3)}`);
    await send(`active ${s.idx} ${s.active ? 1 : 0}`);
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
  $("qrpayload").textContent = qr ? t("matter.qr", { qr }) : "";
}

/* ── wiring ───────────────────────────────────────────────────────────── */

$("ack").addEventListener("change", (e) => { $("connect").disabled = !e.target.checked; });
$("connect").addEventListener("click", () => connect().catch((e) => log("ERR " + e.message)));
$("refresh").addEventListener("click", () => refresh().catch((e) => log("ERR " + e.message)));
$("next").addEventListener("click", () => setStep(step + 1));
$("back").addEventListener("click", () => setStep(step - 1));
$("export").addEventListener("click", () => exportBackup().catch((e) => log("ERR " + e.message)));
$("import").addEventListener("click", () => $("importFile").click());
$("importFile").addEventListener("change", (e) => {
  if (e.target.files[0]) importBackup(e.target.files[0]).catch((err) => log("ERR " + err.message));
});
$("pairBtn").addEventListener("click", () => getPairing().catch((e) => log("ERR " + e.message)));
$("resetBtn").addEventListener("click", () => {
  if (confirm(t("confirm.reset"))) send("reset");
});

/** Apply a saved theme: "system" clears the override so the OS decides. */
function applyTheme(v) {
  if (v === "system") document.documentElement.removeAttribute("data-theme");
  else document.documentElement.dataset.theme = v;
}
const savedTheme = localStorage.getItem("theme") || "system";
$("theme").value = savedTheme;
applyTheme(savedTheme);
$("theme").addEventListener("change", (e) => {
  localStorage.setItem("theme", e.target.value);
  applyTheme(e.target.value);
});

applyI18n();
fetchReleases();
setStep(0);
