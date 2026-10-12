/**
 * @file usbd_msc_core.c
 * @author N32cube
 */
#include "usbd_msc_core.h"
#include "usbd_msc_bot.h"
#include "usbd_msc_scsi.h"
#include "usbd_req.h"
/* NTFx CODE START */
uint8_t  USBD1_MSC_Init (void  *pdev, uint8_t cfgidx);
uint8_t  USBD1_MSC_DeInit (void  *pdev, uint8_t cfgidx);
uint8_t  USBD1_MSC_Setup (void  *pdev, USB_SETUP_REQ *req);
uint8_t  USBD1_MSC_DataIn (void  *pdev, uint8_t epnum);
uint8_t  USBD1_MSC_DataOut (void  *pdev, uint8_t epnum);
uint8_t  USBD_MSC_Init (void  *pdev, uint8_t cfgidx,USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar);
uint8_t  USBD_MSC_DeInit (void  *pdev, uint8_t cfgidx,USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar);
uint8_t  USBD_MSC_Setup (void  *pdev, USB_SETUP_REQ *req,USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar,uint8_t *USBD_MSC_AltSet);
uint8_t  *USBD_MSC_GetCfgDesc (uint8_t speed, uint16_t *length);
uint8_t  *USBD_MSC_GetOtherCfgDesc (uint8_t speed, uint16_t *length);

/* NTFx CODE END */

/* NTFx CODE START */
USBD_Class_cb_TypeDef  USBD1_MSC_cb = 
{
    USBD1_MSC_Init,
    USBD1_MSC_DeInit,
    USBD1_MSC_Setup,
    NULL, /*EP0_TxSent*/
    NULL, /*EP0_RxReady*/
    USBD1_MSC_DataIn,
    USBD1_MSC_DataOut,
    NULL, /*SOF */ 
    NULL,
    NULL,
    USBD_MSC_GetCfgDesc,
    USBD_MSC_GetOtherCfgDesc,
};
/* NTFx CODE END */

/* NTFx CODE START */
#ifdef USB_INTERNAL_DMA_ENABLED
  #if defined ( __ICCARM__ ) /*!< IAR Compiler */
    #pragma data_alignment=4   
  #endif
#endif /* USB_INTERNAL_DMA_ENABLED */
__ALIGN_BEGIN uint8_t  USBD1_MSC_AltSet  __ALIGN_END = 0;

#ifdef USB_INTERNAL_DMA_ENABLED
  #if defined ( __ICCARM__ ) /*!< IAR Compiler */
    #pragma data_alignment=4   
  #endif
#endif /* USB_INTERNAL_DMA_ENABLED */
__ALIGN_BEGIN USBD_MSC_GlobalVar_Info USBD1_MSC_GlobalVar  __ALIGN_END={
    .MSC_BOT_Info=&USBD1_MSC_BOT_Info,
    .SCSI_BlkInfo=&USBD1_SCSI_BlkInfo,
    .USBD_STORAGE_fops=&USBD1_STORAGE_fops,
    .USBD_SCSI_SenseInfo=&USBD1_SCSI_SenseInfo,

};
/* NTFx CODE END */

#ifdef USB_INTERNAL_DMA_ENABLED
  #if defined ( __ICCARM__ ) /*!< IAR Compiler */
    #pragma data_alignment=4   
  #endif
