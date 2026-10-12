/**
 * @file usbd_msc_scsi.c
 * @author N32cube
 */
#include "usbd_msc_scsi.h"
#include "usbd_msc_bot.h"
/* NTFx CODE START */
#define LENGTH_INQUIRY_PAGE00		 7
/* NTFx CODE END */

/* NTFx CODE START */
SCSI_Block_Info_TypeDef USBD1_SCSI_BlkInfo;
SCSI_Sense_Info_TypeDef USBD1_SCSI_SenseInfo;
/* NTFx CODE END */

/* USB Mass storage Page 0 Inquiry Data */
const uint8_t  MSC_Page00_Inquiry_Data[] = 
{//7
    0x00,
    0x00, 
    0x00, 
    (LENGTH_INQUIRY_PAGE00 - 4),
    0x00, 
    0x80, 
    0x83 
};  
/* USB Mass storage sense 6  Data */
const uint8_t  MSC_Mode_Sense6_data[] = 
{
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00, 
    0x00,
    0x00
};
/* USB Mass storage sense 10  Data */
const uint8_t  MSC_Mode_Sense10_data[] = 
{
    0x00,
    0x06, 
    0x00, 
    0x00, 
    0x00, 
    0x00, 
    0x00, 
    0x00
};


static int8_t SCSI_TestUnitReady(USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar,uint8_t lun, uint8_t *params);
static int8_t SCSI_Inquiry(USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar,uint8_t lun, uint8_t *params);
static int8_t SCSI_ReadFormatCapacity(USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar,uint8_t lun, uint8_t *params);
static int8_t SCSI_ReadCapacity10(USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar,uint8_t lun, uint8_t *params);
static int8_t SCSI_RequestSense (USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar,uint8_t lun, uint8_t *params);
static int8_t SCSI_StartStopUnit(USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar,uint8_t lun, uint8_t *params);
static int8_t SCSI_ModeSense6 (USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar,uint8_t lun, uint8_t *params);
static int8_t SCSI_ModeSense10 (USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar,uint8_t lun, uint8_t *params);
static int8_t SCSI_Write10(USB_CORE_MODULE  *pdev,USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar,uint8_t lun , uint8_t *params);
static int8_t SCSI_Read10(USB_CORE_MODULE  *pdev,USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar,uint8_t lun , uint8_t *params);
static int8_t SCSI_Verify10(USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar,uint8_t lun, uint8_t *params);
static int8_t SCSI_CheckAddressRange (USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar,
                                      uint8_t lun ,
                                      uint32_t blk_offset ,
                                      uint16_t blk_nbr);
static int8_t SCSI_ProcessRead (USB_CORE_MODULE  *pdev,USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar,uint8_t lun);

static int8_t SCSI_ProcessWrite (USB_CORE_MODULE  *pdev,USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar,uint8_t lun);

/**
*\*\name   SCSI_ProcessCmd.
*\*\fun    Process SCSI commands.
*\*\param  pdev: USB device
*\*\param  lun: Logical unit number
*\*\param  params: Command parameters
*\*\return status
**/
int8_t SCSI_ProcessCmd(USB_CORE_MODULE  *pdev,USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar, uint8_t lun, uint8_t *params)
{

    switch (params[0])
    {
        case SCSI_TEST_UNIT_READY:
            return SCSI_TestUnitReady(USBD_MSC_GlobalVar,lun, params);

        case SCSI_REQUEST_SENSE:
            return SCSI_RequestSense (USBD_MSC_GlobalVar,lun, params);
        case SCSI_INQUIRY:
            return SCSI_Inquiry(USBD_MSC_GlobalVar,lun, params);

        case SCSI_START_STOP_UNIT:
            return SCSI_StartStopUnit(USBD_MSC_GlobalVar,lun, params);

        case SCSI_ALLOW_MEDIUM_REMOVAL:
            return SCSI_StartStopUnit(USBD_MSC_GlobalVar,lun, params);

        case SCSI_MODE_SENSE6:
            return SCSI_ModeSense6 (USBD_MSC_GlobalVar,lun, params);

        case SCSI_MODE_SENSE10:
            return SCSI_ModeSense10 (USBD_MSC_GlobalVar,lun, params);

        case SCSI_READ_FORMAT_CAPACITIES:
            return SCSI_ReadFormatCapacity(USBD_MSC_GlobalVar,lun, params);

        case SCSI_READ_CAPACITY10:
            return SCSI_ReadCapacity10(USBD_MSC_GlobalVar,lun, params);

        case SCSI_READ10:
            return SCSI_Read10(pdev,USBD_MSC_GlobalVar,lun, params);

        case SCSI_WRITE10:
            return SCSI_Write10(pdev,USBD_MSC_GlobalVar,lun, params);

        case SCSI_VERIFY10:
            return SCSI_Verify10(USBD_MSC_GlobalVar,lun, params);

        default:
            SCSI_SenseCode(USBD_MSC_GlobalVar->USBD_SCSI_SenseInfo,lun, ILLEGAL_REQUEST, INVALID_CDB);
        return -1;
    }
}

