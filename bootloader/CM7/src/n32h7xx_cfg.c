/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file n32h7xx_cfg.c
 * @author N32cube
 */

#include "n32h7xx_cfg.h"
/* NTFx CODE START */
#include <string.h>
ETH_InfoType sEthInfo_ETH1;
__ALIGN_BEGIN  USB_CORE_MODULE USB1_Core __ALIGN_END;
        
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
 /**
 *@name  DMA_SetSrcDstAddr.
 *@brief Set peripher address and memory address of DMA
 *param   DMAy (The input parameters must be the following values):
 *          - DMA1
 *          - DMA2
 *          - DMA3
 *param   ChNum (The input parameters must be the following values):
 *          - DMA_CHANNEL_0
 *          - DMA_CHANNEL_1
 *          - DMA_CHANNEL_2
 *          - DMA_CHANNEL_3
 *          - DMA_CHANNEL_4
 *          - DMA_CHANNEL_5
 *          - DMA_CHANNEL_6
 *          - DMA_CHANNEL_7
 *@param SrcAddr   Source address
 *@param DstAddr   Destination address
 *@return status
 */
 void DMA_SetSrcDstAddr(DMA_Module *const DMAy,DMA_ChNumType ChNum, uint32_t SrcAddr,uint32_t DstAddr )
 {
    /* Sets channel n source address register */
    WRITE_REG(DMAy->CH[ChNum].SA, SrcAddr);
    /* Sets channel n destination address register */
    WRITE_REG(DMAy->CH[ChNum].DA, DstAddr);
 }
  /**
 *@name  MDMA_SetSrcDstAddr.
 *@brief Set peripher address and memory address of MDMA
 *param   MDMAy (The input parameters must be the following values):
 *          - MDMA
 *param   ChNum (The input parameters must be the following values):
 *          - MDMA_CHANNEL_0
 *          - MDMA_CHANNEL_1
 *          - MDMA_CHANNEL_2
 *          - MDMA_CHANNEL_3
 *          - MDMA_CHANNEL_4
 *          - MDMA_CHANNEL_5
 *          - MDMA_CHANNEL_6
 *          - MDMA_CHANNEL_7
 *          - MDMA_CHANNEL_8
 *          - MDMA_CHANNEL_9
 *          - MDMA_CHANNEL_10
 *          - MDMA_CHANNEL_11
 *          - MDMA_CHANNEL_12
 *          - MDMA_CHANNEL_13
 *          - MDMA_CHANNEL_14
 *          - MDMA_CHANNEL_15
 *@param SrcAddr   Source address
 *@param DstAddr   Destination address
 *@return status
 */
 void MDMA_SetSrcDstAddr(MDMA_Module *const MDMAy,MDMA_ChNumType ChNum, uint32_t SrcAddr,uint32_t DstAddr )
 {
    /* Sets channel n source address register */
    WRITE_REG(MDMAy->CH[ChNum].SA, SrcAddr);
    /* Sets channel n destination address register */
    WRITE_REG(MDMAy->CH[ChNum].DA, DstAddr);
 }
/* NTFx CODE END */

/* NTFx CODE START */
/**
 *@brief Initializes the clock tree
 *@param null
 *@return status
 */
