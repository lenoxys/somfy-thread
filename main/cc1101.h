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

#define CC1101_IOCFG0   0x02
#define CC1101_PKTCTRL0 0x08
#define CC1101_FREQ2    0x0D
#define CC1101_FREQ1    0x0E
#define CC1101_FREQ0    0x0F
#define CC1101_MDMCFG2  0x12
#define CC1101_FREND0   0x22
#define CC1101_PATABLE  0x3E

#define CC1101_SRES     0x30
#define CC1101_SCAL     0x33
#define CC1101_STX      0x35
#define CC1101_SIDLE    0x36
#define CC1101_SNOP     0x3D

#define CC1101_XTAL_HZ  26000000UL

typedef struct {
    spi_device_handle_t spi;
    float               freq_mhz;
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
 * @return The current carrier frequency in MHz.
 */
float cc1101_get_frequency(const cc1101_t *dev);

/**
 * Enter TX. In async-serial mode the waveform on GDO0 keys the carrier.
 */
void  cc1101_enter_tx_mode(cc1101_t *dev);

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

/**
 * Send a command strobe.
 */
void    cc1101_strobe(cc1101_t *dev, uint8_t cmd);

#ifdef __cplusplus
}
#endif
