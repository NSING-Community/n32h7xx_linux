/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file spi_flash.h
 * @brief On-board W25Q128 NOR flash on XSPI2 (N32H787 HMI V1.1).
 *
 * The device is a 16 MiB W25Q128 wired to XSPI2:
 *
 *   NCS1 = PG12 (AF0), CLK = PB2 (AF2),
 *   IO0  = PF8  (AF1), IO1 = PB0 (AF1),
 *   IO2  = PF7  (AF1), IO3 = PF6 (AF1).
 *
 * The whole device is exposed as a raw block device: logical block 0 is flash
 * offset 0, which is also where the kernel image is expected.
 */
#ifndef __SPI_FLASH_H__
#define __SPI_FLASH_H__

#include <stdbool.h>
#include <stdint.h>

/** Device geometry. */
#define SPIFLASH_TOTAL_BYTES   (16U * 1024U * 1024U)
#define SPIFLASH_SECTOR_BYTES  (4U * 1024U)
#define SPIFLASH_PAGE_BYTES    (256U)
#define SPIFLASH_BLOCK_BYTES   (512U)
#define SPIFLASH_BLOCK_COUNT   (SPIFLASH_TOTAL_BYTES / SPIFLASH_BLOCK_BYTES)

/** Bring up XSPI2 and the flash. Returns false when the device is unusable. */
bool SpiFlash_Init(void);

/** Read @p len bytes from flash offset @p addr. */
bool SpiFlash_Read(uint32_t addr, uint8_t *dst, uint32_t len);

/** Erase @p len bytes starting at @p addr (sector granularity, 0xFF filled). */
bool SpiFlash_Erase(uint32_t addr, uint32_t len);

/** Program @p len bytes at @p addr into already-erased space. */
bool SpiFlash_Program(uint32_t addr, const uint8_t *src, uint32_t len);

/**
 * Read @p count 512-byte logical blocks starting at block @p block.
 * @p dst must be 4-byte aligned.
 */
bool SpiFlash_ReadBlocks(uint8_t *dst, uint32_t block, uint32_t count);

/**
 * Write @p count 512-byte logical blocks starting at block @p block.
 * Performs read-modify-write at 4 KiB sector granularity.
 */
bool SpiFlash_WriteBlocks(const uint8_t *src, uint32_t block, uint32_t count);

#if defined(SPIFLASH_BENCH)
/** Run the optional board-level throughput benchmark and print the results. */
void SpiFlash_BenchmarkRun(void);
#endif

#endif /* __SPI_FLASH_H__ */
