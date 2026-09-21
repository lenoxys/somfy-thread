---
name: local-build
description: Build the somfy-thread firmware .bin locally in the esp-matter Docker image (no GitHub release / CI). Use to test-compile firmware changes, or to produce a flashable dev bin, on this machine. Not for cutting a release — that is the release skill.
allowed-tools: Bash, Read
---

# Build the firmware .bin locally

Produces `somfy-thread-esp32c6-dev.bin` at the repo root by running `idf.py
build` inside the pinned esp-matter Docker image, then merging the artifacts
with `esptool merge_bin`. Use this to verify firmware compiles or to flash a
dev bin — **not** to publish (that is the `release` skill, which builds in CI
and attaches the bin to a GitHub release; the local bin reports version `dev`,
not a tag).

## Prerequisites (all cached — do NOT recreate)

- **Colima/Docker daemon running.** Socket: `unix:///Users/adelamarre/.colima/default/docker.sock`.
  If `docker ps` errors, `colima status`; if it reports "empty value" or an
  unreachable daemon, `colima stop -f && colima start` (a plain `colima start`
  lies "already running" while the daemon is dead).
- **The image is already pulled** (~22.6 GB): `espressif/esp-matter:release-v1.4_idf_v5.4.1`.
  It is **x86_64**, so on this Apple-Silicon Mac it runs under emulation — a
  **clean build is SLOW (~30-40 min)**. Never re-pull it to "refresh".
- **The Bash tool is sandboxed** and cannot reach the Docker daemon. Every
  `docker` / `colima` call needs `dangerouslyDisableSandbox: true`.

## Keep the cache — the build is expensive

The slow part is compiling ~1700 objects under emulation. **Preserve the build
cache between runs** so only changed files recompile (minutes, not half an hour):

- **Keep `/work/build`** — do NOT `rm -rf build` before a build. Ninja does
  correct incremental rebuilds; a stale `build/` was a **red herring** in an
  earlier failure (the real cause was the `cd /work` gotcha below, not the
  cache). Only wipe `build/` when changing target, editing `sdkconfig`/partition
  layout, or if a build is genuinely corrupt.
- **Keep `managed_components/` + `dependencies.lock`** — re-resolving the
  component manager is wasted time.
- **Keep the Docker image and the container's toolchain.** `--rm` only discards
  the ephemeral container, not the image layers or the bind-mounted `/work`
  cache.

## The build command

Run it in the **background** (`run_in_background: true`) with
`dangerouslyDisableSandbox: true`, and tee to a log so you can watch progress:

```sh
docker run --rm -v "$PWD":/work -w /work \
  espressif/esp-matter:release-v1.4_idf_v5.4.1 bash -c '
    set -e
    git config --global --add safe.directory /work
    . "$IDF_PATH/export.sh"
    . "$ESP_MATTER_PATH/export.sh"
    cd /work
    idf.py set-target esp32c6
    idf.py build
    cd /work/build
    esptool.py --chip esp32c6 merge_bin -o /work/somfy-thread-esp32c6-dev.bin @flash_args
    echo "=== BUILD DONE: $(ls -la /work/somfy-thread-esp32c6-dev.bin) ==="
  ' > /tmp/somfy_build.log 2>&1
```

### CRITICAL gotcha — `cd /work` AFTER the exports

`. "$ESP_MATTER_PATH/export.sh"` **changes the shell CWD** to
`/opt/espressif/esp-matter`. If you run `idf.py` without `cd /work` first, it
builds the **esp-matter repo itself** (fails at its `CMakeLists.txt:196
idf_build_get_property`). Always `cd /work` after sourcing both `export.sh`
scripts, before `set-target`/`build`.

`git config --global --add safe.directory /work` silences the dubious-ownership
warning from the bind mount.

## Watch / verify

```sh
tail -3 /tmp/somfy_build.log
grep -iE "error:|BUILD DONE" /tmp/somfy_build.log | tail
```

- Progress prints `[N/1700] Building …`. A benign `aligned_storage`
  deprecation `note:` from the RISC-V system headers is expected — not an error.
- Success = a `=== BUILD DONE: … somfy-thread-esp32c6-dev.bin ===` line and the
  bin present at the repo root.

## Flash it

- Web flasher (ESP Web Tools) served from `web/`, or
- `esptool.py --chip esp32c6 -p /dev/cu.usbmodem14101 write_flash 0x0 somfy-thread-esp32c6-dev.bin`
  (`/dev/cu.usbmodem14101` = the C6's native USB; run esptool on the host, not in
  the sandbox). Flashing and serial queries are fine anytime; a Somfy RF
  **transmit** test needs asking first + a ≥20 s window.
