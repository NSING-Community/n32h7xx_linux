/**
 * @file usbd_desc.h
 * @author N32cube
 */
#ifndef __USB_DESC_H
#define __USB_DESC_H

#include "usbd_req.h"
/* NTFx CODE START */
#define USB_DEVICE_DESCRIPTOR_TYPE              0x01
#define USB_CONFIGURATION_DESCRIPTOR_TYPE       0x02
#define USB_STRING_DESCRIPTOR_TYPE              0x03
#define USB_INTERFACE_DESCRIPTOR_TYPE           0x04
#define USB_ENDPOINT_DESCRIPTOR_TYPE            0x05
#define USB_SIZ_DEVICE_DESC                     18
#define USB_SIZ_STRING_LANGID                   4

#define  USB_SIZ_STRING_SERIAL       0x1A

#define USB_DEVICE_CLASS_AUDIO                        0x01
#define USB_DEVICE_CLASS_CDC                          0x02
#define USB_DEVICE_CLASS_HID                          0x03
#define USB_DEVICE_CLASS_IMAGE                        0x06
#define USB_DEVICE_CLASS_PRINTER                      0x07
#define USB_DEVICE_CLASS_MASS_STORAGE                 0x08

/* NTFx CODE END */

/* NTFx CODE START */
extern uint8_t USBD_DeviceQualifierDesc[USB_LEN_DEV_QUALIFIER_DESC];
extern  USBD_DEVICE_DESC USBD1_desc; 

/* NTFx CODE END */

/**
  * @}
  */ 

#endif /* __USBD_DESC_H */
