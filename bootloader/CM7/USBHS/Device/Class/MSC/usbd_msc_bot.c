/**
 * @file usbd_msc_bot.c
 * @author N32cube
 */
#include "usbd_msc_bot.h"
#include "usbd_msc_scsi.h"
#include "usbd_ioreq.h"
/* NTFx CODE START */

#ifdef USB_INTERNAL_DMA_ENABLED
  #if defined ( __ICCARM__ ) /*!< IAR Compiler */
    #pragma data_alignment=4   
  #endif
#endif /* USB_INTERNAL_DMA_ENABLED */
__ALIGN_BEGIN uint8_t USBD1_MSC_BOT_Data[USBD1_MSC_MEDIA_PACKET] __ALIGN_END ;


#ifdef USB_INTERNAL_DMA_ENABLED
  #if defined ( __ICCARM__ ) /*!< IAR Compiler */
    #pragma data_alignment=4   
  #endif
#endif /* USB_INTERNAL_DMA_ENABLED */
__ALIGN_BEGIN MSC_BOT_Info_TypeDef  USBD1_MSC_BOT_Info __ALIGN_END ={
	  .Data=USBD1_MSC_BOT_Data,
    .DataBufMaxSize=USBD1_MSC_MEDIA_PACKET,
};
/* NTFx CODE END */

static void MSC_BOT_CBW_Decode (USB_CORE_MODULE  *pdev,USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar);

static void  MSC_BOT_SendData(USB_CORE_MODULE  *pdev,USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar);

static void MSC_BOT_Abort(USB_CORE_MODULE  *pdev,USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar);

/**
*\*\name   MSC_BOT_Init.
*\*\fun    Initialize the BOT Process.
*\*\param  pdev : USB device
*\*\return none
**/
void MSC_BOT_Init (USB_CORE_MODULE  *pdev,USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar)
{
    USBD_MSC_GlobalVar->MSC_BOT_Info->TaskState = BOT_IDLE;
    USBD_MSC_GlobalVar->MSC_BOT_Info->Status = BOT_STATE_NORMAL;
    USBD_MSC_GlobalVar->USBD_STORAGE_fops->Init(0);

    USBDEV_EP_Flush(pdev, MSC_OUT_EP);
    USBDEV_EP_Flush(pdev, MSC_IN_EP);
    /* Prapare EP to Receive First BOT Cmd */
    USBDEV_EP_PrepareRx (pdev, MSC_OUT_EP, (uint8_t *)&USBD_MSC_GlobalVar->MSC_BOT_Info->cbw, BOT_CBW_LENGTH);
}

/**
*\*\name   MSC_BOT_Reset.
*\*\fun    Reset the BOT Machine.
*\*\param  pdev : USB device
*\*\return none
**/
void MSC_BOT_Reset (USB_CORE_MODULE  *pdev,USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar)
{
    USBD_MSC_GlobalVar->MSC_BOT_Info->TaskState = BOT_IDLE;
    USBD_MSC_GlobalVar->MSC_BOT_Info->Status = BOT_STATE_RECOVERY;
    /* Prapare EP to Receive First BOT Cmd */
    USBDEV_EP_PrepareRx (pdev, MSC_OUT_EP, (uint8_t *)&USBD_MSC_GlobalVar->MSC_BOT_Info->cbw, BOT_CBW_LENGTH);
}

/**
*\*\name   MSC_BOT_DeInit.
*\*\fun    Uninitialize the BOT Machine.
*\*\param  pdev : USB device
*\*\return none
**/
void MSC_BOT_DeInit (USB_CORE_MODULE  *pdev,USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar)
{
    USBD_MSC_GlobalVar->MSC_BOT_Info->TaskState = BOT_IDLE;
}

/**
*\*\name   MSC_BOT_DataIn.
*\*\fun    Handle BOT IN data stage.
*\*\param  pdev : USB device
*\*\param  epnum: endpoint index
*\*\return none
**/
void MSC_BOT_DataIn (USB_CORE_MODULE  *pdev, uint8_t epnum,USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar)
{
    switch (USBD_MSC_GlobalVar->MSC_BOT_Info->TaskState)
    {
        case BOT_DATA_IN:
            if(SCSI_ProcessCmd(pdev, USBD_MSC_GlobalVar,USBD_MSC_GlobalVar->MSC_BOT_Info->cbw.bLUN, &USBD_MSC_GlobalVar->MSC_BOT_Info->cbw.CB[0]) < 0)
            {
                MSC_BOT_SendCSW (pdev,USBD_MSC_GlobalVar, CSW_CMD_FAILED);
            }
            break;

        case BOT_SEND_DATA:
        case BOT_LAST_DATA_IN:
            MSC_BOT_SendCSW (pdev,USBD_MSC_GlobalVar, CSW_CMD_PASSED);
            break;

        default:
            break;
    }
}

