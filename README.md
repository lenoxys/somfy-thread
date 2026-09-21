# somfy-thread

Control Somfy RTS blinds over **Matter-over-Thread** — set up entirely from a browser page, no toolchain, no cloud, no app. It's a standard Matter device, so it works with any Matter controller and Thread border router.

<!--
  DEMO — replace the placeholder below with the recorded walkthrough.
  · GIF:  drop docs/demo.gif in the repo and keep the <img> tag.
  · MP4:  drag the file into a GitHub issue/PR comment, copy the resulting
          https://github.com/user-attachments/... URL, and swap the <img> for:
          <video src="https://github.com/user-attachments/assets/..." controls width="720"></video>
-->
<p align="center">
  <a href="https://lenoxys.github.io/somfy-thread/">
    <img src="docs/demo.gif" alt="somfy-thread browser setup walkthrough" width="720">
  </a>
  <br>
  <em>▶ Open the setup page — connect, add your shades, pair Matter. All in the browser.</em>
</p>

## → [Set it up in your browser](https://lenoxys.github.io/somfy-thread/)

Open the page in **Chrome or Edge**, plug the board into USB, and follow the wizard — everything runs locally over Web Serial and nothing is sent anywhere.

- **Board** — connects over Web Serial and reads the firmware version; flashes or updates in-page with [ESP Web Tools](https://esphome.github.io/esp-web-tools/) when needed. *(Firefox/Safari lack Web Serial — download the `.bin` and flash with `esptool`.)*
- **Shades** — each shade is one motor: name it, run **PROG** (long-press PROG on the motor's existing remote to pair), and test with **Open / Close / My / Stop**. Add a shade by cloning a remote the board hears, or PROG-pair a motor with no remote.
- **Matter** — get the pairing code and add the device to any Matter controller.
- **Backup** — **export** a JSON backup and **import** it to restore or migrate.

Each shade exposes its own WindowCovering endpoint with a stable Matter identity; rolling codes are persisted before every transmit, so a reboot never rewinds them.

> **Test certificate — allow it on your controller.** Builds ship an uncertified **test/development attestation certificate**. In **Home Assistant** (Matter Server) enable **"Enable test-net DCL usage."** before pairing. **Apple Home** and **Google Home** offer no such override and will refuse the device until the project ships a certified identity.

## Hardware

Waveshare ESP32-C6-Zero + E07-M1101D (CC1101, 433 MHz). Full wiring, pin map, and radio notes: **[docs/HARDWARE.md](docs/HARDWARE.md)**.

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

Each release ships a `somfy-thread-esp32c6-<version>.bin` with a build-provenance attestation: `gh attestation verify somfy-thread-esp32c6-<version>.bin --repo lenoxys/somfy-thread`.

## Scope

Drives **Somfy RTS only** (433 MHz, OOK, rolling code in the clear). io-homecontrol is out of reach here — different band (868/915 MHz, needs a second radio) and AES-encrypted with no open stack to build on.

## Disclaimer

An independent open-source project, **not affiliated with, endorsed by, or sponsored by Somfy**. Somfy and RTS are trademarks of their respective owner, used here only to describe interoperability.

## Credits

- **[kgun2g/somfy-rts-remote-by-thread](https://github.com/kgun2g/somfy-rts-remote-by-thread)** — Matter-over-Thread + CC1101 firmware structure and register base.
- **[Nickduino/Somfy_Remote](https://github.com/Nickduino/Somfy_Remote)** — canonical Somfy RTS frame/protocol reference.
- **[ESPSomfy-RTS](https://github.com/rstrouse/ESPSomfy-RTS)** — RTS timing and behaviour reference.
- **[esp-matter](https://github.com/espressif/esp-matter)** / **[esp-idf](https://github.com/espressif/esp-idf)** — Matter/Thread stack and SDK.
- **[ESP Web Tools](https://github.com/esphome/esp-web-tools)** — in-browser flashing.

## Support

Public domain and free — if it saved you a controller, you can [sponsor the project](https://github.com/sponsors/lenoxys). Optional, always appreciated.

## License

Released into the public domain under [The Unlicense](./LICENSE).