#endif /* USB_INTERNAL_DMA_ENABLED */
/* USB Mass storage device Configuration Descriptor */
/*   All Descriptors (Configuration, Interface, Endpoint, Class, Vendor */
__ALIGN_BEGIN uint8_t USBD_MSC_CfgDesc[USB_MSC_CONFIG_DESC_SIZ] __ALIGN_END =
{
    0x09,   /* bLength: Configuation Descriptor size */
    USB_DESC_TYPE_CONFIGURATION,   /* bDescriptorType: Configuration */
    USB_MSC_CONFIG_DESC_SIZ,
    0x00,
    0x01,   /* bNumInterfaces: 1 interface */
    0x01,   /* bConfigurationValue: */
    0x04,   /* iConfiguration: */
    0xC0,   /* bmAttributes: */
    0x32,   /* MaxPower 100 mA */

    /********************  Mass Storage interface ********************/
    0x09,   /* bLength: Interface Descriptor size */
    0x04,   /* bDescriptorType: */
    0x00,   /* bInterfaceNumber: Number of Interface */
    0x00,   /* bAlternateSetting: Alternate setting */
    0x02,   /* bNumEndpoints*/
    0x08,   /* bInterfaceClass: MSC Class */
    0x06,   /* bInterfaceSubClass : SCSI transparent*/
    0x50,   /* nInterfaceProtocol */
    0x05,   /* iInterface: */
    /********************  Mass Storage Endpoints ********************/
    0x07,   /*Endpoint descriptor length = 7*/
    0x05,   /*Endpoint descriptor type */
    MSC_IN_EP,   /*Endpoint address (IN, address 1) */
    0x02,   /*Bulk endpoint type */
    LOBYTE(MSC_MAX_PACKET),
    HIBYTE(MSC_MAX_PACKET),
    0x00,   /*Polling interval in milliseconds */

    0x07,   /*Endpoint descriptor length = 7 */
    0x05,   /*Endpoint descriptor type */
    MSC_OUT_EP,   /*Endpoint address (OUT, address 1) */
    0x02,   /*Bulk endpoint type */
    LOBYTE(MSC_MAX_PACKET),
    HIBYTE(MSC_MAX_PACKET),
    0x00    /*Polling interval in milliseconds*/
};

 #ifdef USB_INTERNAL_DMA_ENABLED
   #if defined ( __ICCARM__ ) /*!< IAR Compiler */
     #pragma data_alignment=4   
   #endif
 #endif /* USB_INTERNAL_DMA_ENABLED */
__ALIGN_BEGIN uint8_t USBD_MSC_OtherCfgDesc[USB_MSC_CONFIG_DESC_SIZ] __ALIGN_END =
{
    0x09,   /* bLength: Configuation Descriptor size */
    USB_DESC_TYPE_OTHER_SPEED_CONFIGURATION,   
    USB_MSC_CONFIG_DESC_SIZ,

    0x00,
    0x01,   /* bNumInterfaces: 1 interface */
    0x01,   /* bConfigurationValue: */
    0x04,   /* iConfiguration: */
    0xC0,   /* bmAttributes: */
    0x32,   /* MaxPower 100 mA */

    /********************  Mass Storage interface ********************/
    0x09,   /* bLength: Interface Descriptor size */
    0x04,   /* bDescriptorType: */
    0x00,   /* bInterfaceNumber: Number of Interface */
    0x00,   /* bAlternateSetting: Alternate setting */
    0x02,   /* bNumEndpoints*/
    0x08,   /* bInterfaceClass: MSC Class */
    0x06,   /* bInterfaceSubClass : SCSI transparent command set*/
    0x50,   /* nInterfaceProtocol */
    0x05,          /* iInterface: */
    /********************  Mass Storage Endpoints ********************/
    0x07,   /*Endpoint descriptor length = 7*/
    0x05,   /*Endpoint descriptor type */
    MSC_IN_EP,   /*Endpoint address (IN, address 1) */
    0x02,   /*Bulk endpoint type */
    0x40,
    0x00,
    0x00,   /*Polling interval in milliseconds */

    0x07,   /*Endpoint descriptor length = 7 */
    0x05,   /*Endpoint descriptor type */
    MSC_OUT_EP,   /*Endpoint address (OUT, address 1) */
    0x02,   /*Bulk endpoint type */
    0x40,
    0x00,
    0x00     /*Polling interval in milliseconds*/
};
/* NTFx CODE START */
/**
*\*\name   USBD_MSC_Init.
*\*\fun    Initialize  the mass storage configuration.
*\*\param  pdev : USB device
*\*\param  cfgidx: configuration index
*\*\return status
**/
uint8_t  USBD_MSC_Init (void  *pdev, uint8_t cfgidx,USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar)
{
/* NTFx CODE END */
    USBD_MSC_DeInit(pdev , cfgidx,USBD_MSC_GlobalVar);

    /* Open EP IN */
    USBDEV_EP_Open(pdev, MSC_IN_EP, MSC_EPIN_SIZE, USB_EP_BULK);

    /* Open EP OUT */
    USBDEV_EP_Open(pdev, MSC_OUT_EP, MSC_EPOUT_SIZE, USB_EP_BULK);

    /* Init the BOT  layer */
    MSC_BOT_Init(pdev,USBD_MSC_GlobalVar); 
/* NTFx CODE START */
    return USBD_OK;
}

