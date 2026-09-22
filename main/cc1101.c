// SPDX-License-Identifier: Unlicense
// CC1101 driver for Somfy RTS TX: 433.42 MHz OOK, asynchronous-serial mode.
#include "cc1101.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"
#include <string.h>

static const char *TAG = "cc1101";
static void cc1101_strobe(cc1101_t *dev, uint8_t cmd);

/**
 * Register set for 433 MHz OOK in asynchronous-serial mode, shared by TX and RX.
 * Each entry is {address, value}. The functional choices are: IOCFG2 (0x00) =
 * 0x0D streams the demodulated RX data out on GDO2; IOCFG0 (0x02) = 0x2E puts
 * GDO0 in 3-state so the RMT peripheral drives it as the TX-data input; PKTCTRL0
 * (0x08) = 0x32 selects async-serial mode with infinite packet length; MDMCFG2
 * (0x12) = 0x34 selects OOK (the SYNC_MODE=4 field is inert here because the
 * async packet engine is bypassed and TX ignores it); AGCCTRL2 (0x1B) = 0xC3
 * disables the top three DVGA gain steps so the idle noise floor stays below the
 * OOK slicer — without it the free-running AGC amplifies band noise into a
 * continuous edge stream on GDO2 that floods the RX capture (measured on-air:
 * ~400 spurious frames/s at 0x03, zero at 0xC3, real remotes still decode);
 * FREND0 (0x22) = 0x11 makes the PA ramp between PATABLE[0] (off) and PATABLE[1]
 * (on). The remaining entries are the standard 433 MHz front-end/calibration
 * defaults. FREQ is written separately from the runtime frequency.
 */
static const uint8_t init_regs[][2] = {
    {0x00, 0x0D}, {0x02, 0x2E}, {0x03, 0x47}, {0x06, 0xFF}, {0x07, 0x04}, {0x08, 0x32},
    {0x0B, 0x06}, {0x0C, 0x00}, {0x10, 0x8A}, {0x11, 0x83}, {0x12, 0x34},
    {0x13, 0x22}, {0x14, 0xF8}, {0x18, 0x18}, {0x19, 0x16}, {0x1B, 0xC3},
    {0x1C, 0x40}, {0x1D, 0x91}, {0x21, 0x56}, {0x22, 0x11}, {0x23, 0xE9},
    {0x24, 0x2A}, {0x25, 0x00}, {0x26, 0x1F}, {0x2C, 0x81}, {0x2D, 0x35},
    {0x2E, 0x09},
};

/**
 * TI manual power-up reset (SWRS061 section 19.1.2), bit-banged before the SPI
 * driver takes the pins. After CSn goes low it waits for MISO to fall low
 * (XOSC stable) before and after strobing SRES; the E07 module otherwise reads
 * its registers back as zero.
 * @return false if MISO never falls low, indicating a power or wiring fault.
 */
static bool manual_reset(void)
{
    const gpio_num_t SCK = CC1101_PIN_SCK, MOSI = CC1101_PIN_MOSI;
    const gpio_num_t MISO = CC1101_PIN_MISO, CS = CC1101_PIN_CS;

    gpio_config_t out = {
        .pin_bit_mask = (1ULL << SCK) | (1ULL << MOSI) | (1ULL << CS),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_config_t in = { .pin_bit_mask = (1ULL << MISO), .mode = GPIO_MODE_INPUT };
    gpio_config(&in);

    gpio_set_level(SCK, 0);
    gpio_set_level(MOSI, 0);
    gpio_set_level(CS, 1);
    esp_rom_delay_us(50);

    gpio_set_level(CS, 0);
    esp_rom_delay_us(10);
    gpio_set_level(CS, 1);
    esp_rom_delay_us(60);
    gpio_set_level(CS, 0);

    int timeout_us = 5000;
    while (gpio_get_level(MISO) && timeout_us > 0) {
        esp_rom_delay_us(10);
        timeout_us -= 10;
    }
    if (timeout_us <= 0) {
        gpio_set_level(CS, 1);
        ESP_LOGE(TAG, "MISO(IO%d) never went LOW after CSn — check CC1101 power/wiring", MISO);
        return false;
    }

    uint8_t cmd = CC1101_SRES;
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(MOSI, (cmd >> i) & 1);
        esp_rom_delay_us(1);
        gpio_set_level(SCK, 1);
        esp_rom_delay_us(1);
        gpio_set_level(SCK, 0);
    }
    timeout_us = 10000;
    while (gpio_get_level(MISO) && timeout_us > 0) {
        esp_rom_delay_us(10);
        timeout_us -= 10;
    }
    gpio_set_level(CS, 1);
    return true;
}

