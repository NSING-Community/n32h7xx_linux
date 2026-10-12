/**
 * @file usbd_desc.c
 * @author N32cube
 */

#include "usbd_desc.h"
/* NTFx CODE START */

#define USBD1_MANUFACTURER_STRING        "NSING"
#define USBD1_PRODUCT_STRING             "N32H787XIB7"
#define USBD1_CONFIGURATION_STRING       "Default_Config_Id"
#define USBD1_INTERFACE_STRING           "Default_Interface_Id"
#define USBD1_DEVICE_SERIAL0            (0x11223344)  
#define USBD1_DEVICE_SERIAL1            (0x55667788)
#define USBD1_DEVICE_SERIAL2            (0x12345678)

/* NTFx CODE END */

/* NTFx CODE START */
uint8_t *     USBD1_USER_DeviceDescriptor( uint8_t speed , uint16_t *length);
uint8_t *     USBD1_USER_LangIDStrDescriptor( uint8_t speed , uint16_t *length);
uint8_t *     USBD1_USER_ManufacturerStrDescriptor ( uint8_t speed , uint16_t *length);
uint8_t *     USBD1_USER_ProductStrDescriptor ( uint8_t speed , uint16_t *length);
uint8_t *     USBD1_USER_SerialStrDescriptor( uint8_t speed , uint16_t *length);
uint8_t *     USBD1_USER_ConfigStrDescriptor( uint8_t speed , uint16_t *length);
uint8_t *     USBD1_USER_InterfaceStrDescriptor( uint8_t speed , uint16_t *length);
#ifdef USB_SUPPORT_USER_STRING_DESC
uint8_t *     USBD_USER_USRStringDesc (uint8_t speed, uint8_t idx , uint16_t *length);  
#endif /* USB_SUPPORT_USER_STRING_DESC */  

static void Get_SerialNum(uint32_t SerivalNum0,uint32_t SerivalNum1,uint32_t SerivalNum2);
static void IntToUnicode(uint32_t value, uint8_t * pbuf, uint8_t len);
/* NTFx CODE END */

/* NTFx CODE START */
USBD_DEVICE_DESC USBD1_desc = 
{
    USBD1_USER_DeviceDescriptor,
    USBD1_USER_LangIDStrDescriptor,
    USBD1_USER_ManufacturerStrDescriptor,
    USBD1_USER_ProductStrDescriptor,
    USBD1_USER_SerialStrDescriptor,
    USBD1_USER_ConfigStrDescriptor,
    USBD1_USER_InterfaceStrDescriptor,
};
/* NTFx CODE END */

/* NTFx CODE START */
#ifdef USB_INTERNAL_DMA_ENABLED
#if defined ( __ICCARM__ )      /* !< IAR Compiler */
#pragma data_alignment=4
#endif
#endif                          /* USB_INTERNAL_DMA_ENABLED */
/* USB Standard Device Descriptor */
__ALIGN_BEGIN uint8_t USBD1_DeviceDesc[USB_SIZ_DEVICE_DESC] __ALIGN_END = 
{
    0x12,                         /* bLength */
    USB_DEVICE_DESCRIPTOR_TYPE,   /* bDescriptorType */
    0x00,                         /* bcdUSB */
    0x02,
    USB_DEVICE_CLASS_MASS_STORAGE, /*bDeviceClass*/
    0x00, /*bDeviceSubClass*/
    0x00, /*bDeviceProtocol*/
    USB_MAX_EP0_SIZE,         /* bMaxPacketSize */
    /* NSING company VID 0x19F5, the same one the NSLink probe uses. The PID is
     * the vendor library's own mass-storage ID (0x5780, see the MSC example's
     * USBD_PID), which keeps this off NSLink's 0x3106 and off the CDC/HID
     * example IDs. */
    0xF5, /*idVendor  = 0x19F5*/
    0x19,
    0x80, /*idProduct = 0x5780*/
    0x57,
    0x00,                         /* bcdDevice rel. 2.00 */
    0x02,
    USBD_IDX_MFC_STR,             /* Index of manufacturer string */
    USBD_IDX_PRODUCT_STR,         /* Index of product string */
    USBD_IDX_SERIAL_STR,          /* Index of serial number string */
    USBD_CFG_MAX_NUM              /* bNumConfigurations */
};                              /* USB_DeviceDescriptor */


