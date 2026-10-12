/**
 * @file usbd_msc.h
 * @author N32cube
 */

#ifndef __USBD_MSC_H
#define __USBD_MSC_H
#include "usbd_msc_storage.h"
/* NTFx CODE START */
#define SENSE_LIST_DEEPTH                          4
#define USBD1_MSC_MEDIA_PACKET              (32U * 1024U)

typedef struct _MSC_BOT_CBW
{
  uint32_t dSignature;
  uint32_t dTag;
  uint32_t dDataLength;
  uint8_t  bmFlags;
  uint8_t  bLUN;
  uint8_t  bCBLength;
  uint8_t  CB[16];
}
MSC_BOT_CBW_TypeDef;


typedef struct _MSC_BOT_CSW
{
  uint32_t dSignature;
  uint32_t dTag;
  uint32_t dDataResidue;
  uint8_t  bStatus;
}
MSC_BOT_CSW_TypeDef;

typedef struct _MSC_BOT_Info
{
    uint8_t *Data;
    uint32_t DataBufMaxSize;
    uint32_t DataLen;
    uint8_t TaskState;
    uint8_t Status;
    MSC_BOT_CBW_TypeDef  cbw;
    MSC_BOT_CSW_TypeDef  csw;
}
MSC_BOT_Info_TypeDef;

typedef struct  {                
  char Skey;
  union {
    struct _ASCs {
      char ASC;
      char ASCQ;
    }b;
    unsigned int    ASC;
    char *pData;
  } w;
} SCSI_Sense_TypeDef; 

typedef struct  {                
SCSI_Sense_TypeDef     Senses [SENSE_LIST_DEEPTH];
uint8_t   Head;
uint8_t   Tail;
} SCSI_Sense_Info_TypeDef; 

typedef struct  {                
uint32_t  Size;
uint32_t  Num;
uint32_t  Addr;
uint32_t  Len;
} SCSI_Block_Info_TypeDef; 


typedef struct
{
    MSC_BOT_Info_TypeDef* MSC_BOT_Info;
    SCSI_Block_Info_TypeDef *SCSI_BlkInfo;
    USBD_STORAGE_cb_TypeDef * USBD_STORAGE_fops;
    SCSI_Sense_Info_TypeDef  *USBD_SCSI_SenseInfo;
}USBD_MSC_GlobalVar_Info;
/* NTFx CODE END */
#endif /*__USBD_MSC_H*/