/**
*\*\name   SCSI_TestUnitReady.
*\*\fun    Process SCSI Test Unit Ready Command.
*\*\param  lun: Logical unit number
*\*\param  params: Command parameters
*\*\return status
**/
static int8_t SCSI_TestUnitReady(USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar,uint8_t lun, uint8_t *params)
{
    /* case 9 : Hi > D0 */
    if (USBD_MSC_GlobalVar->MSC_BOT_Info->cbw.dDataLength != 0)
    {
        SCSI_SenseCode(USBD_MSC_GlobalVar->USBD_SCSI_SenseInfo,USBD_MSC_GlobalVar->MSC_BOT_Info->cbw.bLUN, ILLEGAL_REQUEST, INVALID_CDB);
        return -1;
    }
    if(USBD_MSC_GlobalVar->USBD_STORAGE_fops->IsReady(lun) !=0 )
    {
        SCSI_SenseCode(USBD_MSC_GlobalVar->USBD_SCSI_SenseInfo,lun, NOT_READY, MEDIUM_NOT_PRESENT);
        return -1;
    }
    USBD_MSC_GlobalVar->MSC_BOT_Info->DataLen = 0;
    return 0;
}

/**
*\*\name   SCSI_Inquiry.
*\*\fun    Process Inquiry command.
*\*\param  lun: Logical unit number
*\*\param  params: Command parameters
*\*\return status
**/
static int8_t SCSI_Inquiry(USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar,uint8_t lun, uint8_t *params)
{
    uint8_t* pPage;
    uint16_t len;

    if (params[1] & 0x01)/*Evpd is set*/
    {
        pPage = (uint8_t *)MSC_Page00_Inquiry_Data;
        len = LENGTH_INQUIRY_PAGE00;
    }
    else
    {
        pPage = (uint8_t *)&USBD_MSC_GlobalVar->USBD_STORAGE_fops->pInquiry[lun * USBD_STD_INQUIRY_LENGTH];
        len = pPage[4] + 5;
        if (params[4] <= len)
        {
            len = params[4];
        }
    }
    USBD_MSC_GlobalVar->MSC_BOT_Info->DataLen = len;

    while (len)
    {
        len--;
        USBD_MSC_GlobalVar->MSC_BOT_Info->Data[len] = pPage[len];
    }
    return 0;
}