#ifdef USB_INTERNAL_DMA_ENABLED
#if defined ( __ICCARM__ )      /* !< IAR Compiler */
#pragma data_alignment=4
#endif
#endif                          /* USB_INTERNAL_DMA_ENABLED */
/* USB Standard Device Descriptor */
__ALIGN_BEGIN uint8_t USBD1_LangIDDesc[USB_SIZ_STRING_LANGID] __ALIGN_END = 
{
    USB_SIZ_STRING_LANGID,
    USB_DESC_TYPE_STRING,
    0x04,
    0x08, // Chinese 

};

#ifdef USB_INTERNAL_DMA_ENABLED
#if defined ( __ICCARM__ )      /* !< IAR Compiler */
#pragma data_alignment=4
#endif
#endif                          /* USB_INTERNAL_DMA_ENABLED */
/* USB Standard Device Descriptor */
__ALIGN_BEGIN uint8_t USBD_DeviceQualifierDesc[USB_LEN_DEV_QUALIFIER_DESC]
  __ALIGN_END = 
{
    USB_LEN_DEV_QUALIFIER_DESC,
    USB_DESC_TYPE_DEVICE_QUALIFIER,
    
    0x00,
    0x02,
    0x00,
    0x00,
    0x00,
    0x40,
    0x01,
    0x00,
};

uint8_t USBD_StringSerial[USB_SIZ_STRING_SERIAL] = 
{
    USB_SIZ_STRING_SERIAL,
    USB_DESC_TYPE_STRING,
};

#ifdef USB_INTERNAL_DMA_ENABLED
#if defined ( __ICCARM__ )      /* !< IAR Compiler */
#pragma data_alignment=4
#endif
#endif                          /* USB_INTERNAL_DMA_ENABLED */
__ALIGN_BEGIN uint8_t USBD_StrDesc[USB_MAX_STR_DESC_SIZ] __ALIGN_END;

/* NTFx CODE END */

/* NTFx CODE START */
/**
*\*\name   USBD1_USER_DeviceDescriptor.
*\*\fun    return the device descriptor.
*\*\param  speed : current device speed
*\*\param  length : pointer to data length variable
*\*\return pointer to descriptor buffer
*/
uint8_t *USBD1_USER_DeviceDescriptor(uint8_t speed, uint16_t * length)
{
    *length = sizeof(USBD1_DeviceDesc);
    return (uint8_t *) USBD1_DeviceDesc;
}

/**
*\*\name   USBD1_USER_LangIDStrDescriptor.
*\*\fun    return the LangID string descriptor.
*\*\param  speed : current device speed
*\*\param  length : pointer to data length variable
*\*\return pointer to descriptor buffer
*/
uint8_t *USBD1_USER_LangIDStrDescriptor(uint8_t speed, uint16_t * length)
{
    *length = sizeof(USBD1_LangIDDesc);
    return (uint8_t *) USBD1_LangIDDesc;
}

/**
*\*\name   USBD1_USER_ProductStrDescriptor.
*\*\fun    return the product string descriptor.
*\*\param  speed : current device speed
*\*\param  length : pointer to data length variable
*\*\return pointer to descriptor buffer
*/
uint8_t *USBD1_USER_ProductStrDescriptor(uint8_t speed, uint16_t * length)
{
    if (speed == USB_SPEED_HIGH)
    {
        USBD_GetString((uint8_t *) (uint8_t *) USBD1_PRODUCT_STRING, USBD_StrDesc, length);
    }
    else
    {
        USBD_GetString((uint8_t *) (uint8_t *) USBD1_PRODUCT_STRING, USBD_StrDesc, length);
    }
    return USBD_StrDesc;
}

/**
*\*\name   USBD1_USER_ManufacturerStrDescriptor.
*\*\fun    return the manufacturer string descriptor.
*\*\param  speed : current device speed
*\*\param  length : pointer to data length variable
*\*\return pointer to descriptor buffer
*/
uint8_t *USBD1_USER_ManufacturerStrDescriptor(uint8_t speed, uint16_t * length)
{
    USBD_GetString((uint8_t *) (uint8_t *) USBD1_MANUFACTURER_STRING, USBD_StrDesc, length);
    return USBD_StrDesc;
}

