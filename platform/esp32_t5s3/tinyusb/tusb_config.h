// SPDX-License-Identifier: Apache-2.0
//
// TinyUSB's configuration for the card's USB disk mode
// (docs/adr/0036-the-card-as-a-usb-disk.md): a full-speed device with one
// mass-storage interface and nothing else. TinyUSB includes this by name;
// apps/firmware_esp32/components/hac_platform/CMakeLists.txt puts this
// directory on TinyUSB's include path and on usb_disk.c's. Written for this
// project rather than taken from Espressif's esp_tinyusb, which is not a
// dependency (the ADR says why).
//
// Every class TinyUSB builds is off but mass storage: tusb_option.h defaults
// each CFG_TUD_<class> to 0, and the classes' sources compile to nothing.

#ifndef HAC_PLATFORM_ESP32_T5S3_TUSB_CONFIG_H
#define HAC_PLATFORM_ESP32_T5S3_TUSB_CONFIG_H

#include "usb_disk_limits.h"

// CFG_TUSB_MCU comes from the TinyUSB component's compile options
// (OPT_MCU_ESP32S3), and with it the FreeRTOS include path for ESP-IDF.
#define CFG_TUSB_OS OPT_OS_FREERTOS
#define CFG_TUSB_DEBUG 0

#define CFG_TUD_ENABLED 1
#define CFG_TUD_MAX_SPEED OPT_MODE_FULL_SPEED
// The S3's OTG controller in slave mode: the CPU moves the FIFO's bytes,
// which at full speed's 12 Mbit/s is little work.
#define CFG_TUD_DWC2_SLAVE_ENABLE 1
#define CFG_TUD_ENDPOINT0_SIZE 64

// TinyUSB's buffers are static: in internal RAM, word-aligned, where the SD
// driver's SPI DMA reads and writes the card's sectors directly.
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))

#define CFG_TUD_MSC 1
#define CFG_TUD_MSC_EP_BUFSIZE USB_DISK_MSC_BUFFER_BYTES

#endif  // HAC_PLATFORM_ESP32_T5S3_TUSB_CONFIG_H