/**
*\*\name   SCSI_ReadCapacity10.
*\*\fun    Process Read Capacity 10 command.
*\*\param  lun: Logical unit number
*\*\param  params: Command parameters
*\*\return status
**/
static int8_t SCSI_ReadCapacity10(USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar,uint8_t lun, uint8_t *params)
{

	
    if(USBD_MSC_GlobalVar->USBD_STORAGE_fops->GetCapacity(lun, &USBD_MSC_GlobalVar->SCSI_BlkInfo->Num, &USBD_MSC_GlobalVar->SCSI_BlkInfo->Size) != 0)
    {
        SCSI_SenseCode(USBD_MSC_GlobalVar->USBD_SCSI_SenseInfo,lun, NOT_READY, MEDIUM_NOT_PRESENT);
        return -1;
    }
    else
    {
        USBD_MSC_GlobalVar->MSC_BOT_Info->Data[0] = (uint8_t)((USBD_MSC_GlobalVar->SCSI_BlkInfo->Num - 1) >> 24);
        USBD_MSC_GlobalVar->MSC_BOT_Info->Data[1] = (uint8_t)((USBD_MSC_GlobalVar->SCSI_BlkInfo->Num - 1) >> 16);
        USBD_MSC_GlobalVar->MSC_BOT_Info->Data[2] = (uint8_t)((USBD_MSC_GlobalVar->SCSI_BlkInfo->Num - 1) >>  8);
        USBD_MSC_GlobalVar->MSC_BOT_Info->Data[3] = (uint8_t)(USBD_MSC_GlobalVar->SCSI_BlkInfo->Num - 1);

        USBD_MSC_GlobalVar->MSC_BOT_Info->Data[4] = (uint8_t)(USBD_MSC_GlobalVar->SCSI_BlkInfo->Size >>  24);
        USBD_MSC_GlobalVar->MSC_BOT_Info->Data[5] = (uint8_t)(USBD_MSC_GlobalVar->SCSI_BlkInfo->Size >>  16);
        USBD_MSC_GlobalVar->MSC_BOT_Info->Data[6] = (uint8_t)(USBD_MSC_GlobalVar->SCSI_BlkInfo->Size >>  8);
        USBD_MSC_GlobalVar->MSC_BOT_Info->Data[7] = (uint8_t)(USBD_MSC_GlobalVar->SCSI_BlkInfo->Size);

        USBD_MSC_GlobalVar->MSC_BOT_Info->DataLen = 8;
        return 0;
    }
}

/**
*\*\name   SCSI_ReadFormatCapacity.
*\*\fun    Process Read Format Capacity command.
*\*\param  lun: Logical unit number
*\*\param  params: Command parameters
*\*\return status
**/
static int8_t SCSI_ReadFormatCapacity(USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar,uint8_t lun, uint8_t *params)
{
    uint32_t blk_size;
    uint32_t blk_nbr;
    uint16_t i;

    for(i=0 ; i < 12 ; i++)
    {
        USBD_MSC_GlobalVar->MSC_BOT_Info->Data[i] = 0;
    }

    if(USBD_MSC_GlobalVar->USBD_STORAGE_fops->GetCapacity(lun, &blk_nbr, &blk_size) != 0)
    {
        SCSI_SenseCode(USBD_MSC_GlobalVar->USBD_SCSI_SenseInfo,lun, NOT_READY, MEDIUM_NOT_PRESENT);
        return -1;
    }
    else
    {
        USBD_MSC_GlobalVar->MSC_BOT_Info->Data[3] = 0x08;
        USBD_MSC_GlobalVar->MSC_BOT_Info->Data[4] = (uint8_t)((blk_nbr - 1) >> 24);
        USBD_MSC_GlobalVar->MSC_BOT_Info->Data[5] = (uint8_t)((blk_nbr - 1) >> 16);
        USBD_MSC_GlobalVar->MSC_BOT_Info->Data[6] = (uint8_t)((blk_nbr - 1) >>  8);
        USBD_MSC_GlobalVar->MSC_BOT_Info->Data[7] = (uint8_t)(blk_nbr - 1);

        USBD_MSC_GlobalVar->MSC_BOT_Info->Data[8] = 0x02;
        USBD_MSC_GlobalVar->MSC_BOT_Info->Data[9] = (uint8_t)(blk_size >>  16);
        USBD_MSC_GlobalVar->MSC_BOT_Info->Data[10] = (uint8_t)(blk_size >>  8);
        USBD_MSC_GlobalVar->MSC_BOT_Info->Data[11] = (uint8_t)(blk_size);

        USBD_MSC_GlobalVar->MSC_BOT_Info->DataLen = 12;
        return 0;
    }
}

/**
*\*\name   SCSI_ModeSense6.
*\*\fun    Process Mode Sense6 command.
*\*\param  lun: Logical unit number
*\*\param  params: Command parameters
*\*\return status
**/
static int8_t SCSI_ModeSense6 (USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar,uint8_t lun, uint8_t *params)
{
    uint16_t len = 8 ;
    USBD_MSC_GlobalVar->MSC_BOT_Info->DataLen = len;

    while (len)
    {
        len--;
        USBD_MSC_GlobalVar->MSC_BOT_Info->Data[len] = MSC_Mode_Sense6_data[len];
    }
    return 0;
}

