---
name: release
description: Cut a new somfy-thread firmware release — create the GitHub release, let CI build and attach the .bin, mirror it, and verify. Use when the user asks to release, publish a new firmware version, or bump the version.
allowed-tools: Bash, Read, Write
---

# Release a new firmware version

The firmware version **is** the git tag: `CMakeLists.txt` leaves `PROJECT_VER`
unset, `firmware.yml` writes the release tag into `version.txt`, and ESP-IDF
reports it via `esp_app_get_description()->version`. The web configurator
compares that to the latest GitHub release tag, so tag and firmware version stay
in lockstep. Never re-add `set(PROJECT_VER …)` to `CMakeLists.txt` — it overrides
`version.txt` and reintroduces the "installed X, vY available" false-positive.

Flow: **you publish the release, CI attaches the bin.** Publishing a release fires
`firmware.yml` (build → attest → `curl`-attach the merged bin to the release).
Its successful completion fires `pages.yml`, which mirrors the bin to
`web/fw/<tag>/` so the flasher serves it same-origin. `gh` is NOT installed in
the esp-matter build container, so CI attaches via `curl` to the release
`upload_url` — don't add `gh` steps to the container job.

## Steps

1. **Preconditions.** `main` is pushed and CI-green; any `web/` change is deployed
   (pages.yml runs on push to `main`). **Release immutability must be OFF**
   (Settings → General) — with it on, attaching the bin after publish fails with
   HTTP 422 and tag names get permanently reserved. See gotchas.

2. **Pick the next version.** Bump `vMAJOR.MINOR.PATCH`. Check existing:
   `gh release list`.

3. **Write end-user release notes** to a temp file: what the **firmware** does,
   not the web UI or dev internals. Concise, formatted, one line per bullet. May
   name ecosystems (Apple Home, Google Home, Alexa, Home Assistant). If firmware
   behaviour is unchanged from the last tag, say so — check with
   `git log --oneline <lastTag>..HEAD -- main/ CMakeLists.txt`.

4. **Create and publish the release** (this creates the tag on `main` HEAD, which
   must contain the current `firmware.yml`):
   ```sh
   gh release create vX.Y.Z --target main --title vX.Y.Z --notes-file /tmp/relnotes.md
   ```

5. **Wait for `firmware.yml`** to build and attach the bin (~10-15 min):
   ```sh
   gh run list --workflow firmware.yml --limit 1 --json status,conclusion
   gh release view vX.Y.Z --json assets -q '[.assets[].name]'   # expect one .bin
   ```

6. **Wait for Pages.** The successful firmware workflow triggers `pages.yml`,
   which mirrors the attached binary. Verify the result:
   ```sh
   gh run list --workflow pages.yml --limit 1 --json status,conclusion
   curl -fsSI "https://lenoxys.github.io/somfy-thread/fw/vX.Y.Z/somfy-thread-esp32c6-vX.Y.Z.bin" | head -1
   ```

7. **Verify** in the configurator (hard-refresh first): the board reports `vX.Y.Z`
   and detect shows "up to date"; flashing fetches the mirrored bin.

## Gotchas

- **Release immutability breaks this flow.** If it's ON: (a) CI's post-publish
  attach fails with **422** (assets frozen at publish), and (b) a published tag
  name is **reserved forever** — deleting the release doesn't free it, and
  re-pushing that tag is rejected with "repository rule violations". Keep it OFF,
  or the release must be built as a draft with the asset attached before publish
  (needs `gh` in the container or a curl-based draft flow — not the current setup).
- **Botched release recovery:** `gh release delete <tag> --cleanup-tag --yes`, then
  re-cut. If immutability was ever on for that tag, bump to the next number.
- **Manual build (no release):** `workflow_dispatch` on `firmware.yml` builds +
  attests only (attaches nothing). Local build: see `AGENTS.md`.

See `.github/workflows/firmware.yml` and `.github/workflows/pages.yml`, and the
`web-flasher-hosting` memory for flasher-side detail.