bool RCC_Configuration(void)
{
    ErrorStatus ClockStatus;
     
    RCC_ConfigHSIclkDivider(RCC_HSICLK_DIV1);
    RCC_EnableHsi(ENABLE);
    /* Wait till HSI is ready */
    ClockStatus = RCC_WaitHsiStable();
    if (ClockStatus != SUCCESS) return false;
     
    /* Enable PWR peripheral clock */
    RCC_EnableAHB5PeriphClk2(RCC_AHB5_PERIPHEN_PWR, ENABLE);
    /* Enable access to backup registers */
    PWR_BackupAccessEnable(ENABLE);
    RCC_ConfigSysclkDivider(RCC_SYSCLK_DIV1);
    RCC_ConfigSysbusDivider(RCC_BUSCLK_DIV2);
    /*Configures the Periph clock source as HSI*/
    RCC_ConfigPeriphClk(RCC_PERIPHCLK_SRC_HSI);
     
    /* Configure APB1 clock is AHB1/2 = 150.000M */
    /* Configure APB2 clock is AHB2/2 = 150.000M */
    /* Configure APB5 clock is AHB5/2 = 150.000M */
    /* Configure APB6 clock is AHB6/2 = 150.000M */
    RCC_ConfigAPBclkDivider(RCC_APB1CLK_DIV2, RCC_APB2CLK_DIV2, RCC_APB5CLK_DIV2, RCC_APB6CLK_DIV2);
    /*Configures the PLL1 clock source and multiplication factor,Fin=64M,Fout=600M*/
    //RCC_ConfigPll1(RCC_PLL_SRC_HSI,64000000,600000000,ENABLE);
    RCC_ConfigPll1_NoCalculate(RCC_PLL_SRC_HSI,0,153600,3,ENABLE);
    /*Configure PLL1 divider value to Pll1A*/
    RCC_ConfigPLL1ADivider(RCC_PLLA_DIV1);
    /*Configure PLL1 divider value to Pll1B*/
    RCC_ConfigPLL1BDivider(RCC_PLLB_DIV2);
    /*Configure PLL1 divider value to Pll1C*/
    RCC_ConfigPLL1CDivider(RCC_PLLC_DIV2);
    /*Configures the PLL3 clock source and multiplication factor,Fin=64M,Fout=665M*/
    //RCC_ConfigPll3(RCC_PLL_SRC_HSI,64000000,665000000,ENABLE);
    RCC_ConfigPll3_NoCalculate(RCC_PLL_SRC_HSI,0,170240,4,ENABLE);
    /*Configure PLL3 divider value to Pll3A*/
    RCC_ConfigPLL3ADivider(RCC_PLLA_DIV5);
    /*Configure PLL3 divider value to Pll3B*/
    RCC_ConfigPLL3BDivider(RCC_PLLB_DIV1);
    /*Configure PLL3 divider value to Pll3C*/
    RCC_ConfigPLL3CDivider(RCC_PLLC_DIV1);
    /*Configure AXI clock source and divider*/
    RCC_ConfigAXIClk(RCC_AXIHYPERCLK_SRC_PLL1A);
    RCC_ConfigAXIclkDivider(RCC_AXICLK_DIV2);
    /*Configure M7 clock source and divider*/
    RCC_ConfigM7SystickClkDivider(RCC_STCLK_DIV1);
    RCC_ConfigM7Clk(RCC_M7HYPERCLK_SRC_PLL1A);
    /*Configure M4 clock  divider*/
    RCC_ConfigM4SystickClkDivider(RCC_STCLK_DIV2);
    /* configure sys_clk source is PLL1A */
    RCC_ConfigSysclk(RCC_SYSCLK_SRC_PLL1A);
    /* Check if sys_clk source is PLL1A */
    while(RCC_GetSysclkSrc() != RCC_SYSCLK_STS_PLL1A);
     
    /*Configures the ETH1 clock source and divider*/
    RCC_ConfigETH1PtpClk(RCC_ETHPTPCLK_SRC_SYSBUSDIV,RCC_ETHPTPCLK_SYSBUSDIV2);
    RCC_ConfigETH1GMIITXClk(RCC_ETH1GMIITXCLK_SRC_PLL1C);
    
    /*Config the DVP1/2 M clock AXI divider*/
    RCC_ConfigDVP1_2_MClkDivider(RCC_DVPMCLK_AXIDIV16);
    /*Config the DVP2 M clock source*/
    RCC_ConfigDVP2MClkSource(RCC_DVPMCLK_SRC_AXIDIV);
    
    /*Config the I2C4_6 kernel clock source (sys_bus_div_clk) divider*/
    RCC_ConfigI2C4_6_KerSysDivider(RCC_I2CKERCLK_SYSBUSDIV8);
    /*Config the I2C4 kernel clock source*/
    RCC_ConfigI2C4KerClkSource(RCC_I2CKERCLK_SRC_SYSBUSDIV);
    
    /*Config the USART1/2 PCLK divider from ahb1*/
    RCC_ConfigUSARTPClk(RCC_USARTPCLK_AHB1_DIV1);
    
    /*Config the SDRAM memory clock source and  divider*/
    RCC_ConfigSDRAMMemClk(RCC_SDRAMMEMCLK_SRC_PLL3A,RCC_SDRAMMEMCLK_AXIDIV4);
    
    /*Config the I2S1_2 kernel clock source (sys_bus_div_clk) divider*/
    RCC_ConfigI2S1_2_KerSysDivider(RCC_I2SKERCLK_SYSBUSDIV8);
    /*Config the I2S1 kernel clock source*/
    RCC_ConfigI2S1KerClkSource(RCC_I2SKERCLK_SRC_SYSBUSDIV);
    /*Config the I2S3_4 kernel clock source (sys_bus_div_clk) divider*/
    RCC_ConfigI2S3_4_KerSysDivider(RCC_I2SKERCLK_SYSBUSDIV8);
    /*Config the I2S4 kernel clock source*/
    RCC_ConfigI2S4KerClkSource(RCC_I2SKERCLK_SRC_CLKIN);
    
    /*Config the USB1/2 reference clock divider from HSE*/
    RCC_ConfigUSBRefClk(RCC_USBREFCLK_HSE_DIV2);
    
    /*Configures the SDMMC1 clock source and divider*/
    RCC_ConfigSDMMC1KerClk(RCC_SDMMC1KERCLK_SRC_AXIDIV,RCC_SDMMC1KERCLK_AXIDIV8);
    
    /*Config the TRNG clock*/
    RCC_ConfigTRNGClk(RCC_TRNGCLK_SRC_SYSBUSDIV,RCC_TRNGCLK_SYSBUSDIV2);
    
     
    /*Enables the AHB2 peripheral clock 2 .*/
    RCC_EnableAHB2PeriphClk2(RCC_AHB2_PERIPHEN_M7_ETH1TX | RCC_AHB2_PERIPHEN_M7_ETH1RX | RCC_AHB2_PERIPHEN_M7_ETH1MAC ,ENABLE);
    /*Enables the AXI peripheral clock 2 .*/
    RCC_EnableAXIPeriphClk2(RCC_AXI_PERIPHEN_M7_DVP2APB | RCC_AXI_PERIPHEN_M7_DVP2 ,ENABLE);
    /*Enables the APB2 peripheral clock 2 .*/
    RCC_EnableAPB2PeriphClk2(RCC_APB2_PERIPHEN_M7_I2C4 | RCC_APB2_PERIPHEN_M7_I2S1 ,ENABLE);
    /*Enables the APB1 peripheral clock 3 .  The low-power gate must stay open
     * as well: Linux idles the core with WFI/WFE, and with only the run gate
     * enabled USART1 stops receiving characters while the core is asleep, so
     * the console appears dead even though transmit still works.*/
    RCC_EnableAPB1PeriphClk3(RCC_APB1_PERIPHEN_M7_USART1 |
                             RCC_APB1_PERIPHEN_M7_USART1LP ,ENABLE);
    /*Enables the AXI peripheral clock 4 .*/
    RCC_EnableAXIPeriphClk4(RCC_AXI_PERIPHEN_M7_SDRAM ,ENABLE);
    /*Enables the APB1 peripheral clock 4 .*/
    RCC_EnableAPB1PeriphClk4(RCC_APB1_PERIPHEN_M7_I2S4 ,ENABLE);
    /*Enables the AHB2 peripheral clock 1 .*/
    RCC_EnableAHB2PeriphClk1(RCC_AHB2_PERIPHEN_M7_USB1 ,ENABLE);
    /*Enables the AXI peripheral clock 1 .*/
    RCC_EnableAXIPeriphClk1(RCC_AXI_PERIPHEN_M7_SDMMC1 ,ENABLE);
    /*Enables EXTI clock .*/
    RCC_EnableAPB5PeriphClk2(RCC_APB5_PERIPHEN_EXTI,ENABLE);
/* NTFx CODE END */

    return true;
}
/* NTFx CODE START */
/**
 *@brief Initializes the NVIC
 *@param null
 *@return status
 */
