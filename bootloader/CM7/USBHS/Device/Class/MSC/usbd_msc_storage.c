/**
 * @file usbd_msc_storage.c
 * @brief USB MSC backing store: the boot medium selected by boot_storage.h.
 */
#include "usbd_msc_storage.h"

#include "boot_storage.h"

int8_t USBD1_STORAGE_Init(uint8_t lun);
int8_t USBD1_STORAGE_GetCapacity(uint8_t lun, uint32_t *block_num, uint32_t *block_size);
int8_t USBD1_STORAGE_IsReady(uint8_t lun);
int8_t USBD1_STORAGE_IsWriteProtected(uint8_t lun);
int8_t USBD1_STORAGE_Read(uint8_t lun, uint8_t *buf, uint32_t blk_addr, uint16_t blk_len);
int8_t USBD1_STORAGE_Write(uint8_t lun, uint8_t *buf, uint32_t blk_addr, uint16_t blk_len);
int8_t USBD1_STORAGE_GetMaxLun(void);

/* USB Mass storage Standard Inquiry Data */
const int8_t USBD1_STORAGE_Inquirydata[] =
{/*36*/
    /* LUN 0 */
    0x00,
    0x80,
    0x02,
    0x02,
    (USBD_STD_INQUIRY_LENGTH - 4u),
    0x00,
    0x00,
    0x00,
    /* Vendor Identification */
    'N', 'S', 'I', 'N', 'G', ' ', ' ', ' ',         // 8 bytes
    /* Product Identification */
    'N', '3', '2', 'H', '7', 'x', 'x', ' ',        //16 bytes
    'B', 'o', 'o', 't', ' ', ' ', ' ', ' ',
    /* Product Revision Level */
    '1', '.', '0', ' ',                             //4 bytes
};

USBD_STORAGE_cb_TypeDef USBD1_STORAGE_fops =
{
    USBD1_STORAGE_Init,
    USBD1_STORAGE_GetCapacity,
    USBD1_STORAGE_IsReady,
    USBD1_STORAGE_IsWriteProtected,
    USBD1_STORAGE_Read,
    USBD1_STORAGE_Write,
    USBD1_STORAGE_GetMaxLun,
    (int8_t *)USBD1_STORAGE_Inquirydata,
};

/**
*\*\name   USBD1_STORAGE_Init.
*\*\fun    Initialize the storage.
*\*\param  lun : logical unit number
*\*\return Status.
**/
int8_t USBD1_STORAGE_Init(uint8_t lun)
{
    return (USBD1_STORAGE_IsReady(lun) == 0) ? 0 : -1;
}

/**
*\*\name   USBD1_STORAGE_GetCapacity.
*\*\fun    Get Storage capacity.
*\*\param  lun : logical unit number
*\*\param  block_num : number of physical block
*\*\param  block_size : size of a physical block
*\*\return Status.
**/
int8_t USBD1_STORAGE_GetCapacity(uint8_t lun, uint32_t *block_num,
                                 uint32_t *block_size)
{
    uint32_t blocks = BOOT_StorageBlockCount();

    if (lun != 0U || block_num == NULL || block_size == NULL || blocks == 0U)
    {
        return -1;
    }

    *block_size = BOOT_STORAGE_LBA_BYTES;
    *block_num = blocks;
    return 0;
}

/**
*\*\name   USBD1_STORAGE_IsReady.
*\*\fun    Check whether the storage is ready.
*\*\param  lun : logical unit number
*\*\return Status.
**/
int8_t USBD1_STORAGE_IsReady(uint8_t lun)
{
    if (lun != 0U || BOOT_StorageBlockCount() == 0U)
    {
        return -1;
    }
    return 0;
}

/**
*\*\name   USBD1_STORAGE_IsWriteProtected.
*\*\fun    check whether the storage is write-protected.
*\*\param  lun : logical unit number
*\*\return Status.
**/
int8_t USBD1_STORAGE_IsWriteProtected(uint8_t lun)
{
    (void)lun;
    return 0;
}

/**
*\*\name   USBD1_STORAGE_Read.
*\*\fun    Read data from the storage device.
*\*\param  lun: logical unit number
*\*\param  buf: Pointer to the buffer to save data
*\*\param  blk_addr: address of 1st block to be read
*\*\param  blk_len: number of blocks to be read
*\*\return Status.
**/
int8_t USBD1_STORAGE_Read(uint8_t lun, uint8_t *buf, uint32_t blk_addr,
                          uint16_t blk_len)
{
    uint32_t blocks = BOOT_StorageBlockCount();

    if (USBD1_STORAGE_IsReady(lun) != 0 || buf == NULL || blk_len == 0U ||
        blk_addr >= blocks || (uint32_t)blk_len > blocks - blk_addr)
    {
        return -1;
    }

    return BOOT_StorageReadBlocks(buf, blk_addr, blk_len) ? 0 : -1;
}

/**
*\*\name   USBD1_STORAGE_Write.
*\*\fun    Write data to the storage device.
*\*\param  lun: logical unit number
*\*\param  buf: Pointer to the buffer to write from
*\*\param  blk_addr: address of 1st block to be written
*\*\param  blk_len: number of blocks to be write
*\*\return Status.
**/
int8_t USBD1_STORAGE_Write(uint8_t lun, uint8_t *buf, uint32_t blk_addr,
                           uint16_t blk_len)
{
    uint32_t blocks = BOOT_StorageBlockCount();

    if (USBD1_STORAGE_IsReady(lun) != 0 || buf == NULL || blk_len == 0U ||
        blk_addr >= blocks || (uint32_t)blk_len > blocks - blk_addr)
    {
        return -1;
    }

    return BOOT_StorageWriteBlocks(buf, blk_addr, blk_len) ? 0 : -1;
}

/**
*\*\name   USBD1_STORAGE_GetMaxLun.
*\*\fun    Return number of supported logical unit.
*\*\param  none
*\*\return number of logical unit.
**/
int8_t USBD1_STORAGE_GetMaxLun(void)
{
    return (USBD_STORAGE_LUN_NBR - 1);
}