/**
*\*\name   USBD_MSC_DeInit.
*\*\fun    DeInitilaize  the mass storage configuration.
*\*\param  pdev : USB device
*\*\param  cfgidx: configuration index
*\*\return status
**/
uint8_t  USBD_MSC_DeInit (void  *pdev, uint8_t cfgidx,USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar)
{
/* NTFx CODE END */
  /* Close MSC EPs */
  USBDEV_EP_Close (pdev , MSC_IN_EP);
  USBDEV_EP_Close (pdev , MSC_OUT_EP);
  
  /* Un Init the BOT layer */
  MSC_BOT_DeInit(pdev,USBD_MSC_GlobalVar);
/* NTFx CODE START */
  return USBD_OK;
}

/**
*\*\name   USBD_MSC_Setup.
*\*\fun    Handle the MSC specific requests.
*\*\param  pdev : USB device
*\*\param  req: USB request
*\*\return status
**/
uint8_t  USBD_MSC_Setup (void  *pdev, USB_SETUP_REQ *req,USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar,uint8_t *USBD_MSC_AltSet)
{
/* NTFx CODE END */
    uint8_t USBD_MSC_MaxLun;
    switch (req->bmRequest & USB_REQ_TYPE_MASK)
    {

        /* Class request */
        case USB_REQ_TYPE_CLASS :
            switch (req->bRequest)
            {
                case BOT_GET_MAX_LUN :
                    if((req->wValue  == 0) && (req->wLength == 1) && ((req->bmRequest & 0x80) == 0x80))
                    {
                        USBD_MSC_MaxLun = USBD_MSC_GlobalVar->USBD_STORAGE_fops->GetMaxLun();
                        if(USBD_MSC_MaxLun > 0)
                        {
                            USBD_CtrlSendData (pdev, &USBD_MSC_MaxLun, 1);
                        }
                        else
                        {
                            USBD_CtrlError(pdev , req);
                            return USBD_FAIL; 
                        }
                    }
                    else
                    {
                        USBD_CtrlError(pdev , req);
                        return USBD_FAIL; 
                    }
                    break;

                case BOT_RESET :
                    if((req->wValue  == 0) && (req->wLength == 0) && ((req->bmRequest & 0x80) != 0x80))
                    {
                        MSC_BOT_Reset(pdev,USBD_MSC_GlobalVar);
                    }
                    else
                    {
                        USBD_CtrlError(pdev , req);
                        return USBD_FAIL; 
                    }
                    break;

                default:
                    USBD_CtrlError(pdev , req);
                    return USBD_FAIL; 
            }
            break;
            
        /* Interface & Endpoint request */
        case USB_REQ_TYPE_STANDARD:
            switch (req->bRequest)
            {
                case USB_REQ_GET_INTERFACE :
                    USBD_CtrlSendData (pdev, USBD_MSC_AltSet, 1);
                    break;

                case USB_REQ_SET_INTERFACE :
                    *USBD_MSC_AltSet = (uint8_t)(req->wValue);
                    break;

                case USB_REQ_CLEAR_FEATURE:  

                    /* Flush the FIFO and Clear the stall status */    
                    USBDEV_EP_Flush(pdev, (uint8_t)req->wIndex);

                    /* Re-activate the EP */      
                    USBDEV_EP_Close (pdev , (uint8_t)req->wIndex);
                    if((((uint8_t)req->wIndex) & 0x80) == 0x80)
                    {
                        USBDEV_EP_Open(pdev, ((uint8_t)req->wIndex), MSC_EPIN_SIZE, USB_EP_BULK);
                    }
                    else
                    {
                        USBDEV_EP_Open(pdev, ((uint8_t)req->wIndex), MSC_EPOUT_SIZE, USB_EP_BULK);
                    }
                    /* Handle BOT error */
                    MSC_BOT_CplClrFeature(pdev,USBD_MSC_GlobalVar, (uint8_t)req->wIndex);
                    break;

                }  
                break;

        default:
            break;
    }
/* NTFx CODE START */
    return USBD_OK;
}
/**
*\*\name   USBD_MSC_GetCfgDesc.
*\*\fun    return configuration descriptor.
*\*\param  speed: current device speed
*\*\param  length: pointer data length
*\*\return pointer to descriptor buffer
**/
uint8_t  *USBD_MSC_GetCfgDesc (uint8_t speed, uint16_t *length)
{
    *length = sizeof (USBD_MSC_CfgDesc);
    return USBD_MSC_CfgDesc;
}