/**
 * Write a register or, when len > 1, a burst starting at addr.
 */
static void spi_write(cc1101_t *dev, uint8_t addr, const uint8_t *data, size_t len)
{
    uint8_t tx[1 + len];
    tx[0] = (len > 1) ? (addr | 0x40) : addr;
    memcpy(&tx[1], data, len);
    spi_transaction_t t = { .length = (1 + len) * 8, .tx_buffer = tx };
    spi_device_polling_transmit(dev->spi, &t);
}

/**
 * Read a CC1101 status register. Status registers (0x30-0x3D) require the burst
 * bit set (0xC0 | addr) or the chip treats the access as a command strobe.
 */
static uint8_t spi_read_status(cc1101_t *dev, uint8_t addr)
{
    uint8_t tx[2] = { (uint8_t)(0xC0 | addr), 0xFF };
    uint8_t rx[2] = { 0, 0 };
    spi_transaction_t t = { .length = 16, .tx_buffer = tx, .rx_buffer = rx };
    spi_device_polling_transmit(dev->spi, &t);
    return rx[1];
}

/**
 * Program the three FREQ registers from a frequency in MHz.
 */
static void set_freq_regs(cc1101_t *dev, float freq_mhz)
{
    uint32_t reg = (uint32_t)((double)freq_mhz * 1e6 * 65536.0 / (double)CC1101_XTAL_HZ);
    cc1101_write_reg(dev, CC1101_FREQ2, (reg >> 16) & 0xFF);
    cc1101_write_reg(dev, CC1101_FREQ1, (reg >> 8) & 0xFF);
    cc1101_write_reg(dev, CC1101_FREQ0, reg & 0xFF);
    ESP_LOGI(TAG, "freq %.3f MHz -> FREQ=0x%06lX", freq_mhz, (unsigned long)(reg & 0xFFFFFF));
}

/**
 * PATABLE "on" byte per TX-power index for the E07 (CC1101) at 433 MHz, from the
 * datasheet output-power table (SWRS061; +10 dBm is this module's ceiling).
 */
static const uint8_t pa_on[CC1101_TX_POWER_COUNT] = {0x12, 0x0E, 0x1D, 0x34, 0x60, 0x84, 0xC8, 0xC0};
const int8_t cc1101_tx_power_dbm[CC1101_TX_POWER_COUNT] = {-30, -20, -15, -10, 0, 5, 7, 10};

/**
 * RX bandwidth presets as the MDMCFG4 high nibble (CHANBW_E<<2 | CHANBW_M).
 * BW = f_xosc / (8·(4+CHANBW_M)·2^CHANBW_E) with f_xosc = 26 MHz; the low nibble
 * (DRATE_E) is inert for OOK RX and kept at the init value 0x0A.
 */
static const uint8_t rxbw_hi[CC1101_RXBW_COUNT] = {0xF, 0xC, 0x8, 0x4, 0x0};
const uint16_t cc1101_rxbw_khz[CC1101_RXBW_COUNT] = {58, 102, 203, 406, 813};
#define MDMCFG4_DRATE_E 0x0A
#define RXBW_DEFAULT_IDX 2

void cc1101_set_power(cc1101_t *dev, uint8_t idx)
{
    if (idx >= CC1101_TX_POWER_COUNT) idx = CC1101_TX_POWER_COUNT - 1;
    uint8_t table[8] = { 0x00, pa_on[idx], 0, 0, 0, 0, 0, 0 };
    spi_write(dev, CC1101_PATABLE | 0x40, table, 8);
}

void cc1101_set_rxbw(cc1101_t *dev, uint8_t idx)
{
    if (idx >= CC1101_RXBW_COUNT) idx = RXBW_DEFAULT_IDX;
    cc1101_write_reg(dev, CC1101_MDMCFG4, (uint8_t)(rxbw_hi[idx] << 4) | MDMCFG4_DRATE_E);
}