/**
*\*\name   USBD1_USER_SerialStrDescriptor.
*\*\fun    return the serial number string descriptor.
*\*\param  speed : current device speed
*\*\param  length : pointer to data length variable
*\*\return pointer to descriptor buffer
*/
uint8_t *USBD1_USER_SerialStrDescriptor(uint8_t speed, uint16_t * length)
{
    *length = USB_SIZ_STRING_SERIAL;

    /* Update the serial number string descriptor with the data from the unique * ID */
    Get_SerialNum(USBD1_DEVICE_SERIAL0,USBD1_DEVICE_SERIAL1,USBD1_DEVICE_SERIAL2);

    return (uint8_t *) USBD_StringSerial;
}

/**
*\*\name   USBD1_USER_ConfigStrDescriptor.
*\*\fun    return the configuration string descriptor.
*\*\param  speed : current device speed
*\*\param  length : pointer to data length variable
*\*\return pointer to descriptor buffer
*/
uint8_t *USBD1_USER_ConfigStrDescriptor(uint8_t speed, uint16_t * length)
{
    if (speed == USB_SPEED_HIGH)
    {
        USBD_GetString((uint8_t *) (uint8_t *) USBD1_CONFIGURATION_STRING, USBD_StrDesc, length);
    }
    else
    {
        USBD_GetString((uint8_t *) (uint8_t *) USBD1_CONFIGURATION_STRING, USBD_StrDesc, length);
    }
    return USBD_StrDesc;
}

/**
*\*\name   USBD1_USER_InterfaceStrDescriptor.
*\*\fun    return the interface string descriptor.
*\*\param  speed : current device speed
*\*\param  length : pointer to data length variable
*\*\return pointer to descriptor buffer
*/
uint8_t *USBD1_USER_InterfaceStrDescriptor(uint8_t speed, uint16_t * length)
{
    if (speed == 0)
    {
        USBD_GetString((uint8_t *) (uint8_t *) USBD1_INTERFACE_STRING, USBD_StrDesc, length);
    }
    else
    {
        USBD_GetString((uint8_t *) (uint8_t *) USBD1_INTERFACE_STRING, USBD_StrDesc, length);
    }
    return USBD_StrDesc;
}
/**
*\*\name   Get_SerialNum.
*\*\fun    Create the serial number string descriptor .
*\*\param  none
*\*\param  none
*\*\return pointer to descriptor buffer
*/
static void Get_SerialNum(uint32_t SerivalNum0,uint32_t SerivalNum1,uint32_t SerivalNum2)
{
    volatile uint32_t deviceserial0, deviceserial1, deviceserial2;

    deviceserial0 = SerivalNum1;
    deviceserial1 = SerivalNum1;
    deviceserial2 = SerivalNum1;

    deviceserial0 += deviceserial2;

    if (deviceserial0 != 0)
    {
        IntToUnicode(deviceserial0, &USBD_StringSerial[2], 8);
        IntToUnicode(deviceserial1, &USBD_StringSerial[18], 4);
    }
	
	/* Add a volatile opration, prevent code optimization */
    volatile uint8_t dummy = USBD_StringSerial[0];
	/* Avoid unused variable warnings */
    (void)dummy; 
}

/**
*\*\name   IntToUnicode.
*\*\fun    Convert Hex 32Bits value into char.
*\*\param  value: value to convert
*\*\param  pbuf: pointer to the buffer 
*\*\param  len: buffer length
*\*\return none.
*/
static void IntToUnicode(uint32_t value, uint8_t * pbuf, uint8_t len)
{
    uint8_t idx = 0;

    for (idx = 0; idx < len; idx++)
    {
        if (((value >> 28)) < 0xA)
        {
            pbuf[2 * idx] = (value >> 28) + '0';
        }
        else
        {
            pbuf[2 * idx] = (value >> 28) + 'A' - 10;
        }
        value = value << 4;
        pbuf[2 * idx + 1] = 0;
    }
}
/* NTFx CODE END */

