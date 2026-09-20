// SPDX-License-Identifier: Unlicense
// Board profile: Waveshare ESP32-C6-Zero (native USB-C) + E07-M1101D (CC1101).
// Pins avoid GPIO12/13 (native USB), 8/9/15 (strapping/flash), and GPIO14
// (on-board RF antenna switch). GPIO4/5 are the external-JTAG pins, but the
// console uses the internal USB-Serial-JTAG, so they are free GPIOs here — and
// unlike GPIO23 (a back-side pad only) they are broken out on the left header.
// CC1101 SPI is low-speed, so GPIO-matrix routing on these pins is adequate.
#pragma once

#define BOARD_NAME              "waveshare-c6"

#define CC1101_PIN_SCK          19
#define CC1101_PIN_MISO         20
#define CC1101_PIN_MOSI         4
#define CC1101_PIN_CS           21
#define CC1101_PIN_GD0          22
// GDO2 carries the demodulated OOK data in RX (see ESPSomfy: GDO0=TX, GDO2=RX).
// On GP5 (left header) because GP23 is only a back-side pad you can't wire to.
#define CC1101_PIN_GD2          5

#define BOARD_DEFAULT_FREQ_MHZ  433.42f
#define BOARD_FREQ_MIN_MHZ      433.05f
#define BOARD_FREQ_MAX_MHZ      434.79f

#define BLIND_MAX_COUNT         32
