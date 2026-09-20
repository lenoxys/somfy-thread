/* SPDX-License-Identifier: Unlicense */
/*
 * CHIP project-config overrides, wired via CONFIG_CHIP_PROJECT_CONFIG. Only the
 * Basic Information name strings are set here so the device shows as somfy-thread
 * instead of CHIP's "TEST_VENDOR"/"TEST_PRODUCT" defaults. These are cosmetic:
 * VendorID/ProductID (CONFIG_DEVICE_VENDOR_ID/_PRODUCT_ID) are left at the example
 * DAC's 0xFFF1/0x8000 so device attestation stays consistent. CHIP's own defaults
 * are #ifndef-guarded, so defining them before that header wins.
 */
#pragma once

#define CHIP_DEVICE_CONFIG_DEVICE_VENDOR_NAME "somfy-thread"
#define CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_NAME "somfy-thread"
#define CHIP_DEVICE_CONFIG_DEVICE_HARDWARE_VERSION_STRING "ESP32-C6 + E07-M1101D"
