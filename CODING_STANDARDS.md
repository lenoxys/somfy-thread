<!-- SPDX-License-Identifier: Unlicense -->
# Coding standards

How to work in `somfy-thread`: a hard architecture boundary, reuse before
abstraction, ground-truth comments, and evidence over guessing — for an ESP32-C6
firmware plus a static Web Serial configuration site.

## Stack and gate

Firmware is C and C++ on ESP-IDF + esp-matter, target `esp32c6`, built with the
`espressif/esp-matter:release-v1.6_idf_v6.0.2` toolchain (Docker image or a local
IDF of the same version).
The configuration site under `web/` is a static GitHub Pages app: vanilla
ES modules, no framework, no build step, vendored dependencies (`web/vendor/`),
external CSS, strict CSP, and Web Serial as the only transport.
Pure frame logic (`main/somfy_frame.c`) carries host self-tests that compile with
a plain `cc` and no ESP dependencies.

The gate before any change is done:
- `idf.py build` is clean (via the pinned toolchain image).
- The host self-tests pass:
  `cc -Imain test/test_somfy_frame.c main/somfy_frame.c -o /tmp/t && /tmp/t`
  and `cc -Imain -Itest/stubs test/test_blind_store.c main/blind_store.c -o /tmp/t && /tmp/t`.
- Anything touching runtime behaviour is confirmed on real hardware
  (build → flash → observe), not asserted from the source alone.

## Quick reference

- Firmware build (pinned toolchain):
  `docker run --rm -v "$PWD":/project espressif/esp-matter:release-v1.6_idf_v6.0.2 bash -lc 'source $IDF_PATH/export.sh >/dev/null 2>&1 && source $ESP_MATTER_PATH/export.sh >/dev/null 2>&1 && cd /project && idf.py build'`
  (the `cd /project` is required — `export.sh` changes the working directory).
- Flash from the host (Docker cannot reach `/dev/cu.*`), run from `build/`:
  `esptool --chip esp32c6 -p /dev/cu.usbmodemXXXX -b 460800 write_flash --flash_mode dio --flash_size 4MB --flash_freq 80m 0x0 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin 0x10000 ota_data_initial.bin 0x20000 somfy_thread.bin`
- Host self-tests:
  `cc -Imain test/test_somfy_frame.c main/somfy_frame.c -o /tmp/t && /tmp/t`
  `cc -Imain -Itest/stubs test/test_blind_store.c main/blind_store.c -o /tmp/t && /tmp/t`

## Architecture boundary

The firmware owns radio truth and device state: the CC1101 driver, RTS frame
build/decode, TX and the RX edge-decoder, the persisted shade store, the Matter
endpoints, and the Thread stack.
The web site owns presentation and workflow: the connect/update/config wizard,
Web Serial I/O, i18n, and rendering.
The two meet at exactly one contract — the serial console command set. The site
never infers internal firmware state; it asks over the contract. A behaviour the
site needs but the contract does not expose is a gap in the contract, to be added
deliberately on the firmware side, never guessed at in the browser.

The contract is versioned. `version` reports a `proto` number
(`SOMFY_PROTO` in `main/app_main.cpp`); the site targets a `REQUIRED_PROTO`
(`web/app.js`) and refuses to configure a board reporting a lower one, prompting
an update instead. Any change to the command set — adding, removing, or altering
a command's inputs or outputs — bumps both numbers together.

`board.h` is the single source of pin truth. No GPIO number is written anywhere
else; everything refers to the `CC1101_PIN_*` / `BOARD_*` macros.

## Module graph

`main/` is a small, layered graph — keep it that way:
- `somfy_frame.{c,h}` — pure RTS logic (build, decode, pulse state machine). No
  ESP dependencies, so it stays host-testable. This is an invariant: do not pull
  `esp_*` headers into it.
- `cc1101.{c,h}` — the SPI/OOK radio driver. Owns register truth for the E07
  module; nothing else pokes registers except the `reg` calibration command.
- `somfy_rts.{c,h}` — TX: renders a frame to the RMT symbol stream and keys OOK.
- `somfy_rx.{c,h}` — RX: the GPIO edge-interrupt front-end that feeds
  `somfy_decode_pulses`.
- `blind_store.{c,h}` — the shade records and their NVS persistence.
- `app_main.cpp` — orchestration only: Matter node/endpoints, the console
  contract, and RF dispatch. It wires the modules; it does not reimplement them.

Interfaces live beside their owner. No generic `util`/`common` bucket, no
speculative indirection, no import cycles.

## Reuse before abstraction

Search the tree before adding anything. A helper, constant, or pattern that
already exists is reused, not re-created. Add an abstraction only after a second
concrete caller exists and the tests are green — never for a single anticipated
one. The frame decoder is the model: one pure function serves TX round-trip
tests, the RX task, and the host self-test.

## Comments

Block comments in the JSDoc style sit **above** the function, type, or register
table they document and state ground truth — what the code does and why, with the
measured or referenced fact behind a non-obvious choice.
No inline comments. No narration of the obvious, no speculative or "may later"
comments, no change-log prose in comments.
When a comment states a physical fact (a timing window, a register value, a
measured noise rate), it names the evidence.

## Radio and Matter facts

Claims about the radio are grounded in the ESPSomfy-RTS / Nickduino reference and
in on-air measurement, the way the test files cite `Somfy.cpp` line ranges.
Claims about esp-matter are grounded in the actual API of the pinned version
(`esp_matter_core.{h,cpp}`), not assumed. When behaviour depends on a hardware
limit (RMT channels, glitch-filter ceiling, dynamic-endpoint count), the limit is
verified in the toolchain, not guessed.
Physical values that a minimal model cannot see — symbol timings, sync windows,
AGC gain, glitch floors — stay as named, tunable constants with the calibration
note beside them.

## Web site rules

Strict CSP; no inline script or style, no CDN — every dependency is vendored
under `web/vendor/` and referenced locally.
CSS lives in external files. User-facing strings go through the i18n tables in
`web/lang/`, never hard-coded in logic.
The site degrades honestly: browsers without Web Serial are told to flash the
`.bin` with `esptool` instead of being left in a broken state.

## Secrets and real data

Never commit real device or account data. ESPSomfy backup files carry real remote
addresses and rolling codes; they are inputs to import, never checked in.
Provisioning material (Matter QR / manual pairing codes) is device output, not
repository content.

## Workflow

Conventional-style commit subjects. Commit messages contain no backticks (use
`git commit -F <file>`), and no automatic `Co-authored-by` / attribution trailers.
On-air RF tests (listening for a decoded remote, driving a motor) are gated:
ask first, wait for confirmation, and give a listen window of at least 20 seconds.

## Verifying on real hardware

Build then flash, in that order. The `esp-matter` Docker image builds but cannot
reach `/dev/cu.*`, so flashing runs from the host with `esptool`.
The toolchain's `export.sh` changes the working directory, so a build inside the
container must `cd` back to the project before `idf.py build`.
Prefer observations the host cannot fake: a decoded remote frame on air, an
endpoint that appears live in a Matter controller, a rolling code that advances in
NVS across a reboot.
