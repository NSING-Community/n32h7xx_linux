/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file boot_storage.h
 * @brief Bootloader storage backend selection.
 *
 * Exactly one backend is compiled in:
 *
 *   BOOT_STORAGE_SPI_FLASH - XSPI2 + on-board W25Q128 (16 MiB) NOR.
 *                            MSC and the kernel image both live here.
 *   BOOT_STORAGE_SDMMC     - SDMMC1 + microSD card, the previous default.
 *
 * The SDMMC backend and its driver stack are excluded from the build when the
 * SPI flash backend is selected, and vice versa.
 */
#ifndef __BOOT_STORAGE_H__
#define __BOOT_STORAGE_H__

#include <stdbool.h>
#include <stdint.h>

/* Default to the on-board SPI flash unless the build overrides it. */
#if !defined(BOOT_STORAGE_SPI_FLASH) && !defined(BOOT_STORAGE_SDMMC)
#define BOOT_STORAGE_SPI_FLASH 1
#endif

#if defined(BOOT_STORAGE_SPI_FLASH) && defined(BOOT_STORAGE_SDMMC)
#error "Select exactly one boot storage backend"
#endif

#define BOOT_STORAGE_LBA_BYTES 512U

#if defined(BOOT_STORAGE_SPI_FLASH)

#include "spi_flash.h"

/** Bring up the backend. Returns false when the medium is unusable. */
bool BOOT_StorageInit(void);

/** Number of 512-byte logical blocks exposed to USB MSC. */
uint32_t BOOT_StorageBlockCount(void);

/** Read @p count logical blocks starting at @p block into @p buf. */
bool BOOT_StorageReadBlocks(uint8_t *buf, uint32_t block, uint32_t count);

/** Write @p count logical blocks starting at @p block from @p buf. */
bool BOOT_StorageWriteBlocks(const uint8_t *buf, uint32_t block, uint32_t count);

#else /* BOOT_STORAGE_SDMMC */

#include "sd_app.h"

bool BOOT_StorageInit(void);
uint32_t BOOT_StorageBlockCount(void);
bool BOOT_StorageReadBlocks(uint8_t *buf, uint32_t block, uint32_t count);
bool BOOT_StorageWriteBlocks(const uint8_t *buf, uint32_t block, uint32_t count);

#endif

#endif /* __BOOT_STORAGE_H__ */