/**
*\*\name   MSC_BOT_DataOut.
*\*\fun    Handle BOT OUT data stage.
*\*\param  pdev : USB device
*\*\param  epnum: endpoint index
*\*\return none
**/
void MSC_BOT_DataOut (USB_CORE_MODULE  *pdev, uint8_t epnum,USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar)
{
    switch (USBD_MSC_GlobalVar->MSC_BOT_Info->TaskState)
    {
        case BOT_IDLE:
            MSC_BOT_CBW_Decode(pdev,USBD_MSC_GlobalVar);
            break;

        case BOT_DATA_OUT:
            if(SCSI_ProcessCmd(pdev,USBD_MSC_GlobalVar, USBD_MSC_GlobalVar->MSC_BOT_Info->cbw.bLUN, &USBD_MSC_GlobalVar->MSC_BOT_Info->cbw.CB[0]) < 0)
            {
                MSC_BOT_SendCSW (pdev,USBD_MSC_GlobalVar, CSW_CMD_FAILED);
            }
            break;

        default:
            break;
    }
}

/**
*\*\name   MSC_BOT_CBW_Decode.
*\*\fun    Decode the CBW command and set the BOT state machine accordingtly.
*\*\param  pdev : USB device
*\*\return none
**/
static void  MSC_BOT_CBW_Decode (USB_CORE_MODULE  *pdev,USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar)
{

    USBD_MSC_GlobalVar->MSC_BOT_Info->csw.dTag = USBD_MSC_GlobalVar->MSC_BOT_Info->cbw.dTag;
    USBD_MSC_GlobalVar->MSC_BOT_Info->csw.dDataResidue = USBD_MSC_GlobalVar->MSC_BOT_Info->cbw.dDataLength;

    if ((USBD_GetRxCount (pdev ,MSC_OUT_EP) != BOT_CBW_LENGTH) ||
        (USBD_MSC_GlobalVar->MSC_BOT_Info->cbw.dSignature != BOT_CBW_SIGNATURE)||
        (USBD_MSC_GlobalVar->MSC_BOT_Info->cbw.bLUN > 1) || 
        (USBD_MSC_GlobalVar->MSC_BOT_Info->cbw.bCBLength < 1) || 
        (USBD_MSC_GlobalVar->MSC_BOT_Info->cbw.bCBLength > 16))
    {
        SCSI_SenseCode(USBD_MSC_GlobalVar->USBD_SCSI_SenseInfo,USBD_MSC_GlobalVar->MSC_BOT_Info->cbw.bLUN, 
        ILLEGAL_REQUEST, 
        INVALID_CDB);
        USBD_MSC_GlobalVar->MSC_BOT_Info->Status = BOT_STATE_ERROR;   
        MSC_BOT_Abort(pdev,USBD_MSC_GlobalVar);
    }
    else
    {
        if(SCSI_ProcessCmd(pdev,USBD_MSC_GlobalVar, USBD_MSC_GlobalVar->MSC_BOT_Info->cbw.bLUN, &USBD_MSC_GlobalVar->MSC_BOT_Info->cbw.CB[0]) < 0)
        {
            MSC_BOT_Abort(pdev,USBD_MSC_GlobalVar);
        }
        /*Burst xfer handled internally*/
        else if ((USBD_MSC_GlobalVar->MSC_BOT_Info->TaskState != BOT_DATA_IN) && (USBD_MSC_GlobalVar->MSC_BOT_Info->TaskState != BOT_DATA_OUT) && (USBD_MSC_GlobalVar->MSC_BOT_Info->TaskState != BOT_LAST_DATA_IN)) 
        {
            if (USBD_MSC_GlobalVar->MSC_BOT_Info->DataLen > 0)
            {
                MSC_BOT_SendData(pdev,USBD_MSC_GlobalVar);
            }
            else if (USBD_MSC_GlobalVar->MSC_BOT_Info->DataLen == 0) 
            {
                MSC_BOT_SendCSW (pdev,USBD_MSC_GlobalVar, CSW_CMD_PASSED);
            }
        }
    }
}


