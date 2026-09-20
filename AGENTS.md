# AGENTS.md

Read [CODING_STANDARDS.md](CODING_STANDARDS.md) first — it is the source of truth
for how to work in this repository (architecture boundary, module graph, comment
style, radio/Matter evidence rules, web-site rules, workflow, and how to verify on
real hardware).

## Quick reference

- Firmware build (pinned toolchain):
  `docker run --rm -v "$PWD":/project espressif/esp-matter:release-v1.4_idf_v5.4.1 bash -lc 'source $IDF_PATH/export.sh >/dev/null 2>&1 && source $ESP_MATTER_PATH/export.sh >/dev/null 2>&1 && cd /project && idf.py build'`
  (the `cd /project` is required — `export.sh` changes the working directory).
- Flash from the host (Docker cannot reach `/dev/cu.*`), run from `build/`:
  `esptool --chip esp32c6 -p /dev/cu.usbmodemXXXX -b 460800 write_flash --flash_mode dio --flash_size 4MB --flash_freq 80m 0x0 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin 0x10000 ota_data_initial.bin 0x20000 somfy_thread.bin`
- Host self-tests:
  `cc -Imain test/test_somfy_frame.c main/somfy_frame.c -o /tmp/t && /tmp/t`
  `cc -Imain -Itest/stubs test/test_blind_store.c main/blind_store.c -o /tmp/t && /tmp/t`

## Hard rules (see CODING_STANDARDS.md for the rest)

- Comments: JSDoc-style, above the function, ground truth. No inline comments.
- Reuse before abstraction; `board.h` is the only place pin numbers live.
- Firmware and the web site meet only at the serial console contract.
- Never commit real remote addresses, rolling codes, or backups.
- Commit messages: no backticks (use `git commit -F`), no attribution trailers.
- Ask before on-air RF tests; give a listen window of at least 20 seconds.
