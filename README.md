# somfy-thread

Control Somfy RTS blinds over **Matter-over-Thread** with a **Waveshare ESP32-C6-Zero** and an **E07-M1101D (CC1101, 433 MHz)** radio. No MQTT, no on-device web server, no cloud — it's a standard Matter device, so it works with any Matter controller and Thread border router (Apple Home, Google Home, SmartThings, Home Assistant…) and nothing else.

Firmware is **generic**: it exposes eight WindowCovering endpoints, each mapped to a Somfy RTS remote address. You pair (PROG) your own motors onto it. All configuration, PROG pairing, and backup/restore happen from a **static browser page over Web Serial** — there is no code to write and nothing is sent anywhere.

## Hardware

Waveshare ESP32-C6-Zero (native USB-C) wired to an E07-M1101D:

| CC1101 (E07) | ESP32-C6 GPIO |
|--------------|---------------|
| SCK          | 19            |
| MISO         | 20            |
| MOSI         | 18            |
| CSN          | 21            |
| GDO0         | 22            |

Default carrier 433.42 MHz; per-shade frequency is tunable (a real crystal drifts — sweep 433.36–433.44 if a motor stays silent).

## Set it up (no toolchain)

Open the project page in a Chromium-based browser (Chrome/Edge), plug the board into USB, and follow the wizard — everything runs in the browser and nothing is sent anywhere.

- **Board** — connect over Web Serial; the page reads the firmware version. If it's current you continue straight to config; if a newer release exists you can update; if no somfy-thread firmware is found it flashes one with [ESP Web Tools](https://esphome.github.io/esp-web-tools/). Firefox/Safari don't support Web Serial — download the `.bin` and flash with `esptool` instead.

Each released `firmware.bin` carries a build-provenance attestation, so you can confirm it was built by this repo's workflow: `gh attestation verify firmware.bin --repo lenoxys/somfy-thread`.
- **Shades** — each shade is one motor: name it, run **PROG** (then long-press PROG on the motor's existing remote to pair), and test with **Open / Close / My / Stop**. Address and rolling code are only needed when restoring a backup.
- **Matter** — get the pairing code and add the device to any Matter controller.
- **Backup** — **export** a JSON backup and **import** it back to restore or migrate.

Rolling codes are persisted to flash before every transmit, so a reboot never rewinds them.

## Build from source

Needs esp-idf and esp-matter (with submodules). With `IDF_PATH` and `ESP_MATTER_PATH` set:

```sh
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

The Somfy frame builder has a host self-test with no ESP dependencies:

```sh
cc -Imain test/test_somfy_frame.c main/somfy_frame.c -o /tmp/t && /tmp/t
```

## Scope & roadmap

Today this drives **Somfy RTS only** (433 MHz, OOK, rolling code in the clear) — which is why it can be reimplemented at all.

Making **RTS and io-homecontrol coexist** on the same device is a goal, not a promise. io is a different beast: 868/915 MHz (the 433 MHz CC1101 here physically can't reach it, so it needs a second radio), and bidirectional + AES-encrypted with a key-exchange pairing — there is no open io stack to build on, which is the real blocker. If that changes, the natural shape is a per-shade `protocol` tag: the WindowCovering endpoints and the whole config/backup flow stay identical, and only the RF dispatch branches on RTS vs io.

## Credits

This project stands on prior work:

- **[kgun2g/somfy-rts-remote-by-thread](https://github.com/kgun2g/somfy-rts-remote-by-thread)** — the Matter-over-Thread + CC1101 firmware this project's structure and CC1101 register base are informed by.
- **[Nickduino/Somfy_Remote](https://github.com/Nickduino/Somfy_Remote)** — the canonical Somfy RTS frame/protocol reference.
- **[ESPSomfy-RTS](https://github.com/rstrouse/ESPSomfy-RTS)** — RTS timing and behaviour reference.
- **[esp-matter](https://github.com/espressif/esp-matter)** and **[esp-idf](https://github.com/espressif/esp-idf)** — the Matter/Thread stack and SDK.
- **[ESP Web Tools](https://github.com/esphome/esp-web-tools)** — the in-browser flashing used by the install page.

## License

Released into the public domain under [The Unlicense](./LICENSE).