/**
*\*\name   SCSI_ModeSense10.
*\*\fun    Process Mode Sense10 command.
*\*\param  lun: Logical unit number
*\*\param  params: Command parameters
*\*\return status
**/
static int8_t SCSI_ModeSense10 (USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar,uint8_t lun, uint8_t *params)
{
    uint16_t len = 8;

    USBD_MSC_GlobalVar->MSC_BOT_Info->DataLen = len;

    while (len)
    {
        len--;
        USBD_MSC_GlobalVar->MSC_BOT_Info->Data[len] = MSC_Mode_Sense10_data[len];
    }
    return 0;
}

/**
*\*\name   SCSI_RequestSense.
*\*\fun    Process Request Sense command.
*\*\param  lun: Logical unit number
*\*\param  params: Command parameters
*\*\return status
**/
static int8_t SCSI_RequestSense (USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar,uint8_t lun, uint8_t *params)
{
    SCSI_Sense_Info_TypeDef *USBD_SCSI_SenseInfo=USBD_MSC_GlobalVar->USBD_SCSI_SenseInfo;
    uint8_t i;

    for(i=0 ; i < REQUEST_SENSE_DATA_LEN ; i++)
    {
        USBD_MSC_GlobalVar->MSC_BOT_Info->Data[i] = 0;
    }

    USBD_MSC_GlobalVar->MSC_BOT_Info->Data[0] = 0x70;
    USBD_MSC_GlobalVar->MSC_BOT_Info->Data[7] = REQUEST_SENSE_DATA_LEN - 6;

    if((USBD_SCSI_SenseInfo->Head != USBD_SCSI_SenseInfo->Tail)) 
    {

        USBD_MSC_GlobalVar->MSC_BOT_Info->Data[2]     = USBD_SCSI_SenseInfo->Senses[USBD_SCSI_SenseInfo->Head].Skey;
        USBD_MSC_GlobalVar->MSC_BOT_Info->Data[12]    = USBD_SCSI_SenseInfo->Senses[USBD_SCSI_SenseInfo->Head].w.b.ASCQ;
        USBD_MSC_GlobalVar->MSC_BOT_Info->Data[13]    = USBD_SCSI_SenseInfo->Senses[USBD_SCSI_SenseInfo->Head].w.b.ASC;
        USBD_SCSI_SenseInfo->Head++;

        if (USBD_SCSI_SenseInfo->Head == SENSE_LIST_DEEPTH)
        {
            USBD_SCSI_SenseInfo->Head = 0;
        }
    }
    USBD_MSC_GlobalVar->MSC_BOT_Info->DataLen = REQUEST_SENSE_DATA_LEN;

    if (params[4] <= REQUEST_SENSE_DATA_LEN)
    {
        USBD_MSC_GlobalVar->MSC_BOT_Info->DataLen = params[4];
    }
    return 0;
}

/**
*\*\name   SCSI_SenseCode.
*\*\fun    Load the last error code in the error list.
*\*\param  lun: Logical unit number
*\*\param  sKey: Sense Key
*\*\param  ASC: Additional Sense Key
*\*\return none
**/
void SCSI_SenseCode(SCSI_Sense_Info_TypeDef *USBD_SCSI_SenseInfo,uint8_t lun, uint8_t sKey, uint8_t ASC)
{
    USBD_SCSI_SenseInfo->Senses[USBD_SCSI_SenseInfo->Tail].Skey  = sKey;
    USBD_SCSI_SenseInfo->Senses[USBD_SCSI_SenseInfo->Tail].w.ASC = ASC << 8;
    USBD_SCSI_SenseInfo->Tail++;
    if (USBD_SCSI_SenseInfo->Tail == SENSE_LIST_DEEPTH)
    {
        USBD_SCSI_SenseInfo->Tail = 0;
    }
}

/**
*\*\name   SCSI_StartStopUnit.
*\*\fun    Process Start Stop Unit command.
*\*\param  lun: Logical unit number
*\*\param  params: Command parameters
*\*\return status
**/
static int8_t SCSI_StartStopUnit(USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar,uint8_t lun, uint8_t *params)
{
    USBD_MSC_GlobalVar->MSC_BOT_Info->DataLen = 0;
    return 0;
}