bool NVIC_Configuration(void)
{
     
    NVIC_InitType NVIC_InitStructure;
    EXTI_InitType EXTI_InitStructure;
    EXTI_InitStruct(&EXTI_InitStructure);
    /*Configure the preemption priority and subpriority:
    - 4 bits for pre-emption priority: possible value are 0..15
    - 0 bits for subpriority: possible value are 0
    - Lower values gives higher priority
    */
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);
    
    /*Set USB1_HS_WKUP_IRQ  interrupt priority*/
    NVIC_InitStructure.NVIC_IRQChannel                   =USB1_HS_WKUP_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority =0;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority        =0;
    NVIC_InitStructure.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&NVIC_InitStructure);
    
    /*Set USB1_HS_IRQ  interrupt priority*/
    NVIC_InitStructure.NVIC_IRQChannel                   =USB1_HS_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority =0;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority        =0;
    NVIC_InitStructure.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&NVIC_InitStructure);
    /*config the EXTI62 ,EXTI62 connect to USB1 weakup*/
    EXTI_InitStructure.EXTI_Line    = EXTI_LINE62;
    EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Rising;
    EXTI_InitStructure.EXTI_Mode    = EXTI_Mode_Interrupt;
    EXTI_InitStructure.EXTI_LineCmd = ENABLE;
    EXTI_InitPeripheral(&EXTI_InitStructure);
     
/* NTFx CODE END */

    return true;
}
/* NTFx CODE START */
/**
 *@brief Initializes the DMA
 *@param null
 *@return status
 */
bool DMA_Configuration(void)
{
     
/* NTFx CODE END */

    return true;
}
/* NTFx CODE START */
/**
 *@brief Initializes the GPIO
 *@param null
 *@return status
 */
