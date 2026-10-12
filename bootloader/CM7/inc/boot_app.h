/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file boot_app.h
 * @brief Boot policy and kernel hand-off.
 */
#ifndef __BOOT_APP_H__
#define __BOOT_APP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

/** SDRAM address the kernel partition is copied to; the zImage starts here. */
#define BOOT_KERNEL_LOAD_ADDR 0xC0000000U

/**
 * Enter USB MSC if KEY1 (PG3) is held at reset or a DAP/debugger wrote the
 * hand-off flag.  Otherwise read the dtb and kernel partitions from the boot
 * medium, place the kernel at BOOT_KERNEL_LOAD_ADDR and the DTB high in the
 * same SDRAM window, and enter the zImage with r0=0, r1=~0, r2=DTB address.
 *
 * The medium is laid out dtb / kernel / rootfs / system; the bootloader reads
 * only the first two, because Linux reaches rootfs and system through MTD at
 * run time.
 */
void BOOT_App_Run(void);

/** True when KEY1 is held or the debugger hand-off flag is present. */
bool BOOT_MscRequested(void);

/** Configure PG3 and sample KEY1; consumes the debugger flag when present. */
void BOOT_InputInit(void);

#ifdef __cplusplus
}
#endif

#endif /* __BOOT_APP_H__ */