/**
*\*\name   SCSI_Read10.
*\*\fun    Process Read10 command.
*\*\param  lun: Logical unit number
*\*\param  params: Command parameters
*\*\return status
**/
static int8_t SCSI_Read10(USB_CORE_MODULE  *pdev,USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar,uint8_t lun , uint8_t *params)
{

    if(USBD_MSC_GlobalVar->MSC_BOT_Info->TaskState == BOT_IDLE)  /* Idle */
    {

        /* case 10 : Ho <> Di */

        if ((USBD_MSC_GlobalVar->MSC_BOT_Info->cbw.bmFlags & 0x80) != 0x80)
        {
            SCSI_SenseCode(USBD_MSC_GlobalVar->USBD_SCSI_SenseInfo,USBD_MSC_GlobalVar->MSC_BOT_Info->cbw.bLUN, ILLEGAL_REQUEST, INVALID_CDB);
            return -1;
        }
        if(USBD_MSC_GlobalVar->USBD_STORAGE_fops->IsReady(lun) !=0 )
        {
            SCSI_SenseCode(USBD_MSC_GlobalVar->USBD_SCSI_SenseInfo,lun, NOT_READY, MEDIUM_NOT_PRESENT);
            return -1;
        }

        USBD_MSC_GlobalVar->SCSI_BlkInfo->Addr = (params[2] << 24) | (params[3] << 16) | (params[4] <<  8) | params[5];
        USBD_MSC_GlobalVar->SCSI_BlkInfo->Len =  (params[7] <<  8) | params[8];
        
        if( SCSI_CheckAddressRange(USBD_MSC_GlobalVar,lun, USBD_MSC_GlobalVar->SCSI_BlkInfo->Addr, USBD_MSC_GlobalVar->SCSI_BlkInfo->Len) < 0)
        {
            return -1; /* error */
        }

        USBD_MSC_GlobalVar->MSC_BOT_Info->TaskState = BOT_DATA_IN;
        USBD_MSC_GlobalVar->SCSI_BlkInfo->Addr *= USBD_MSC_GlobalVar->SCSI_BlkInfo->Size;
        USBD_MSC_GlobalVar->SCSI_BlkInfo->Len  *= USBD_MSC_GlobalVar->SCSI_BlkInfo->Size;

        /* cases 4,5 : Hi <> Dn */
        if (USBD_MSC_GlobalVar->MSC_BOT_Info->cbw.dDataLength != USBD_MSC_GlobalVar->SCSI_BlkInfo->Len)
        {
            SCSI_SenseCode(USBD_MSC_GlobalVar->USBD_SCSI_SenseInfo,USBD_MSC_GlobalVar->MSC_BOT_Info->cbw.bLUN, ILLEGAL_REQUEST, INVALID_CDB);
            return -1;
        }
    }
    USBD_MSC_GlobalVar->MSC_BOT_Info->DataLen = USBD_MSC_GlobalVar->MSC_BOT_Info->DataBufMaxSize;

    return SCSI_ProcessRead(pdev,USBD_MSC_GlobalVar,lun);
}