/**
*\*\name   USBD_MSC_GetOtherCfgDesc.
*\*\fun    return configuration descriptor.
*\*\param  speed: current device speed
*\*\param  length: pointer data length
*\*\return pointer to descriptor buffer
**/
uint8_t  *USBD_MSC_GetOtherCfgDesc (uint8_t speed, uint16_t *length)
{
    *length = sizeof (USBD_MSC_OtherCfgDesc);
    return USBD_MSC_OtherCfgDesc;
}
/**
*\*\name   USBD1_MSC_DataIn.
*\*\fun    Handle data IN Stage.
*\*\param  pdev : USB device
*\*\param  epnum: endpoint index
*\*\return status
**/
uint8_t  USBD1_MSC_DataIn (void  *pdev, uint8_t epnum)
{
    MSC_BOT_DataIn(pdev , epnum,&USBD1_MSC_GlobalVar);
    return USBD_OK;
}

/**
*\*\name   USBD1_MSC_DataOut.
*\*\fun    Handle data OUT Stage.
*\*\param  pdev : USB device
*\*\param  epnum: endpoint index
*\*\return status
**/
uint8_t  USBD1_MSC_DataOut (void  *pdev, uint8_t epnum)
{
    MSC_BOT_DataOut(pdev , epnum,&USBD1_MSC_GlobalVar);
    return USBD_OK;
}

/**
*\*\name   USBD1_MSC_Init.
*\*\fun    Initialize  the mass storage configuration.
*\*\param  pdev : USB device
*\*\param  cfgidx: configuration index
*\*\return status
**/
uint8_t  USBD1_MSC_Init (void  *pdev, uint8_t cfgidx)
{

    return USBD_MSC_Init (pdev, cfgidx,&USBD1_MSC_GlobalVar);
}
/**
*\*\name   USBD1_MSC_DeInit.
*\*\fun    DeInitilaize  the mass storage configuration.
*\*\param  pdev : USB device
*\*\param  cfgidx: configuration index
*\*\return status
**/
uint8_t  USBD1_MSC_DeInit (void  *pdev, uint8_t cfgidx)
{
    return USBD_MSC_DeInit (pdev, cfgidx,&USBD1_MSC_GlobalVar);
}
/**
*\*\name   USBD1_MSC_Setup.
*\*\fun    Handle the MSC specific requests.
*\*\param  pdev : USB device
*\*\param  req: USB request
*\*\return status
**/
uint8_t  USBD1_MSC_Setup (void  *pdev, USB_SETUP_REQ *req)
{
    return USBD_MSC_Setup (pdev, req,&USBD1_MSC_GlobalVar,&USBD1_MSC_AltSet);
}

/* NTFx CODE END */

