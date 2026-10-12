/**
 * @file usbd_msc_storage.h
 * @author N32cube
 */
#ifndef __USBD_MSC_STORAGE_H__
#define __USBD_MSC_STORAGE_H__
/* Includes ------------------------------------------------------------------*/
#include "usbd_def.h"
#include "n32h7xx.h"
/* NTFx CODE START */

#define USBD_STORAGE_LUN_NBR  1
#define USBD_STD_INQUIRY_LENGTH     36

/* NTFx CODE END */

/* NTFx CODE START */
typedef struct 
{
  int8_t (* Init) (uint8_t lun);
  int8_t (* GetCapacity) (uint8_t lun, uint32_t *block_num, uint32_t *block_size);
  int8_t (* IsReady) (uint8_t lun);
  int8_t (* IsWriteProtected) (uint8_t lun);
  int8_t (* Read) (uint8_t lun, uint8_t *buf, uint32_t blk_addr, uint16_t blk_len);
  int8_t (* Write)(uint8_t lun, uint8_t *buf, uint32_t blk_addr, uint16_t blk_len);
  int8_t (* GetMaxLun)(void);
  int8_t *pInquiry;

}USBD_STORAGE_cb_TypeDef;
/* NTFx CODE END */

/* NTFx CODE START */
extern USBD_STORAGE_cb_TypeDef USBD1_STORAGE_fops;
/* NTFx CODE END */
#endif /* __USBD_STORAGE_H__ */
