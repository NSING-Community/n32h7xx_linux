/**
 * @file usbd_user.c
 * @author N32cube
 */
#include "usbd_user.h"
/* NTFx CODE START */
USBD_User_cb_TypeDef USBD1_USER_cb = {
  USBD1_USER_Init,
  USBD1_USER_DeviceReset,
  USBD1_USER_DeviceConfigured,
  USBD1_USER_DeviceSuspended,
  USBD1_USER_DeviceResumed,
  USBD1_USER_DeviceConnected,
  USBD1_USER_DeviceDisconnected,
};
/* NTFx CODE END */

/* NTFx CODE START */
/**
*\*\name   USBD1_USER_Init.
*\*\fun    host lib initialization.
*\*\param  none.
*\*\param  none.
*\*\return none.
*/
void USBD1_USER_Init(void)
{
/* NTFx CODE END */
    /* Setup SysTick Timer for 20 msec interrupts This interrupt is used to probe the joystick */
/* NTFx CODE START */   
}

/**
*\*\name   USBD1_USER_DeviceReset.
*\*\fun    device Reset Event.
*\*\param  speed : device speed.
*\*\param  none.
*\*\return none.
*/
void USBD1_USER_DeviceReset(uint8_t speed)
{
/* NTFx CODE END */
    switch (speed)
    {
        case USB_SPEED_HIGH:
        break;

        case USB_SPEED_FULL:
        break;
        default:
        break;
    }
/* NTFx CODE START */   
}

/**
*\*\name   USBD1_USER_DeviceConfigured.
*\*\fun    device configuration Event.
*\*\param  none.
*\*\param  none.
*\*\return none.
*/
void USBD1_USER_DeviceConfigured(void)
{
/* NTFx CODE END */

/* NTFx CODE START */
}

/**
*\*\name   USBD1_USER_DeviceConnected.
*\*\fun    device connection Event.
*\*\param  none.
*\*\param  none.
*\*\return none.
*/
void USBD1_USER_DeviceConnected(void)
{
/* NTFx CODE END */

/* NTFx CODE START */
}

/**
*\*\name   USBD1_USER_DeviceDisconnected.
*\*\fun    device disconnection Event.
*\*\param  none.
*\*\param  none.
*\*\return none.
*/
void USBD1_USER_DeviceDisconnected(void)
{
/* NTFx CODE END */

/* NTFx CODE START */
}

/**
*\*\name   USBD1_USER_DeviceSuspended.
*\*\fun    device suspend Event.
*\*\param  none.
*\*\param  none.
*\*\return none.
*/
void USBD1_USER_DeviceSuspended(void)
{
/* NTFx CODE END */
     /* Users can do their application actions here for the USB-Reset */
/* NTFx CODE START */
 
}

/**
*\*\name   USBD_USER_DeviceResumed.
*\*\fun    device resume Event.
*\*\param  none.
*\*\param  none.
*\*\return none.
*/
void USBD1_USER_DeviceResumed(void)
{
/* NTFx CODE END */
    /* Users can do their application actions here for the USB-Reset */
/* NTFx CODE START */

}
/* NTFx CODE END Last*/

