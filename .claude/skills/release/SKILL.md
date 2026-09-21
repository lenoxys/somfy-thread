---
name: release
description: Cut a new somfy-thread firmware release — tag, build, publish the .bin, and verify. Use when the user asks to release, publish a new firmware version, or bump the version.
allowed-tools: Bash, Read, Write
---

# Release a new firmware version

The firmware version **is** the git tag: `CMakeLists.txt` leaves `PROJECT_VER`
unset, `firmware.yml` writes the tag into `version.txt`, and ESP-IDF reports it
via `esp_app_get_description()->version`. The web configurator compares that to
the latest GitHub release tag, so tag and firmware version stay in lockstep.

Releases are **immutable** (repo setting: assets and tags frozen once published).
The whole pipeline is built around that — read the gotchas before deviating.

## Steps

1. **Preconditions.** `main` is pushed and CI-green. Any `web/` change is already
   deployed (pages.yml runs on push to `main`).

2. **Pick the next version.** Bump `vMAJOR.MINOR.PATCH`. **Never reuse a tag name
   that was ever published** — immutability reserves it permanently, and deleting
   the release does NOT free it (`git push` is rejected with "repository rule
   violations"). Check existing tags: `gh release list`.

3. **Write end-user release notes** to a temp file. Describe what the **firmware**
   does, not the web UI or dev internals. Concise, formatted, one line per bullet.
   Release notes MAY name ecosystems (Apple Home, Google Home, Alexa, Home
   Assistant). If firmware behaviour is unchanged from the last tag, say so
   plainly (check with `git log --oneline <lastTag>..HEAD -- main/ CMakeLists.txt`).

4. **Create the annotated tag with the notes as its message, and push it:**
   ```sh
   git tag -a vX.Y.Z -F /tmp/relnotes.md
   git push origin vX.Y.Z
   ```
   The tag must sit on a commit that contains the current `firmware.yml` — a
   tag-triggered run uses the workflow from the tagged commit.

5. **CI does the rest** (`firmware.yml`, on tag push): builds → attests provenance
   → creates a **draft** release with the merged `.bin` attached → publishes it
   last. Publishing fires `pages.yml`, which mirrors the asset to `web/fw/<tag>/`
   so the flasher serves it same-origin. **Do not edit the release while CI runs.**

6. **Verify** once the run completes:
   ```sh
   gh run list --workflow firmware.yml --limit 1 --json status,conclusion
   gh release view vX.Y.Z --json isDraft,assets      # isDraft=false, one .bin asset
   curl -fsSI "https://lenoxys.github.io/somfy-thread/fw/vX.Y.Z/somfy-thread-esp32c6-vX.Y.Z.bin" | head -1
   ```
   Then in the configurator (hard-refresh first): the board reports `vX.Y.Z` and
   detect shows "up to date".

## Gotchas

- **Immutability = attach before publish.** Attaching an asset to an
  already-published release fails with **HTTP 422**. The CI flow attaches to a
  draft, then publishes — never add `--after`-style attach steps.
- **Tag names are burned on publish.** A botched release can't be re-cut under the
  same tag. Recover by `gh release delete <tag> --cleanup-tag --yes`, then bump to
  the **next** number.
- **Version source of truth is the tag.** No hardcoded version anywhere; don't
  re-add `set(PROJECT_VER …)` to `CMakeLists.txt` — it would override `version.txt`
  and reintroduce the "installed X, vY available" false-positive.
- **Manual build (no release):** `workflow_dispatch` on `firmware.yml` builds +
  attests only (publishes nothing). Local build: see `AGENTS.md`.

See `.github/workflows/firmware.yml` and `.github/workflows/pages.yml` for the
implementation, and the `web-flasher-hosting` memory for flasher-side detail.