bool GPIO_Configuration(void)
{
     
    GPIO_InitType GPIO_InitStructure;
    GPIO_InitStruct(&GPIO_InitStructure);
    /* Enable the GPIO clock*/
    RCC_EnableAHB5PeriphClk1(RCC_AHB5_PERIPHEN_M7_GPIOA | RCC_AHB5_PERIPHEN_M7_GPIOH | RCC_AHB5_PERIPHEN_M7_GPIOG | RCC_AHB5_PERIPHEN_M7_GPIOF | RCC_AHB5_PERIPHEN_M7_GPIOD | RCC_AHB5_PERIPHEN_M7_GPIOE | RCC_AHB5_PERIPHEN_M7_GPIOC | RCC_AHB5_PERIPHEN_M4_GPIOA | RCC_AHB5_PERIPHEN_M4_GPIOH | RCC_AHB5_PERIPHEN_M4_GPIOG | RCC_AHB5_PERIPHEN_M4_GPIOF | RCC_AHB5_PERIPHEN_M4_GPIOD | RCC_AHB5_PERIPHEN_M4_GPIOE | RCC_AHB5_PERIPHEN_M4_GPIOC, ENABLE);
    RCC_EnableAHB5PeriphClk2(RCC_AHB5_PERIPHEN_M7_GPIOI | RCC_AHB5_PERIPHEN_M4_GPIOI, ENABLE);
    RCC_EnableAHB5PeriphClk2(RCC_AHB5_PERIPHEN_M7_AFIO|RCC_AHB5_PERIPHEN_M4_AFIO,ENABLE);
    
     
     
    /*********Initialize SDRAM GPIO function *********/
    /********PH8->SDRAM.D16********/
    /********PH9->SDRAM.D17********/
    /********PH10->SDRAM.D18********/
    /********PH11->SDRAM.D19********/
    /********PH12->SDRAM.D20********/
    /********PH13->SDRAM.D21********/
    /********PH14->SDRAM.D22********/
    /********PH15->SDRAM.D23********/
    /********PI0->SDRAM.D24********/
    /********PI1->SDRAM.D25********/
    /********PI2->SDRAM.D26********/
    /********PI3->SDRAM.D27********/
    /********PI6->SDRAM.D28********/
    /********PI7->SDRAM.D29********/
    /********PI9->SDRAM.D30********/
    /********PI10->SDRAM.D31********/
    /********PI4->SDRAM.DQM2********/
    /********PI5->SDRAM.DQM3********/
    /********PG8->SDRAM.CLK********/
    /********PF11->SDRAM.NRAS********/
    /********PF0->SDRAM.A0********/
    /********PF1->SDRAM.A1********/
    /********PF2->SDRAM.A2********/
    /********PF3->SDRAM.A3********/
    /********PF4->SDRAM.A4********/
    /********PF5->SDRAM.A5********/
    /********PF12->SDRAM.A6********/
    /********PF13->SDRAM.A7********/
    /********PF14->SDRAM.A8********/
    /********PF15->SDRAM.A9********/
    /********PG0->SDRAM.A10********/
    /********PG1->SDRAM.A11********/
    /********PD14->SDRAM.D0********/
    /********PD15->SDRAM.D1********/
    /********PD0->SDRAM.D2********/
    /********PD1->SDRAM.D3********/
    /********PE7->SDRAM.D4********/
    /********PE8->SDRAM.D5********/
    /********PE9->SDRAM.D6********/
    /********PE10->SDRAM.D7********/
    /********PA4->SDRAM.D8********/
    /********PA5->SDRAM.D9********/
    /********PE13->SDRAM.D10********/
    /********PE14->SDRAM.D11********/
    /********PC0->SDRAM.D12********/
    /********PD8->SDRAM.D13********/
    /********PD9->SDRAM.D14********/
    /********PD10->SDRAM.D15********/
    /********PE0->SDRAM.DQM0********/
    /********PG2->SDRAM.DQM1********/
    /********PG4->SDRAM.BA0********/
    /********PG5->SDRAM.BA1********/
    /********PC3->SDRAM.CKE0********/
    /********PC5->SDRAM.NCAS********/
    /********PC2->SDRAM.NCE0********/
    /********PH5->SDRAM.NWE********/
    /*Initialize AF_PP GPIO */
    GPIO_InitStructure.GPIO_Mode      = GPIO_MODE_AF_PP;
    GPIO_InitStructure.GPIO_Pull      = GPIO_NO_PULL;
    GPIO_InitStructure.GPIO_Slew_Rate = GPIO_SLEW_RATE_SLOW;
    GPIO_InitStructure.GPIO_Current   = GPIO_HS_IO_DC_1mA;
    GPIO_InitStructure.GPIO_Alternate = GPIO_AF0;
    GPIO_InitStructure.Pin            = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15 | GPIO_PIN_5;
    GPIO_InitPeripheral(GPIOH,&GPIO_InitStructure);
     
    GPIO_InitStructure.Pin            = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_6 | GPIO_PIN_7 | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_4 | GPIO_PIN_5;
    GPIO_InitPeripheral(GPIOI,&GPIO_InitStructure);
     
    GPIO_InitStructure.Pin            = GPIO_PIN_8 | GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5;
    GPIO_InitPeripheral(GPIOG,&GPIO_InitStructure);
     
    GPIO_InitStructure.Pin            = GPIO_PIN_11 | GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;
    GPIO_InitPeripheral(GPIOF,&GPIO_InitStructure);
     
    GPIO_InitStructure.Pin            = GPIO_PIN_14 | GPIO_PIN_15 | GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10;
    GPIO_InitPeripheral(GPIOD,&GPIO_InitStructure);
     
    GPIO_InitStructure.Pin            = GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_0;
    GPIO_InitPeripheral(GPIOE,&GPIO_InitStructure);
     
    GPIO_InitStructure.Pin            = GPIO_PIN_4 | GPIO_PIN_5;
    GPIO_InitPeripheral(GPIOA,&GPIO_InitStructure);
     
    GPIO_InitStructure.Pin            = GPIO_PIN_3 | GPIO_PIN_2;
    GPIO_InitPeripheral(GPIOC,&GPIO_InitStructure);
     
    GPIO_InitStructure.GPIO_Alternate = GPIO_AF1;
    GPIO_InitStructure.Pin            = GPIO_PIN_0;
    GPIO_InitPeripheral(GPIOC,&GPIO_InitStructure);
     
    GPIO_InitStructure.Pin            = GPIO_PIN_2;
    GPIO_InitPeripheral(GPIOG,&GPIO_InitStructure);
     
    GPIO_InitStructure.GPIO_Alternate = GPIO_AF7;
    GPIO_InitStructure.Pin            = GPIO_PIN_5;
    GPIO_InitPeripheral(GPIOC,&GPIO_InitStructure);
     
     
    /*********Initialize USB1 GPIO function *********/
    /********PA11->USB1.DM********/
    /********PA12->USB1.DP********/
    /*Initialize AF_PP GPIO */
    GPIO_InitStructure.GPIO_Mode      = GPIO_MODE_AF_PP;
    GPIO_InitStructure.GPIO_Pull      = GPIO_NO_PULL;
    GPIO_InitStructure.GPIO_Slew_Rate = GPIO_SLEW_RATE_SLOW;
    GPIO_InitStructure.GPIO_Current   = GPIO_5VTOL_DC_1mA;
    GPIO_InitStructure.GPIO_Alternate = GPIO_AF10;
    GPIO_InitStructure.Pin            = GPIO_PIN_11 | GPIO_PIN_12;
    GPIO_InitPeripheral(GPIOA,&GPIO_InitStructure);
     
     
    /*********Initialize SDMMC1 GPIO function *********/
    /********PC12->SDMMC1.CK********/
    /********PD2->SDMMC1.CMD********/
    /********PC9->SDMMC1.D1********/
    /********PC10->SDMMC1.D2********/
    /********PC11->SDMMC1.D3********/
    /********PC8->SDMMC1.D0********/
    /*Initialize AF_PP GPIO */
    GPIO_InitStructure.GPIO_Mode      = GPIO_MODE_AF_PP;
    GPIO_InitStructure.GPIO_Pull      = GPIO_NO_PULL;
    GPIO_InitStructure.GPIO_Slew_Rate = GPIO_SLEW_RATE_SLOW;
    GPIO_InitStructure.GPIO_Current   = GPIO_HS_IO_DC_1mA;
    GPIO_InitStructure.GPIO_Alternate = GPIO_AF1;
    GPIO_InitStructure.Pin            = GPIO_PIN_12;
    GPIO_InitPeripheral(GPIOC,&GPIO_InitStructure);
     
    GPIO_InitStructure.GPIO_Alternate = GPIO_AF2;
    GPIO_InitStructure.Pin            = GPIO_PIN_2;
    GPIO_InitPeripheral(GPIOD,&GPIO_InitStructure);
     
    GPIO_InitStructure.GPIO_Current   = GPIO_DC_2mA;
    GPIO_InitStructure.GPIO_Alternate = GPIO_AF1;
    GPIO_InitStructure.Pin            = GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11;
    GPIO_InitPeripheral(GPIOC,&GPIO_InitStructure);
     
    GPIO_InitStructure.GPIO_Current   = GPIO_5VTOL_DC_1mA;
    GPIO_InitStructure.GPIO_Alternate = GPIO_AF0;
    GPIO_InitStructure.Pin            = GPIO_PIN_8;
    GPIO_InitPeripheral(GPIOC,&GPIO_InitStructure);
     
/* NTFx CODE END */

    return true;
}
/* NTFx CODE START */
/**
 *@brief Initializes the ETH
 *@param null
 *@return status
 */
