/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file n32h7xx_it.c
 * @author N32cube
 */

/* NTFx CODE START */
#include "n32h7xx_it.h"
#include "n32h7xx.h"
#include "usbhs_dcd_int.h"
/* NTFx CODE END */

/* NTFx CODE START */
extern __IO uint32_t mwTick;
extern __ALIGN_BEGIN  USB_CORE_MODULE USB1_Core __ALIGN_END;

extern uint32_t MMC_systick_timeoutms;
/**
 * @brief  This function handles NMI exception.
 */
void NMI_Handler(void)
{
/* NTFx CODE END */

}
/* NTFx CODE START */
/**
 * @brief  This function handles Hard Fault exception.
 */
void HardFault_Handler(void)
{
    /* Go to infinite loop when Hard Fault exception occurs */
    while (1)
    {
    /* NTFx CODE END */

    }
}
/* NTFx CODE START */
/**
 * @brief  This function handles Memory Manage exception.
 */
void MemManage_Handler(void)
{
    /* Go to infinite loop when Memory Manage exception occurs */
    while (1)
    {
/* NTFx CODE END */

    }
}
/* NTFx CODE START */
/**
 * @brief  This function handles Bus Fault exception.
 */
void BusFault_Handler(void)
{
    /* Go to infinite loop when Bus Fault exception occurs */
    while (1)
    {
/* NTFx CODE END */

    }
}
/* NTFx CODE START */
/**
 * @brief  This function handles Usage Fault exception.
 */
void UsageFault_Handler(void)
{
    /* Go to infinite loop when Usage Fault exception occurs */
    while (1)
    {
/* NTFx CODE END */

    }
}
/* NTFx CODE START */
/**
 * @brief  This function handles SVCall exception.
 */
void SVC_Handler(void)
{
/* NTFx CODE END */

}
/* NTFx CODE START */
/**
 * @brief  This function handles Debug Monitor exception.
 */
void DebugMon_Handler(void)
{
/* NTFx CODE END */

}
/* NTFx CODE START */
/**
 * @brief  This function handles SysTick Handler.
 */
void SysTick_Handler(void)
{
    mwTick++;
#if defined(BOOT_STORAGE_SDMMC)
    MMC_systick_timeoutms++;
#endif
/* NTFx CODE END */

}
/* NTFx CODE START(USB1_HS_WKUP_IRQHandler)*/
/**
 * @brief  This function handles USB1_HS_WKUP_IRQHandler.
 */
void USB1_HS_WKUP_IRQHandler(void)
{
/* NTFx CODE END */

/* NTFx CODE START */
    EXTI_ClrITPendBit(EXTI_LINE62);
    USBD_ISTR_WKUP_handler(&USB1_Core);
/* NTFx CODE END */
    
/* NTFx CODE START */
}
/* NTFx CODE END(USB1_HS_WKUP_IRQHandler)*/

/* NTFx CODE START(USB1_HS_IRQHandler)*/
/**
 * @brief  This function handles USB1_HS_IRQHandler.
 */
void USB1_HS_IRQHandler(void)
{
/* NTFx CODE END */

/* NTFx CODE START */
    USBD_ISTR_Handler(&USB1_Core);
/* NTFx CODE END */
    
/* NTFx CODE START */
}
/* NTFx CODE END(USB1_HS_IRQHandler)*/

