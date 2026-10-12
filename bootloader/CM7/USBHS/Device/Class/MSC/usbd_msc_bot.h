/**
 * @file usbd_msc_bot.h
 * @author N32cube
 */

#ifndef __USBD_MSC_BOT_H
#define __USBD_MSC_BOT_H

#include "usbd_core.h"
#include "usbd_msc.h"
/* NTFx CODE START */

#define BOT_IDLE                      0       /* Idle state */
#define BOT_DATA_OUT                  1       /* Data Out state */
#define BOT_DATA_IN                   2       /* Data In state */
#define BOT_LAST_DATA_IN              3       /* Last Data In Last */
#define BOT_SEND_DATA                 4       /* Send Immediate data */

#define BOT_CBW_SIGNATURE             0x43425355
#define BOT_CSW_SIGNATURE             0x53425355
#define BOT_CBW_LENGTH                31
#define BOT_CSW_LENGTH                13

/* CSW Status Definitions */
#define CSW_CMD_PASSED                0x00
#define CSW_CMD_FAILED                0x01
#define CSW_PHASE_ERROR               0x02

/* BOT Status */
#define BOT_STATE_NORMAL              0
#define BOT_STATE_RECOVERY            1
#define BOT_STATE_ERROR               2


#define DIR_IN                        0
#define DIR_OUT                       1
#define BOTH_DIR                      2

/* NTFx CODE END */

/* NTFx CODE START */
extern MSC_BOT_Info_TypeDef  USBD1_MSC_BOT_Info;
extern uint8_t USBD1MSC_BOT_Data[USBD1_MSC_MEDIA_PACKET];
/* NTFx CODE END */

void MSC_BOT_Init (USB_CORE_MODULE  *pdev,USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar);
void MSC_BOT_Reset (USB_CORE_MODULE  *pdev,USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar);
void MSC_BOT_DeInit (USB_CORE_MODULE  *pdev,USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar);
void MSC_BOT_DataIn (USB_CORE_MODULE  *pdev, uint8_t epnum,USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar);

void MSC_BOT_DataOut (USB_CORE_MODULE  *pdev, uint8_t epnum,USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar);

void  MSC_BOT_SendCSW (USB_CORE_MODULE  *pdev,USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar, uint8_t CSW_Status);

void  MSC_BOT_CplClrFeature (USB_CORE_MODULE  *pdev,USBD_MSC_GlobalVar_Info* USBD_MSC_GlobalVar, uint8_t epnum);

#endif /* __USBD_MSC_BOT_H */
