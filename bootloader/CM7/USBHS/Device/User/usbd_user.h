/**
 * @file usbd_user.h
 * @author N32cube
 */

#ifndef __USBD_USER_H__
#define __USBD_USER_H__


/* Includes ------------------------------------------------------------------*/
#include "usbd_ioreq.h"
/* NTFx CODE START */
extern  USBD_User_cb_TypeDef USBD1_USER_cb;
/* NTFx CODE END */

/* NTFx CODE START */
void  USBD1_USER_Init(void);
void  USBD1_USER_DeviceReset(uint8_t speed);
void  USBD1_USER_DeviceConfigured(void);
void  USBD1_USER_DeviceSuspended(void);
void  USBD1_USER_DeviceResumed(void);
void  USBD1_USER_DeviceConnected(void);
void  USBD1_USER_DeviceDisconnected(void);
/* NTFx CODE END */
#endif /*__USBD_USER_H__*/

