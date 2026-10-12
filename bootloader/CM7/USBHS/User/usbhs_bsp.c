/**
 * @file usbhs_bsp.c
 * @author N32cube
 */
#include "usbhs_bsp.h"
#include "n32h7xx_cfg.h"
/* NTFx CODE START */

/**
*\*\name   USB_BSP_mDelay.
*\*\fun    This function provides delay time in milli sec.
*\*\param  msec : Value of delay required in milli sec.
*\*\return none
*/
void USB_BSP_mDelay (const uint32_t msec)
{
	SysTick_Delayms(msec);
}

/**
*\*\name   USB_BSP_uDelay.
*\*\fun    This function provides delay time in micro sec.
*\*\param  usec : Value of delay required in micro sec.
*\*\return none
*/
void USB_BSP_uDelay (const uint32_t usec)
{
/* NTFx CODE END */
    uint32_t ms=usec/1000;
    if(usec%1000>0)
        ms++;
	SysTick_Delayms(ms);
}
