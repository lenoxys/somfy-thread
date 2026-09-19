<!-- SPDX-License-Identifier: Unlicense -->
# Security policy

## Reporting a vulnerability

Please report security issues privately through GitHub's [Report a vulnerability](https://github.com/lenoxys/somfy-thread/security/advisories/new) form rather than opening a public issue.

Include what you found, how to reproduce it, and the impact you expect. This is a hobby project maintained on a best-effort basis — expect a first response within a couple of weeks.

## Scope

This is a headless device that pairs Somfy RTS motors over 433 MHz RF and commissions onto a Matter fabric over Thread. Worth reporting: anything that lets an unpaired party drive a motor, extract Matter credentials, or bypass the rolling-code anti-rewind. Somfy RTS itself uses a 16-bit rolling code with no encryption — that is a property of the protocol, not a bug in this firmware.

All configuration happens locally over USB serial from a static page; nothing is sent to any server.