bool ETH_Configuration(void)
{
     
/* NTFx CODE END */

    return true;
}
/* NTFx CODE START */
/**
 *@brief Initializes the DVP
 *@param null
 *@return status
 */
bool DVP_Configuration(void)
{
/* NTFx CODE END */

    return true;
}
/* NTFx CODE START */
/**
 *@brief Initializes the I2C
 *@param null
 *@return status
 */
bool I2C_Configuration(void)
{
    I2C_InitType I2C_InitStructure;
     
    /*********initialize the I2C4************/
    I2C_InitStruct(&I2C_InitStructure);
    I2C_InitStructure.Timing           = 0xD093DA;
    I2C_InitStructure.HSTiming         = 0x0;
    I2C_InitStructure.OwnAddress1      = 0x0;
    I2C_InitStructure.AddressingMode   = I2C_ADDRESSINGMODE_7BIT;
    I2C_InitStructure.DualAddressMode  = I2C_DUALADDRESS_DISABLE;
    I2C_InitStructure.GeneralCallMode  = I2C_GENERALCALL_DISABLE;
    I2C_InitStructure.NoStretchMode    = I2C_NOSTRCH_DISABLE;
    I2C_Init(I2C4, &I2C_InitStructure);
     
    I2C_Enable(I2C4,ENABLE);
     
/* NTFx CODE END */

    return true;
}
/* NTFx CODE START */
/**
 *@brief Initializes the USART
 *@param null
 *@return status
 */
