/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file n32h7xx_cfg.h
 * @author N32cube
 */
#ifndef __N32H7XX_CFG_H__
#define __N32H7XX_CFG_H__

#ifdef __cplusplus
extern "C" {
#endif
/* NTFx CODE START INCLUDE*/
#include "n32h7xx.h"
#include "n32h7xx_dbg.h"
#include "n32h7xx_dcmu.h"
#include "n32h7xx_eccmon.h"
#include "n32h7xx_mmu.h"
#include "n32h7xx_otpc.h"
#include "n32h7xx_smu.h"
#include "n32h7xx_vrefbuf.h"
#include "n32h7xx_rcc.h"
#include "misc.h"
#include "n32h7xx_dma.h"
#include "n32h7xx_gpio.h"
#include "n32h7xx_eth.h"
#include "n32h7xx_i2c.h"
#include "n32h7xx_usart.h"
#include "n32h7xx_sdram.h"
#include "n32h7xx_i2s.h"
#include "n32h7xx_sdmmc.h"
#include "n32h7xx_exti.h"
#include "n32h7xx_pwr.h"
#include "n32h7xx_dmamux.h"
#include "n32h7xx_mdma.h"
#include "mmc_host.h"
#include "usbd_core.h"
#include "usbd_desc.h"
#include "usbd_user.h"

#include "usbd_msc_core.h"
#include "usbd_msc_bot.h"
#include "usbd_msc_storage.h"
#include "usbd_msc_scsi.h"
/* NTFx CODE END INCLUDE*/

/* NTFx CODE START Extern*/
extern  USB_CORE_MODULE USB1_Core;
extern void SysTick_Delayms(uint32_t Delayms);
extern void DMA_SetPerMemAddr(DMA_ChannelType* DMAChx, uint32_t periphAddr,uint32_t memAddr,uint32_t bufSize );
extern bool RCC_Configuration(void);
extern bool NVIC_Configuration(void);
extern bool DMA_Configuration(void);
extern bool GPIO_Configuration(void);
extern bool ETH_Configuration(void);
extern bool DVP_Configuration(void);
extern bool I2C_Configuration(void);
extern bool USART_Configuration(void);
extern bool SDRAM_Configuration(void);
extern bool I2S_Configuration(void);
extern bool USB_Configuration(void);
extern bool SDMMC_Configuration(void);
/* NTFx CODE END Extern*/

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H__ */
/**
 * @}
 */