/**
*\*\name   SCSI_Write10.
*\*\fun    Process Write10 command.
*\*\param  lun: Logical unit number
*\*\param  params: Command parameters
*\*\return status
**/
static int8_t SCSI_Write10 (USB_CORE_MODULE  *pdev,USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar,uint8_t lun , uint8_t *params)
{
    if (USBD_MSC_GlobalVar->MSC_BOT_Info->TaskState == BOT_IDLE) /* Idle */
    {
        /* case 8 : Hi <> Do */

        if ((USBD_MSC_GlobalVar->MSC_BOT_Info->cbw.bmFlags & 0x80) == 0x80)
        {
            SCSI_SenseCode(USBD_MSC_GlobalVar->USBD_SCSI_SenseInfo,USBD_MSC_GlobalVar->MSC_BOT_Info->cbw.bLUN, ILLEGAL_REQUEST, INVALID_CDB);
            return -1;
        }

        /* Check whether Media is ready */
        if(USBD_MSC_GlobalVar->USBD_STORAGE_fops->IsReady(lun) !=0 )
        {
            SCSI_SenseCode(USBD_MSC_GlobalVar->USBD_SCSI_SenseInfo,lun, NOT_READY, MEDIUM_NOT_PRESENT);
            return -1;
        }

        /* Check If media is write-protected */
        if(USBD_MSC_GlobalVar->USBD_STORAGE_fops->IsWriteProtected(lun) !=0 )
        {
            SCSI_SenseCode(USBD_MSC_GlobalVar->USBD_SCSI_SenseInfo,lun, NOT_READY, WRITE_PROTECTED);
            return -1;
        }


        USBD_MSC_GlobalVar->SCSI_BlkInfo->Addr = (params[2] << 24) | (params[3] << 16) | (params[4] <<  8) | params[5];
        USBD_MSC_GlobalVar->SCSI_BlkInfo->Len = (params[7] <<  8) | params[8];

        /* check if LBA address is in the right range */
        if(SCSI_CheckAddressRange(USBD_MSC_GlobalVar,lun, USBD_MSC_GlobalVar->SCSI_BlkInfo->Addr, USBD_MSC_GlobalVar->SCSI_BlkInfo->Len) < 0)
        {
            return -1; /* error */
        }

        USBD_MSC_GlobalVar->SCSI_BlkInfo->Addr *= USBD_MSC_GlobalVar->SCSI_BlkInfo->Size;
        USBD_MSC_GlobalVar->SCSI_BlkInfo->Len  *= USBD_MSC_GlobalVar->SCSI_BlkInfo->Size;

        /* cases 3,11,13 : Hn,Ho <> D0 */
        if (USBD_MSC_GlobalVar->MSC_BOT_Info->cbw.dDataLength != USBD_MSC_GlobalVar->SCSI_BlkInfo->Len)
        {
            SCSI_SenseCode(USBD_MSC_GlobalVar->USBD_SCSI_SenseInfo,USBD_MSC_GlobalVar->MSC_BOT_Info->cbw.bLUN, ILLEGAL_REQUEST, INVALID_CDB);
            return -1;
        }

        /* Prepare EP to receive first data packet */
        USBD_MSC_GlobalVar->MSC_BOT_Info->TaskState = BOT_DATA_OUT;
        USBDEV_EP_PrepareRx (pdev, MSC_OUT_EP, USBD_MSC_GlobalVar->MSC_BOT_Info->Data, MIN (USBD_MSC_GlobalVar->SCSI_BlkInfo->Len, USBD_MSC_GlobalVar->MSC_BOT_Info->DataBufMaxSize));
    }
    else /* Write Process ongoing */
    {
        return SCSI_ProcessWrite(pdev,USBD_MSC_GlobalVar,lun);
    }
    return 0;
}

/**
*\*\name   SCSI_Verify10.
*\*\fun    Process Verify10 command.
*\*\param  lun: Logical unit number
*\*\param  params: Command parameters
*\*\return status
**/
static int8_t SCSI_Verify10(USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar,uint8_t lun , uint8_t *params)
{
    if ((params[1]& 0x02) == 0x02)
    {
        SCSI_SenseCode (USBD_MSC_GlobalVar->USBD_SCSI_SenseInfo,lun, ILLEGAL_REQUEST, INVALID_FIELED_IN_COMMAND);
        return -1; /* Error, Verify Mode Not supported*/
    }

    USBD_MSC_GlobalVar->SCSI_BlkInfo->Addr = (params[2] << 24) | (params[3] << 16) | (params[4] <<  8) | params[5];
    USBD_MSC_GlobalVar->SCSI_BlkInfo->Len = (params[7] <<  8) | params[8];

    if(SCSI_CheckAddressRange(USBD_MSC_GlobalVar,lun, USBD_MSC_GlobalVar->SCSI_BlkInfo->Addr, USBD_MSC_GlobalVar->SCSI_BlkInfo->Len) < 0)
    {
        return -1; /* error */
    }
    USBD_MSC_GlobalVar->MSC_BOT_Info->DataLen = 0;
    return 0;
}

/**
*\*\name   SCSI_CheckAddressRange.
*\*\fun    Check address range.
*\*\param  lun: Logical unit number
*\*\param  blk_offset: first block address
*\*\param  blk_nbr: number of block to be processed
*\*\return status
**/
static int8_t SCSI_CheckAddressRange (USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar,uint8_t lun , uint32_t blk_offset , uint16_t blk_nbr)
{

    if ((blk_offset + blk_nbr) > USBD_MSC_GlobalVar->SCSI_BlkInfo->Num )
    {
        SCSI_SenseCode(USBD_MSC_GlobalVar->USBD_SCSI_SenseInfo,lun, ILLEGAL_REQUEST, ADDRESS_OUT_OF_RANGE);
        return -1;
    }
    return 0;
}

