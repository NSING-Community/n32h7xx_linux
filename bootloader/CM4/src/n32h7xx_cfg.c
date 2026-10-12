/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file n32h7xx_cfg.c
 * @author N32cube
 */

 #include "n32h7xx_cfg.h"

/* NTFx CODE START */
__IO uint32_t mwTick;
void SysTick_Delayms(uint32_t Delayms)
{
    uint32_t tickstart = mwTick;
    uint32_t wait=Delayms;
    /* Add 1 to guarantee minimum wait */
    if (wait < 0xFFFFFFFFU)
    {
        wait +=1;
    }
    while ((mwTick - tickstart) < wait)
    {
    }
}
/* NTFx CODE END */

/* NTFx CODE START */
/**
 *@brief Initializes the NVIC
 *@param null
 *@return status
 */
bool NVIC_Configuration(void)
{
     
/* NTFx CODE END */

    return true;
}