/**
 * Read the RSSI status register and convert to dBm (SWRS061 17.3): a 2's-complement
 * half-dB value offset by the 74 dB RSSI offset for the 433 MHz band.
 */
int cc1101_rssi_dbm(cc1101_t *dev)
{
    uint8_t raw = spi_read_status(dev, CC1101_RSSI);
    int r = raw >= 128 ? (int)raw - 256 : (int)raw;
    return r / 2 - 74;
}

/**
 * Reset the chip, verify it responds, and apply the OOK/433 configuration.
 * @return false if the chip does not respond over SPI.
 */
bool cc1101_init(cc1101_t *dev)
{
    if (!manual_reset()) return false;

    spi_bus_config_t buscfg = {
        .miso_io_num = CC1101_PIN_MISO,
        .mosi_io_num = CC1101_PIN_MOSI,
        .sclk_io_num = CC1101_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64,
    };
    esp_err_t e = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_DISABLED);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(e));
        return false;
    }
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 4 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = CC1101_PIN_CS,
        .queue_size = 4,
    };
    if (spi_bus_add_device(SPI2_HOST, &devcfg, &dev->spi) != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed");
        return false;
    }

    cc1101_strobe(dev, CC1101_SRES);
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t partnum = spi_read_status(dev, 0x30);
    uint8_t version = spi_read_status(dev, 0x31);
    ESP_LOGI(TAG, "PARTNUM=0x%02X VERSION=0x%02X", partnum, version);
    if ((version == 0xFF && partnum == 0xFF) || (version == 0x00 && partnum == 0x00)) {
        ESP_LOGE(TAG, "CC1101 not responding — RF disabled");
        return false;
    }

    for (size_t i = 0; i < sizeof(init_regs) / sizeof(init_regs[0]); i++)
        cc1101_write_reg(dev, init_regs[i][0], init_regs[i][1]);
    set_freq_regs(dev, BOARD_DEFAULT_FREQ_MHZ);
    cc1101_strobe(dev, CC1101_SIDLE);
    ESP_LOGI(TAG, "init OK (OOK %.3f MHz)", BOARD_DEFAULT_FREQ_MHZ);
    return true;
}

/**
 * Set the carrier frequency, then run and settle synthesiser calibration.
 */
void cc1101_set_frequency(cc1101_t *dev, float freq_mhz)
{
    cc1101_strobe(dev, CC1101_SIDLE);
    set_freq_regs(dev, freq_mhz);
    cc1101_strobe(dev, CC1101_SCAL);
    esp_rom_delay_us(2000);
}

/**
 * Move to IDLE then TX; FS auto-calibration (MCSM0) runs on this transition.
 */
void cc1101_enter_tx_mode(cc1101_t *dev)
{
    cc1101_strobe(dev, CC1101_SIDLE);
    esp_rom_delay_us(200);
    cc1101_strobe(dev, CC1101_STX);
    esp_rom_delay_us(1000);
}

/**
 * Move to IDLE then RX so the demodulated OOK stream appears on GDO2. Called at
 * startup and again after each transmit (the TX path leaves the chip idle).
 */
void cc1101_enter_rx_mode(cc1101_t *dev)
{
    cc1101_strobe(dev, CC1101_SIDLE);
    esp_rom_delay_us(200);
    cc1101_strobe(dev, CC1101_SRX);
    esp_rom_delay_us(1000);
}

void cc1101_idle(cc1101_t *dev)
{
    cc1101_strobe(dev, CC1101_SIDLE);
}

void cc1101_write_reg(cc1101_t *dev, uint8_t addr, uint8_t val)
{
    spi_write(dev, addr, &val, 1);
}

uint8_t cc1101_read_reg(cc1101_t *dev, uint8_t addr)
{
    return spi_read_status(dev, addr);
}

static void cc1101_strobe(cc1101_t *dev, uint8_t cmd)
{
    uint8_t status = 0;
    spi_transaction_t t = { .length = 8, .tx_buffer = &cmd, .rx_buffer = &status };
    spi_device_polling_transmit(dev->spi, &t);
}
