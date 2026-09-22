/* SPDX-License-Identifier: Unlicense */
/*
 * CHIP project-config overrides, wired via CONFIG_CHIP_PROJECT_CONFIG. Sets the
 * Basic Information name strings (shown after commissioning) plus the
 * commissionable device name (the "DN" advertised during BLE/DNS-SD discovery),
 * so the device reads as somfy-thread instead of CHIP's "TEST_VENDOR"/
 * "TEST_PRODUCT" defaults and a commissioner's generic "Matter Accessory"
 * fallback. These are cosmetic: VendorID/ProductID (CONFIG_DEVICE_VENDOR_ID/
 * _PRODUCT_ID) are left at the example DAC's 0xFFF1/0x8000 so device attestation
 * stays consistent. CHIP's own defaults are #ifndef-guarded, so defining them
 * before that header wins.
 */
#pragma once

#define CHIP_DEVICE_CONFIG_DEVICE_VENDOR_NAME "somfy-thread"
#define CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_NAME "somfy-thread"
#define CHIP_DEVICE_CONFIG_DEFAULT_DEVICE_HARDWARE_VERSION_STRING "ESP32-C6 + E07-M1101D"

#define CHIP_DEVICE_CONFIG_ENABLE_COMMISSIONABLE_DEVICE_NAME 1
#define CHIP_DEVICE_CONFIG_DEVICE_NAME "somfy-thread"