bool USART_Configuration(void)
{
    USART_InitType USART_InitStructure;
    USART_StructInit(&USART_InitStructure);
     
     
    /*********initialize the USART1************/
    USART_DeInit(USART1);
    USART_InitStructure.BaudRate            = 115200;
    USART_InitStructure.WordLength          = USART_WL_8B;
    USART_InitStructure.StopBits            = USART_STPB_1;
    USART_InitStructure.Parity              = USART_PE_NO; 
    USART_InitStructure.HardwareFlowControl = USART_HFCTRL_NONE; 
    USART_InitStructure.Mode                = USART_MODE_RX | USART_MODE_TX; 
    USART_InitStructure.OverSampling        = USART_16OVER ; 
    /* Configure USART1 */
    USART_Init(USART1, &USART_InitStructure);
     
     
    /* Enable the USART1 */
    USART_Enable(USART1, ENABLE);
/* NTFx CODE END */

    return true;
}
/* NTFx CODE START */
/**
 *@brief Initializes the SDRAM
 *@param null
 *@return status
 */
bool SDRAM_Configuration(void)
{
    SDRAM_TimingType SDRAM_Timing;
    uint8_t i = 0;
     
    RCC_EnableSDRAMDelayChain(ENABLE);
    RCC_ConfigSDRAMDelay(RCC_SDRAM_DELAY_0_2NS);
    /* Set SDRAM Refresh Interval=(Refresh Period / Number of Rows / Mem clock Period)*/
    SDRAM_RefreshIntervalInit(0x3E8);
    SDRAM_Timing.RowActiveTime       = 0x7;
    SDRAM_Timing.RowCycleTime        = 0x9;
    SDRAM_Timing.RowActToRowActDelay = 0x3;
    SDRAM_Timing.PrechargeTime       = 0x4;
    SDRAM_Timing.WriteRecoveryTime   = 0x3;
    SDRAM_Timing.RefreshCycleTime    = 0x9;
    SDRAM_Timing.RAStoCASDelay       = 0x4;
    SDRAM_TimingInit(&SDRAM_Timing);
     
    /********Config SDRAM 1********************/
     /* Set SDRAM address: BaseAddr = 0xD0000000, AddrMask = 0xFFFFFFFF-(SDRAMSIZI-1)*/
    SDRAM_SetDeviceAddress(SDRAM_DEVICE_1, 0xC0000000, 0xFF000000);
    /* Set SDRAM1 configuration registers*/
    SDRAM_EnableDevice(SDRAM_DEVICE_1,ENABLE);
    SDRAM_EnableRefreshCMD(SDRAM_DEVICE_1, ENABLE);
    SDRAM_EnableAutoPrecharge(SDRAM_DEVICE_1,DISABLE);
    SDRAM_EnablePrefetchRead(SDRAM_DEVICE_1, DISABLE);
    SDRAM_EnableSOM(SDRAM_DEVICE_1, ENABLE);
    SDRAM_EnableBankInterleave(SDRAM_DEVICE_1, DISABLE);
    SDRAM_ConfigBusWidth(SDRAM_DEVICE_1, SDRAM_DEVICE_BUSWID_32BITS);
    SDRAM_ConfigBurstLength(SDRAM_DEVICE_1, SDRAM_DEVICE_BURSTLEN_2);
    SDRAM_ConfigCASLatency(SDRAM_DEVICE_1, SDRAM_DEVICE_CASLTCY_3);
    SDRAM_ConfigAddress(SDRAM_DEVICE_1, SDRAM_BANK4_ROW4096_COL256);
     
    /*Precharge SDRAM1 all banks*/
    SDRAM_EnableClock(ENABLE);
    SDRAM_SetOperationCode(SDRAM_OPCODE_PRECHRG);
    SDRAM_SetDeviceSelect(SDRAM_CS_SDRAM1_ONLY);
    SDRAM_SetBank(SDRAM_BANKADD_1);
    SDRAM_SetAddress(0x00);
    /*A dummy access to activate previous SDRAM_OS configuration, read/write data is dont care*/
    (void)(SDRAM->OR);
     
    /*auto-refresh SDRAM1 all banks*/
    for(i = 0; i < 2; i++)
    {
        SDRAM_EnableClock(ENABLE);
        SDRAM_SetOperationCode(SDRAM_OPCODE_REFRESH);
        SDRAM_SetDeviceSelect(SDRAM_CS_SDRAM1_ONLY);
        SDRAM_SetBank(SDRAM_BANKADD_1);
        SDRAM_SetAddress(0x00);
        /*A dummy access to activate previous SDRAM_OS configuration, read/write data is dont care*/
        (void)(SDRAM->OR);
    }
     
    /* load mode register */
    SDRAM_EnableClock(ENABLE);
    SDRAM_SetOperationCode(SDRAM_OPCODE_LOADMODE);
    SDRAM_SetDeviceSelect(SDRAM_CS_SDRAM1_ONLY);
    SDRAM_SetBank(SDRAM_BANKADD_1);
    /*BurstLen=Burstlen_2, BurstType=Sequential,CASLatency=CASLtcy_3,OpMode=Standard,WBMode=Burst*/
    SDRAM_SetAddress(0x31);
    /*A dummy access to activate previous SDRAM_OS configuration, read/write data is dont care*/
    (void)(SDRAM->OR);
    SysTick_Delayms(1);
/* NTFx CODE END */

    return true;
}
/* NTFx CODE START */
/**
 *@brief Initializes the I2S
 *@param null
 *@return status
 */