/**
*\*\name   MSC_BOT_SendData.
*\*\fun    Send the requested data.
*\*\param  pdev : USB device
*\*\param  buf: pointer to data buffer
*\*\param  len: Data Length
*\*\return none
**/
static void  MSC_BOT_SendData(USB_CORE_MODULE  *pdev,USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar)
{
    uint32_t len = MIN (USBD_MSC_GlobalVar->MSC_BOT_Info->cbw.dDataLength, USBD_MSC_GlobalVar->MSC_BOT_Info->DataLen);
    USBD_MSC_GlobalVar->MSC_BOT_Info->csw.dDataResidue -= len;
    USBD_MSC_GlobalVar->MSC_BOT_Info->csw.bStatus = CSW_CMD_PASSED;
    USBD_MSC_GlobalVar->MSC_BOT_Info->TaskState = BOT_SEND_DATA;

    USBDEV_EP_Tx(pdev, MSC_IN_EP, USBD_MSC_GlobalVar->MSC_BOT_Info->Data, len);
}

/**
*\*\name   MSC_BOT_SendCSW.
*\*\fun    Send the Command Status Wrapper.
*\*\param  pdev : USB device
*\*\param  CSW_Status : CSW status
*\*\return none
**/
void  MSC_BOT_SendCSW (USB_CORE_MODULE  *pdev,USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar, uint8_t CSW_Status)
{
    USBD_MSC_GlobalVar->MSC_BOT_Info->csw.dSignature = BOT_CSW_SIGNATURE;
    USBD_MSC_GlobalVar->MSC_BOT_Info->csw.bStatus = CSW_Status;
    USBD_MSC_GlobalVar->MSC_BOT_Info->TaskState = BOT_IDLE;

    USBDEV_EP_Tx (pdev, MSC_IN_EP, (uint8_t *)&USBD_MSC_GlobalVar->MSC_BOT_Info->csw, BOT_CSW_LENGTH);

    /* Prepare EP to Receive next Cmd */
    USBDEV_EP_PrepareRx (pdev, MSC_OUT_EP, (uint8_t *)&USBD_MSC_GlobalVar->MSC_BOT_Info->cbw, BOT_CBW_LENGTH);  
  
}

/**
*\*\name   MSC_BOT_Abort.
*\*\fun    Abort the current transfer.
*\*\param  pdev : USB device
*\*\return none
**/
static void  MSC_BOT_Abort (USB_CORE_MODULE  *pdev,USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar)
{
    if ((USBD_MSC_GlobalVar->MSC_BOT_Info->cbw.bmFlags == 0) && (USBD_MSC_GlobalVar->MSC_BOT_Info->cbw.dDataLength != 0) && (USBD_MSC_GlobalVar->MSC_BOT_Info->Status == BOT_STATE_NORMAL))
    {
        USBDEV_EP_Stall(pdev, MSC_OUT_EP);
    }
    USBDEV_EP_Stall(pdev, MSC_IN_EP);

    if(USBD_MSC_GlobalVar->MSC_BOT_Info->Status == BOT_STATE_ERROR)
    {
        USBDEV_EP_PrepareRx (pdev, MSC_OUT_EP, (uint8_t *)&USBD_MSC_GlobalVar->MSC_BOT_Info->cbw, BOT_CBW_LENGTH);    
    }
}

/**
*\*\name   MSC_BOT_CplClrFeature.
*\*\fun    Complete the clear feature request.
*\*\param  pdev : USB device
*\*\param  epnum: endpoint index
*\*\return none
**/
void  MSC_BOT_CplClrFeature (USB_CORE_MODULE  *pdev,USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar, uint8_t epnum)
{
    if(USBD_MSC_GlobalVar->MSC_BOT_Info->Status == BOT_STATE_ERROR )/* Bad CBW Signature */
    {
        USBDEV_EP_Stall(pdev, MSC_IN_EP);
        USBD_MSC_GlobalVar->MSC_BOT_Info->Status = BOT_STATE_NORMAL;    
    }
    else if(((epnum & 0x80) == 0x80) && ( USBD_MSC_GlobalVar->MSC_BOT_Info->Status != BOT_STATE_RECOVERY))
    {
        MSC_BOT_SendCSW (pdev,USBD_MSC_GlobalVar, CSW_CMD_FAILED);
    }
}
