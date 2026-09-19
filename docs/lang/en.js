// SPDX-License-Identifier: Unlicense
// English strings. Copy this file to lang/<code>.js and translate the values to
// add a language, then register it in i18n.js. English is the fallback.

export default {
  "app.tag": "Setup wizard",
  "step.board": "Board",
  "step.shades": "Shades",
  "step.matter": "Matter",
  "step.backup": "Backup",

  "board.title": "Connect your board",
  "board.intro": "Somfy RTS blinds over Matter-over-Thread on an ESP32-C6 + CC1101. Use Chrome or Edge on desktop, plug the board into USB, then connect — we'll check what's on it.",
  "board.connect": "Connect board",
  "compat.warn": "This controls Somfy RTS blinds only (433 MHz). It does not support Somfy io-homecontrol — check your motors are RTS before continuing.",
  "compat.ack": "My blinds use Somfy RTS.",
  "board.recheck": "Re-check board",
  "board.portHint": "In the browser's port list, pick <strong>USB JTAG/serial debug unit</strong> — that's the ESP32-C6's native USB. On boards with a USB-to-UART bridge it shows instead as <strong>CP210x</strong>, <strong>CP2104</strong>, or <strong>CH340</strong>. Ignore Bluetooth and audio entries. Unsure which entry it is? Click Connect once with the board unplugged to see the list, cancel, plug the board in, then click again — the new entry is your board.",
  "status.disconnected": "Disconnected",
  "status.connected": "Connected",

  "flasher.versionLabel": "Firmware version",
  "flasher.loading": "loading releases…",
  "flasher.flash": "Flash to device",
  "flasher.unsupported": "This browser can't flash over USB — use Chrome or Edge, or download the .bin below.",
  "flasher.notAllowed": "Flashing needs a secure (https) page.",
  "flasher.releaseNotes": "Release notes",

  "detect.checking": "Checking the board…",
  "detect.none": "No somfy-thread firmware detected. Pick a version and flash it.",
  "detect.outdated": "somfy-thread {ver} installed — {latest} available. ",
  "detect.current": "somfy-thread {ver} — up to date. ",
  "detect.continue": "Continue to config",
  "detect.update": "Update firmware",

  "release.none": "no releases published yet",
  "release.prerelease": " (pre-release)",
  "release.noBin": "This release has no .bin firmware asset.",
  "release.download": "Download {name}",
  "release.fallbackPrefix": "Firefox/Safari: ",
  "release.fallbackSuffix": " and flash at offset 0 with esptool.",

  "shades.title": "Your shades",
  "shades.intro": "Each shade is one roller blind driven by one Somfy motor. Name it, program its motor (PROG), and test movement — all here. Address and rolling code are only needed when restoring a backup.",
  "shades.progHelp": "To program a motor: press and hold the <strong>PROG</strong> button on the motor's existing remote until it jogs, then click <strong>PROG</strong> in that row within a few seconds. The motor jogs again to confirm. This Somfy RF link is separate from Matter (next step).",
  "shades.col.name": "Name",
  "shades.col.freq": "Freq (MHz)",
  "shades.col.address": "Address",
  "shades.col.rolling": "Rolling",
  "shades.col.on": "On",
  "shades.col.motor": "Motor",
  "shades.reload": "Reload from device",
  "motor.prog": "PROG",
  "motor.open": "Open",
  "motor.close": "Close",
  "motor.my": "My",
  "motor.stop": "Stop",

  "matter.title": "Add to your smart home",
  "matter.intro": "This is a standard Matter-over-Thread device. Get the code below and add it in whatever app or hub you use — the controller doesn't matter.",
  "matter.help": "Commissioning happens over Bluetooth, then the device joins your Thread network — so you need a Thread border router reachable on that network (built into many hubs/speakers; any brand works). Keep the board near the phone while pairing. This is separate from the Somfy RF PROG you did per shade.",
  "matter.getCode": "Get pairing code",
  "matter.qr": "QR payload: {qr}",

  "backup.title": "Back up your setup",
  "backup.intro": "Save a JSON backup (addresses + rolling codes) to your disk. Import it later to restore, or to migrate to a new device.",
  "backup.download": "Download backup",
  "backup.import": "Import backup…",
  "backup.done": "You're all set. Reopen this page any time to add shades, re-pair, or restore.",
  "backup.reset": "Factory-reset the device",

  "nav.back": "Back",
  "nav.next": "Next",
  "log.title": "Serial log",

  "alert.webserial": "Web Serial needs Chrome or Edge on desktop.",
  "confirm.reset": "Factory-reset Matter and reboot the device?",
};
