// SPDX-License-Identifier: Unlicense
// Minimal CC1101 driver for Somfy RTS transmit: 433.42 MHz OOK, async-serial TX.
// Reference: TI CC1101 datasheet (SWRS061); register base adapted from
// kgun2g/somfy-rts-remote-by-thread.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "board.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CC1101_FREQ2    0x0D
#define CC1101_FREQ1    0x0E
#define CC1101_FREQ0    0x0F
#define CC1101_MDMCFG4  0x10
#define CC1101_PATABLE  0x3E
#define CC1101_RSSI     0x34

#define CC1101_TX_POWER_COUNT 8
#define CC1101_RXBW_COUNT     5

#define CC1101_SRES     0x30
#define CC1101_SCAL     0x33
#define CC1101_SRX      0x34
#define CC1101_STX      0x35
#define CC1101_SIDLE    0x36

#define CC1101_XTAL_HZ  26000000UL

typedef struct {
    spi_device_handle_t spi;
} cc1101_t;

/**
 * Bring up SPI, reset the chip, and load the OOK/433 register set.
 * @return false if the chip does not answer (missing or miswired); the caller
 *         keeps running without RF.
 */
bool  cc1101_init(cc1101_t *dev);

/**
 * Retune the carrier and recalibrate the synthesiser.
 * @param freq_mhz Carrier frequency in MHz.
 */
void  cc1101_set_frequency(cc1101_t *dev, float freq_mhz);

/**
 * Set the OOK TX level by index into cc1101_tx_power_dbm[] (out-of-range clamps
 * to the top level). Writes PATABLE[1]; index 0 (carrier off) is untouched.
 */
void cc1101_set_power(cc1101_t *dev, uint8_t idx);

/**
 * Set the RX bandwidth by index into cc1101_rxbw_khz[] (out-of-range falls back
 * to the default). Rewrites MDMCFG4, preserving the data-rate exponent nibble.
 */
void cc1101_set_rxbw(cc1101_t *dev, uint8_t idx);

/**
 * @return The current received signal strength in dBm. Meaningful only when the
 *         chip is in RX and has settled (a few hundred µs after entering RX).
 */
int cc1101_rssi_dbm(cc1101_t *dev);

/** Output level in dBm per TX-power index (E07/CC1101 433 MHz, +10 dBm max). */
extern const int8_t cc1101_tx_power_dbm[CC1101_TX_POWER_COUNT];

/** RX bandwidth in kHz per RX-bandwidth index. */
extern const uint16_t cc1101_rxbw_khz[CC1101_RXBW_COUNT];

/**
 * Enter TX. In async-serial mode the waveform on GDO0 keys the carrier.
 */
void  cc1101_enter_tx_mode(cc1101_t *dev);

/**
 * Enter RX. In async-serial mode the demodulated OOK data streams out on GDO2.
 */
void  cc1101_enter_rx_mode(cc1101_t *dev);

/**
 * Return the chip to IDLE.
 */
void  cc1101_idle(cc1101_t *dev);

/**
 * Write one configuration register.
 */
void    cc1101_write_reg(cc1101_t *dev, uint8_t addr, uint8_t val);

/**
 * Read one status register (burst bit is applied internally).
 */
uint8_t cc1101_read_reg(cc1101_t *dev, uint8_t addr);

#ifdef __cplusplus
}
#endif