/**
*\*\name   SCSI_ProcessRead.
*\*\fun    Handle Read Process.
*\*\param  lun: Logical unit number
*\*\return status
**/
static int8_t SCSI_ProcessRead (USB_CORE_MODULE  *pdev,USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar,uint8_t lun)
{
    uint32_t len;

    len = MIN(USBD_MSC_GlobalVar->SCSI_BlkInfo->Len , USBD_MSC_GlobalVar->MSC_BOT_Info->DataBufMaxSize);

    if( USBD_MSC_GlobalVar->USBD_STORAGE_fops->Read(lun , USBD_MSC_GlobalVar->MSC_BOT_Info->Data, USBD_MSC_GlobalVar->SCSI_BlkInfo->Addr / USBD_MSC_GlobalVar->SCSI_BlkInfo->Size, len / USBD_MSC_GlobalVar->SCSI_BlkInfo->Size) < 0)
    {
        SCSI_SenseCode(USBD_MSC_GlobalVar->USBD_SCSI_SenseInfo,lun, HARDWARE_ERROR, UNRECOVERED_READ_ERROR);
        return -1;
    }

    USBDEV_EP_Tx (pdev, MSC_IN_EP, USBD_MSC_GlobalVar->MSC_BOT_Info->Data, len);

    USBD_MSC_GlobalVar->SCSI_BlkInfo->Addr   += len;
    USBD_MSC_GlobalVar->SCSI_BlkInfo->Len    -= len;

    /* case 6 : Hi = Di */
    USBD_MSC_GlobalVar->MSC_BOT_Info->csw.dDataResidue -= len;

    if (USBD_MSC_GlobalVar->SCSI_BlkInfo->Len == 0)
    {
        USBD_MSC_GlobalVar->MSC_BOT_Info->TaskState = BOT_LAST_DATA_IN;
    }
    return 0;
}

/**
*\*\name   SCSI_ProcessWrite.
*\*\fun    Handle Write Process.
*\*\param  lun: Logical unit number
*\*\return status
**/
static int8_t SCSI_ProcessWrite (USB_CORE_MODULE  *pdev,USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar,uint8_t lun)
{
    uint32_t len;

    len = MIN(USBD_MSC_GlobalVar->SCSI_BlkInfo->Len , USBD_MSC_GlobalVar->MSC_BOT_Info->DataBufMaxSize);

    if(USBD_MSC_GlobalVar->USBD_STORAGE_fops->Write(lun , USBD_MSC_GlobalVar->MSC_BOT_Info->Data, USBD_MSC_GlobalVar->SCSI_BlkInfo->Addr / USBD_MSC_GlobalVar->SCSI_BlkInfo->Size, len / USBD_MSC_GlobalVar->SCSI_BlkInfo->Size) < 0)
    {
        SCSI_SenseCode(USBD_MSC_GlobalVar->USBD_SCSI_SenseInfo,lun, HARDWARE_ERROR, WRITE_FAULT);
        return -1;
    }


    USBD_MSC_GlobalVar->SCSI_BlkInfo->Addr  += len;
    USBD_MSC_GlobalVar->SCSI_BlkInfo->Len   -= len;

    /* case 12 : Ho = Do */
    USBD_MSC_GlobalVar->MSC_BOT_Info->csw.dDataResidue -= len;

    if (USBD_MSC_GlobalVar->SCSI_BlkInfo->Len == 0)
    {
        MSC_BOT_SendCSW (pdev, USBD_MSC_GlobalVar,CSW_CMD_PASSED);
    }
    else
    {
        /* Prapare EP to Receive next packet */
        USBDEV_EP_PrepareRx (pdev, MSC_OUT_EP, USBD_MSC_GlobalVar->MSC_BOT_Info->Data, MIN (USBD_MSC_GlobalVar->SCSI_BlkInfo->Len, USBD_MSC_GlobalVar->MSC_BOT_Info->DataBufMaxSize));
    }

    return 0;
}
