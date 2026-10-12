/* SPDX-License-Identifier: BSD-3-Clause */
/**
*\*\file n32eth_cfg.h
*\*\author N32cube
**/

#ifndef __N32ETH_CFG_H__
#define __N32ETH_CFG_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <string.h>
#include "n32h7xx.h"
#include "n32h7xx_rcc.h"
#include "n32h7xx_pwr.h"
#include "n32h7xx_gpio.h"
/* NTFx CODE START */

/* Tx Descriptor Length: Minimum is 4, maximum is 1024 */
#define ETH_TX_DESC_NUMBER          ((uint32_t)4U)
/* Rx Descriptor Length: Minimum is 4, maximum is 1024 */
#define ETH_RX_DESC_NUMBER          ((uint32_t)4U)

/* NTFx CODE END */
#ifdef __cplusplus
}
#endif

#endif /* __N32ETH_CFG_H__ */

