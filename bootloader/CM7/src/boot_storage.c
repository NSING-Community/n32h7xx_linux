/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file boot_storage.c
 * @brief Bootloader storage backend glue.
 */
#include "boot_storage.h"

#if defined(BOOT_STORAGE_SPI_FLASH)

bool BOOT_StorageInit(void)
{
    return SpiFlash_Init();
}

uint32_t BOOT_StorageBlockCount(void)
{
    return SPIFLASH_BLOCK_COUNT;
}

bool BOOT_StorageReadBlocks(uint8_t *buf, uint32_t block, uint32_t count)
{
    return SpiFlash_ReadBlocks(buf, block, count);
}

bool BOOT_StorageWriteBlocks(const uint8_t *buf, uint32_t block, uint32_t count)
{
    return SpiFlash_WriteBlocks(buf, block, count);
}

#else /* BOOT_STORAGE_SDMMC */

bool BOOT_StorageInit(void)
{
    return SD_App_Init();
}

uint32_t BOOT_StorageBlockCount(void)
{
    sd_card_t *card = SD_App_Card();

    return (card != NULL) ? card->sd_card_information.blockCount : 0U;
}

bool BOOT_StorageReadBlocks(uint8_t *buf, uint32_t block, uint32_t count)
{
    return SD_ReadBlocks(SD_App_Card(), (uint32_t *)(void *)buf, block,
                         (uint16_t)count) == Status_Success;
}

bool BOOT_StorageWriteBlocks(const uint8_t *buf, uint32_t block,
                             uint32_t count)
{
    return SD_WriteBlocks(SD_App_Card(), (uint32_t *)(void *)buf, block,
                          (uint16_t)count) == Status_Success;
}

#endif