bool I2S_Configuration(void)
{
    I2S_InitType I2S_InitStructure;
    I2S_InitStruct(&I2S_InitStructure);
     
    /***************** I2S1 configuration ***************/
    I2S_InitStructure.I2sMode      = I2S_MODE_MASTER_TX;
    I2S_InitStructure.Standard       = I2S_STD_PHILLIPS;
    I2S_InitStructure.DataFormat     = I2S_DATA_FMT_16BITS;
    I2S_InitStructure.MCLKEnable     = I2S_MCLK_DISABLE;
    I2S_InitStructure.AudioFrequency = ((uint32_t)44100U);
    I2S_InitStructure.ClkSrcFrequency =((uint32_t)37500000U);
    I2S_InitStructure.CLKPOL         = I2S_CLKPOL_LOW;
    I2S_Init(I2S1, &I2S_InitStructure);
     
    I2S_InitStructure.I2sMode     = I2S_MODE_SlAVE_RX;
    I2S_EXTInit(I2S1_EXT, &I2S_InitStructure); 
     
    /* Enable I2S1*/
    I2S_Enable(I2S1,ENABLE);
    /* Enable I2S1_EXT*/
    I2S_EXTEnable(I2S1_EXT,ENABLE);
    /***************** I2S4 configuration ***************/
    I2S_InitStructure.MCLKEnable     = I2S_MCLK_ENABLE;
    I2S_InitStructure.ClkSrcFrequency =((uint32_t)12280000U);
    I2S_Init(I2S4, &I2S_InitStructure);
     
    I2S_InitStructure.I2sMode     = I2S_MODE_SlAVE_RX;
    I2S_EXTInit(I2S4_EXT, &I2S_InitStructure); 
     
    /* Enable I2S4*/
    I2S_Enable(I2S4,ENABLE);
    /* Enable I2S4_EXT*/
    I2S_EXTEnable(I2S4_EXT,ENABLE);
/* NTFx CODE END */

    return true;
}
/* NTFx CODE START */
/**
 *@brief Initializes the USB
 *@param null
 *@return status
 */
