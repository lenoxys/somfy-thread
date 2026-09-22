---
name: local-build
description: Build and flash a local somfy-thread development firmware with the cached esp-matter toolchain. Use for firmware compile checks and bench-board updates, not releases.
allowed-tools: Bash, Read
---

# Local firmware build

Build with the existing `espressif/esp-matter:release-v1.6_idf_v6.0.2` image and
bind-mounted incremental `build/` tree. The x86_64 image is about 22.6 GB and a
clean build takes 30-40 minutes under Apple-Silicon emulation.

## Preserve the cache

- Keep `build/`, `managed_components/`, `dependencies.lock`, and the Docker image.
- Run `idf.py set-target esp32c6` only when the target changes; it regenerates the
  build tree.
- If Docker is unavailable, check `colima status`; recover a stale daemon with
  `colima stop -f && colima start`.

## Build

Run from the repository root:

```sh
docker run --rm -v "$PWD":/work -w /work \
  espressif/esp-matter:release-v1.6_idf_v6.0.2 bash -c '
    set -e
    git config --global --add safe.directory /work
    . "$IDF_PATH/export.sh"
    . "$ESP_MATTER_PATH/export.sh"
    cd /work
    if [ ! -f build/CMakeCache.txt ]; then idf.py set-target esp32c6; fi
    idf.py build
    cd /work/build
    esptool.py --chip esp32c6 merge_bin -o /work/somfy-thread-esp32c6-dev.bin @flash_args
    echo "=== BUILD DONE: $(ls -la /work/somfy-thread-esp32c6-dev.bin) ==="
  ' > /tmp/somfy_build.log 2>&1
```

`ESP_MATTER_PATH/export.sh` changes the working directory, so `cd /work` after
both exports is required. Success is the `BUILD DONE` line in
`/tmp/somfy_build.log` and a root-level development binary.

## Flash

Use host `esptool` through `.agents/skills/local-build/scripts/flash.sh`. The
port defaults to `/dev/cu.usbmodem14101` and can be overridden with `SOMFY_PORT`.

```sh
.agents/skills/local-build/scripts/flash.sh app
.agents/skills/local-build/scripts/flash.sh wipe
```

`app` updates `ota_0` and `otadata` while preserving NVS and factory data.
`wipe` writes the merged image at `0x0`; its padding erases NVS, including shades,
links, positions, and radio configuration. Back up first with the web
configurator or:

```sh
.agents/skills/local-build/scripts/somfy.py backup
```

Flashing and read-only serial queries do not transmit Somfy RF. Ask before an RF
transmit test and provide at least a 20-second observation window. Only one
process can hold the native USB port at a time.
