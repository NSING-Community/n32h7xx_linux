/**
 * @file usbd_msc_core.h
 * @author N32cube
 */
#ifndef _USB_MSC_CORE_H_
#define _USB_MSC_CORE_H_

#include  "usbd_ioreq.h"
#include "usbd_msc.h"
/* NTFx CODE START */

#define BOT_GET_MAX_LUN              0xFE
#define BOT_RESET                    0xFF
#define USB_MSC_CONFIG_DESC_SIZ      32

#define MSC_EPIN_SIZE                MSC_MAX_PACKET 
#define MSC_EPOUT_SIZE               MSC_MAX_PACKET 
/* NTFx CODE END */

/* NTFx CODE START */

extern USBD_Class_cb_TypeDef  USBD1_MSC_cb;
/* NTFx CODE END */

#endif /* _USB_MSC_CORE_H_ */

