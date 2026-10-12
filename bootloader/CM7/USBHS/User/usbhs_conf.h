/**
 * @file usbhs_conf.h
 * @author N32cube
 */
#ifndef __USBHS_CONF__H__
#define __USBHS_CONF__H__
/* NTFx CODE START */
/** USBHS CONFIGURATION **/
#define RX_FIFO_HS_SIZE                          512
#define TX0_FIFO_HS_SIZE                         64
#define TX1_FIFO_HS_SIZE                         256
#define TX2_FIFO_HS_SIZE                         64
#define TX3_FIFO_HS_SIZE                         64
#define TX4_FIFO_HS_SIZE                         64
#define TX5_FIFO_HS_SIZE                         0
#define TX6_FIFO_HS_SIZE                         0
#define TX7_FIFO_HS_SIZE                         0
#define TX8_FIFO_HS_SIZE                         0

#if ((RX_FIFO_HS_SIZE + \
     TX0_FIFO_HS_SIZE + TX1_FIFO_HS_SIZE + TX2_FIFO_HS_SIZE + TX3_FIFO_HS_SIZE + TX4_FIFO_HS_SIZE + \
     TX5_FIFO_HS_SIZE + TX6_FIFO_HS_SIZE + TX7_FIFO_HS_SIZE + TX8_FIFO_HS_SIZE) > 1024U)
#error  "The USB max FIFO size is 1024 x 4 Bytes!"
#endif



#define USB_INTERNAL_DMA_ENABLED



/** USBHS MODE CONFIGURATION **/
#define USE_DEVICE_MODE
#ifndef USE_DEVICE_MODE
 #ifndef USE_HOST_MODE
    #error  "USE_DEVICE_MODE or USE_HOST_MODE should be defined"
 #endif
#endif
/* NTFx CODE END */


/****************** C Compilers dependant keywords ****************************/
/* In HS mode and when the DMA is used, all variables and data structures dealing
   with the DMA during the transaction process should be 4-bytes aligned */    
#ifdef USB_INTERNAL_DMA_ENABLED
  #if defined   (__GNUC__)        /* GNU Compiler */
    #define __ALIGN_END    __attribute__ ((aligned (4)))
    #define __ALIGN_BEGIN         
  #else                           
    #define __ALIGN_END
    #if defined   (__CC_ARM)      /* ARM Compiler */
      #define __ALIGN_BEGIN    __align(4)  
    #elif defined (__ICCARM__)    /* IAR Compiler */
      #define __ALIGN_BEGIN
    #endif /* __CC_ARM */  
  #endif /* __GNUC__ */ 
#else
  #define __ALIGN_BEGIN
  #define __ALIGN_END   
#endif /* USB_INTERNAL_DMA_ENABLED */

/* __packed keyword used to decrease the data type alignment to 1-byte */
#if defined (__CC_ARM)         /* ARM Compiler */
  #define __packed    __packed
#elif defined (__ICCARM__)     /* IAR Compiler */
  #define __packed    __packed
#elif defined   ( __GNUC__ )   /* GNU Compiler */                        
  #define __packed    __attribute__ ((__packed__))
#endif /* __CC_ARM */

#endif //__USBHS_CONF__H__

