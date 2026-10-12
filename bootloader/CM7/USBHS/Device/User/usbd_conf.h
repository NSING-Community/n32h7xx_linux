/**
 * @file usbd_conf.h
 * @author N32cube
 */
#ifndef __USBD_CONF__H__
#define __USBD_CONF__H__

/* Includes ------------------------------------------------------------------*/
#include "usbhs_conf.h"
/* NTFx CODE START */

#define USBD_CFG_MAX_NUM           1
#define USBD_ITF_MAX_NUM           1

#define USB_MAX_STR_DESC_SIZ       255

#define USBD_SELF_POWERED
#define USBD_DYNAMIC_DESCRIPTOR_CHANGE_ENABLED 
#define  USE_USB_HS_IN_FS
#define MSC_IN_EP                  0x81
#define MSC_OUT_EP                 0x01

#ifdef USB_USB_HS_IN_HS
#define MSC_MAX_PACKET             512
#else
#define MSC_MAX_PACKET             64
#endif

/* NTFx CODE END */
#endif /* __USBD_CONF__H__ */

