/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file main.c
 * @author N32cube
 */
  
 //!!!!!!!!!!!!!!!!NOTE!!!!!!!!!!!!!!!
 // Code cannot be added between /* NTFx CODE START xxxxx*/ and /* NTFx CODE END xxxxx*/

/* NTFx CODE START Include*/
#include "main.h"
#include <stdio.h>
#include <stdint.h>
/* NTFx CODE END Include*/

#include "boot_app.h"

/**
 * @brief  Main program.
 */
int main(void)
{

    /* NTFx CODE START Config*/
    CopyVectTable(0x15000000,0x0,VECT_SIZE);
    RCC_Configuration();
    GPIO_Configuration();
    DMA_Configuration();
    /*  Configure the SysTick to have interrupt in 1ms time basis,then call SysTick_Delayms(ms)*/
    SysTick_Config(600000); 
    NVIC_Configuration();
    ETH_Configuration();
    DVP_Configuration();
    I2C_Configuration();
    USART_Configuration();
    SDRAM_Configuration();
    I2S_Configuration();
#if defined(BOOT_STORAGE_SDMMC)
    SDMMC_Configuration();
#endif
    /* NTFx CODE END Config*/

    /* Bring-up is kept outside N32Cube's generated block so it survives a
     * regeneration. USB is started only in MSC mode, after SD_App_Init(). */
    BOOT_App_Run();

    while(1)
    {

    }
}