bool USB_Configuration(void)
{
    PWR_MoudlePowerEnable(HSC1_USB1_PWRCTRL, ENABLE);
    USBD_Init(&USB1_Core, USBHS1_CORE_ID, &USBD1_desc, &USBD1_MSC_cb, &USBD1_USER_cb);
    //Use reference:
    //Add the configuration related to the MSC hardware in the "usbd_msc_storage.c" file.
/* NTFx CODE END */

    return true;
}
/* NTFx CODE START */
/**
 *@brief Initializes the SDMMC
 *@param null
 *@return status
 */
#if defined(BOOT_STORAGE_SDMMC)
bool SDMMC_Configuration(void)
{
    SDMMC_WrapperType SDMMC_WrapperParamstruct;
    mmc_card_t mmccard ={0};
    /*********initialize the SDMMC1************/
    PWR_MoudlePowerEnable(HSC1_SDMMC1_PWRCTRL,ENABLE);
    mmccard.SDHOSTx = SDHOST1;
    mmccard.SDMMCx = SDMMC1;
    mmccard.card_workmode.busTiming = MMC_NormalSpeedTiming;
    mmccard.card_workmode.busClock_Hz = 37500000;
    mmccard.card_workmode.busWidth = MMC_DataBusWidth4bit;
    mmccard.card_workmode.dma = SDMMC_NODMA;
    mmccard.card_workmode.operationVoltageflag = MMC_OperationVoltageNone;
    SDMMC_TModeStructInit(&mmccard.TMODE_truct);
    SDMMC_StructWrapperInit(&SDMMC_WrapperParamstruct);
    SDMMC_WrapperParamstruct.MaxBlockLen     = SDMMC_MAXBLOCKLEN_512B;
    SDMMC_WrapperParamstruct.SDBaseCLKFreq   = 25;
    SDMMC_WrapperParamstruct.TimeOutUnit     = SDMMC_TIMEOUTCLKUNIT_KHZ;
    SDMMC_WrapperParamstruct.TuningCNT       = 0x20;
    SDMMC_WrapperParamstruct.WKUPSignalMode  = SDMMC_ASYNCWKUP;
    SDMMC_WrapperParamstruct.SPIBlockMode    = SDMMC_SPIBLOCKMODEUNSUPPORT;
    SDMMC_WrapperParamstruct.SPIMode         = SDMMC_SPIMODEUNSUPPORT;
    SDMMC_WrapperParamstruct.DDR50           = SDMMC_DDR50SUPPORT;
    SDMMC_WrapperParamstruct.SDR104          = SDMMC_SDR104SUPPORT;
    SDMMC_WrapperParamstruct.SDR50           = SDMMC_SDR50SUPPORT;
    SDMMC_WrapperParamstruct.SlotType        = SDMMC_SDTYPE;
    SDMMC_WrapperParamstruct.AsyncInt        = SDMMC_ASYNCINTSUPPORT;
    SDMMC_WrapperParamstruct.Suspend_Resume  = SDMMC_SUSRESUNSUPPORT;
    SDMMC_WrapperParamstruct.SDMA            = SDMMC_SDMASUPPORT;
    SDMMC_WrapperParamstruct.HS              = SDMMC_HSSUPPORT;
    SDMMC_WrapperParamstruct.ADMA2           = SDMMC_ADMA2SUPPORT;
    SDMMC_WrapperParamstruct.Embedded_8bit   = SDMMC_EMBEDDEDUNSUPPORT;
    SDMMC_WrapperParamstruct.UseTuningSDR50  = SDMMC_SDR50TUNING;
    SDMMC_WrapperParamstruct.DSSDCLKFreq     = 0x04;
    SDMMC_WrapperParamstruct.INITSDCLKFreq   = 0x00;
    SDMMC_WrapperParamstruct.SDR12SDCLKFreq  = 0x04;
    SDMMC_WrapperParamstruct.HSSDCLKFreq     = 0x02;
    SDMMC_WrapperParamstruct.SDR50SDCLKFreq  = 0x01;
    SDMMC_WrapperParamstruct.SDR25SDCLKFreq  = 0x02;
    SDMMC_WrapperParamstruct.SDR104SDCLKFreq = 0x00;
    SDMMC_WrapperParamstruct.DDR50SDCLKFreq  = 0x02;
    SDMMC_WrapperConfig(SDMMC1,&SDMMC_WrapperParamstruct);
    AFIO_SDMMCClkSel(SDMMC1_CLKFB, ENABLE);
    
/* NTFx CODE END */

    return true;
}
#endif /* BOOT_STORAGE_SDMMC */
