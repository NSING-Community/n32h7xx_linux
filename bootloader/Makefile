# SPDX-License-Identifier: BSD-3-Clause
######################################
# target
######################################
ifeq ($(DCORE), CM4)
TARGET = h787_linux_CM4
else
TARGET = h787_linux_CM7
endif

######################################
# building variables
######################################
# debug build
ifeq ($(release), y)
DEBUG = 0
else
DEBUG = 1
endif
# optimization
OPT = -O0

#######################################
# Parallel compilation jobs
#######################################
# Valid J range: minimum is 1 (= serial, no parallel).
# Best value is around the CPU logical core count; going far above it
# does not help and may slow the build down.
# Default is 8; override on command line with: make J=16
J ?= 8
MAKEFLAGS += -j$(J)

#######################################
# Boot medium: SPI NOR flash (default) or SDMMC
#######################################
# STORAGE=spi   -> XSPI2 + on-board W25Q128; MSC and the kernel image live there
# STORAGE=sdmmc -> SDMMC1 + microSD card (previous default)
#
# The backend is part of the build directory name so switching it cannot
# silently reuse objects compiled against the other backend.
STORAGE ?= spi

# Read-only SDMA/SDRAM comparison; never run the destructive legacy benchmark.
SD_COMPARE ?= 0

#######################################
# Optional on-board SPI flash benchmark
#######################################
# BENCH=1 builds a diagnostic image that stops after BOOT_StorageInit() and
# prints throughput for several XSPI read modes. It uses a separate build
# directory so the normal bootloader image is not overwritten.
BENCH ?= 0

#######################################
# Optional bootloader USART RX test
#######################################
# RX_TEST=1 builds a diagnostic image that configures PA10 as USART1_RX,
# waits for host bytes in both vendor input and AF_PP modes, then stops.
RX_TEST ?= 0
SDRAM_TEST ?= 0
SDRAM_TEST_DIV ?= 5
SDRAM_TEST_BURST ?= 2
SDRAM_TEST_BANK ?= 0
SDRAM_TEST_SPLIT ?= 0
SDRAM_BOOT_DIV ?= 5
MPU_GUARD ?= 1
BOOT_ICACHE ?= 1

#######################################
# Build path
#######################################
# All build output goes under OUT_DIR; nothing is written next to the sources.
# The top-level make passes OUT_DIR=<project>/out/bootloader explicitly; run
# directly, this defaults to the same directory, so both entries land in the
# project's single out/ tree instead of growing a second one under bootloader/.
OUT_DIR ?= $(abspath $(CURDIR)/..)/out/bootloader
BUILD_DIR = $(OUT_DIR)/CM7
ifeq ($(DCORE), CM4)
BUILD_DIR = $(OUT_DIR)/CM4
else
ifeq ($(BENCH),1)
BUILD_DIR = $(OUT_DIR)/CM7_bench
else
ifeq ($(RX_TEST),1)
BUILD_DIR = $(OUT_DIR)/CM7_rxtest
else
ifeq ($(STORAGE), sdmmc)
BUILD_DIR = $(OUT_DIR)/CM7_sdmmc
endif
endif
endif
endif

#####################################
# chip platform info
######################################
ifeq ($(SD_COMPARE),1)
ifneq ($(STORAGE),sdmmc)
$(error SD_COMPARE=1 requires STORAGE=sdmmc)
endif
BUILD_DIR := $(OUT_DIR)/CM7_sdcompare
DEFS += -DBOOT_SD_COMPARE
endif
ifneq ($(SDRAM_TEST),1)
DEFS += -DSDRAM_BOOT_PLL_DIV=$(SDRAM_BOOT_DIV)
ifneq ($(SDRAM_BOOT_DIV),5)
BUILD_DIR := $(BUILD_DIR)_sdramdiv$(SDRAM_BOOT_DIV)
endif
endif
ifeq ($(MPU_GUARD),1)
DEFS += -DBOOT_MPU_GUARD
else
BUILD_DIR := $(BUILD_DIR)_mpuunguarded
endif
DEFS += -DBOOT_ZIMAGE_ICACHE=$(BOOT_ICACHE)
ifeq ($(BOOT_ICACHE),0)
BUILD_DIR := $(BUILD_DIR)_no_boot_icache
endif
ifeq ($(SDRAM_TEST),1)
BUILD_DIR = $(OUT_DIR)/CM7_sdramtest_$(SDRAM_TEST_DIV)_b$(SDRAM_TEST_BURST)_i$(SDRAM_TEST_BANK)_s$(SDRAM_TEST_SPLIT)
DEFS += -DBOOT_SDRAM_READ_TEST -DSDRAM_TEST_PLL_DIV=$(SDRAM_TEST_DIV) -DSDRAM_TEST_BURST=$(SDRAM_TEST_BURST) -DSDRAM_TEST_BANK=$(SDRAM_TEST_BANK)
ifeq ($(SDRAM_TEST_SPLIT),1)
DEFS += -DBOOT_SDRAM_SPLIT_TEST
endif
endif

ifeq ($(DCORE), CM4)
TARGET_STARTUP  := n32h78x_cm4_cube
else
TARGET_STARTUP  := n32h78x_cm7_cube
endif
## NTFx CODE START Define ##
ifeq ($(DCORE), CM4)
DEFS += -DCORE_CM4
else
DEFS += -DCORE_CM7
DEFS += -DUSING_TCM
endif
DEFS += -DN32H78x
DEFS += -DUSE_STDPERIPH_DRIVER
DEFS += -DUSE_USBHS1
DEFS += -DUSE_USB_HS_IN_HS

ifeq ($(BENCH),1)
DEFS += -DSPIFLASH_BENCH
endif

ifeq ($(RX_TEST),1)
DEFS += -DBOOT_USART_RX_TEST
endif

ifeq ($(STORAGE), sdmmc)
DEFS += -DBOOT_STORAGE_SDMMC
EXCLUDE_SRC += CM7/src/spi_flash.c
else
DEFS += -DBOOT_STORAGE_SPI_FLASH
SRC_OBJS_DIRS += Driver/n32h7xx_std_periph_driver/src/n32h7xx_xspi_v2.c
EXCLUDE_SRC += CM7/src/sd_app.c
EXCLUDE_SRC += CM7/src/sd_bench.c
EXCLUDE_SRC += Driver/n32h7xx_periph_app/src/mmc_host.c
EXCLUDE_SRC += Driver/n32h7xx_periph_app/src/sd_host.c
EXCLUDE_SRC += Driver/n32h7xx_std_periph_driver/src/n32h7xx_sdmmc.c
endif
## NTFx CODE END Define ##
######################################
# Algo libs
######################################
#USELIB = 0

######################################
#TOOLS CHAIN
######################################
# The gcc path can be defined by 3 methods:
# 1.Define it in make command via GCC_PATH variable (> make GCC_PATH="D:\\Program Files (x86)\\GNU Arm Embedded Toolchain\\10 2020-q4-major\\bin\\")
# 2.Added it to the PATH environment variable
# 3.Define it in makefile ,such as :
#GCC_PATH = 'D:\Program Files (x86)\GNU Arm Embedded Toolchain\10 2020-q4-major\bin\'
ifdef GCC_PATH
CROSS_COMPILE = $(GCC_PATH)arm-none-eabi-
else 
CROSS_COMPILE = arm-none-eabi-
endif


######################################
# C sources
######################################
## NTFx CODE START C sources ##
ifeq ($(DCORE), CM4)
C_DIRS += CM4/src
else
C_DIRS += CM7/src
endif
C_DIRS += Driver/CMSIS/core
C_DIRS += Driver/CMSIS/device
C_DIRS += Driver/n32h7xx_periph_app/src
SRC_OBJS_DIRS += Driver/n32h7xx_std_periph_driver/src/n32h7xx_dbg.c
SRC_OBJS_DIRS += Driver/n32h7xx_std_periph_driver/src/n32h7xx_dcmu.c
SRC_OBJS_DIRS += Driver/n32h7xx_std_periph_driver/src/n32h7xx_eccmon.c
SRC_OBJS_DIRS += Driver/n32h7xx_std_periph_driver/src/n32h7xx_mmu.c
SRC_OBJS_DIRS += Driver/n32h7xx_std_periph_driver/src/n32h7xx_otpc.c
SRC_OBJS_DIRS += Driver/n32h7xx_std_periph_driver/src/n32h7xx_smu.c
SRC_OBJS_DIRS += Driver/n32h7xx_std_periph_driver/src/n32h7xx_vrefbuf.c
SRC_OBJS_DIRS += Driver/n32h7xx_std_periph_driver/src/n32h7xx_rcc.c
SRC_OBJS_DIRS += Driver/n32h7xx_std_periph_driver/src/misc.c
SRC_OBJS_DIRS += Driver/n32h7xx_std_periph_driver/src/n32h7xx_dma.c
SRC_OBJS_DIRS += Driver/n32h7xx_std_periph_driver/src/n32h7xx_gpio.c
SRC_OBJS_DIRS += Driver/n32h7xx_std_periph_driver/src/n32h7xx_eth.c
SRC_OBJS_DIRS += Driver/n32h7xx_std_periph_driver/src/n32h7xx_i2c.c
SRC_OBJS_DIRS += Driver/n32h7xx_std_periph_driver/src/n32h7xx_usart.c
SRC_OBJS_DIRS += Driver/n32h7xx_std_periph_driver/src/n32h7xx_sdram.c
SRC_OBJS_DIRS += Driver/n32h7xx_std_periph_driver/src/n32h7xx_i2s.c
SRC_OBJS_DIRS += Driver/n32h7xx_std_periph_driver/src/n32h7xx_sdmmc.c
SRC_OBJS_DIRS += Driver/n32h7xx_std_periph_driver/src/n32h7xx_exti.c
SRC_OBJS_DIRS += Driver/n32h7xx_std_periph_driver/src/n32h7xx_pwr.c
SRC_OBJS_DIRS += Driver/n32h7xx_std_periph_driver/src/n32h7xx_dmamux.c
SRC_OBJS_DIRS += Driver/n32h7xx_std_periph_driver/src/n32h7xx_mdma.c
ifeq ($(DCORE), CM4)
else
SRC_OBJS_DIRS += CM7/USBHS/Device/Class/MSC/usbd_msc_core.c
SRC_OBJS_DIRS += CM7/USBHS/Device/Class/MSC/usbd_msc_bot.c
SRC_OBJS_DIRS += CM7/USBHS/Device/Class/MSC/usbd_msc_storage.c
SRC_OBJS_DIRS += CM7/USBHS/Device/Class/MSC/usbd_msc_scsi.c
SRC_OBJS_DIRS += CM7/USBHS/Device/User/usbd_desc.c
SRC_OBJS_DIRS += CM7/USBHS/Device/User/usbd_user.c
SRC_OBJS_DIRS += CM7/USBHS/User/usbhs_bsp.c
SRC_OBJS_DIRS += Driver/n32h7xx_usbhs_driver/device/core/src/usbd_core.c
SRC_OBJS_DIRS += Driver/n32h7xx_usbhs_driver/device/core/src/usbd_ioreq.c
SRC_OBJS_DIRS += Driver/n32h7xx_usbhs_driver/device/core/src/usbd_req.c
SRC_OBJS_DIRS += Driver/n32h7xx_usbhs_driver/driver/src/usbhs_core.c
SRC_OBJS_DIRS += Driver/n32h7xx_usbhs_driver/driver/src/usbhs_dcd.c
SRC_OBJS_DIRS += Driver/n32h7xx_usbhs_driver/driver/src/usbhs_dcd_int.c
endif
## NTFx CODE END C sources ##

SRC_OBJS_DIRS += $(foreach DIR, $(C_DIRS), $(wildcard $(DIR)/*.c))
C_SOURCES = $(filter-out $(EXCLUDE_SRC), $(SRC_OBJS_DIRS))

######################################
# ASM sources
######################################
ASM_SOURCES =Driver/CMSIS/device/startup/startup_$(TARGET_STARTUP)_gcc.s

######################################
# C includes
######################################
## NTFx CODE START C includes ##
ifeq ($(DCORE), CM4)
C_INCS += CM4/inc
else
C_INCS += CM7/inc
endif
C_INCS += Driver/CMSIS/core
C_INCS += Driver/CMSIS/device
C_INCS += Driver/n32h7xx_std_periph_driver/inc
C_INCS += Driver/n32h7xx_periph_app/inc
ifeq ($(DCORE), CM4)
else
C_INCS += CM7/USBHS/Device/Class/MSC
C_INCS += CM7/USBHS/Device/User
C_INCS += CM7/USBHS/User
C_INCS += Driver/n32h7xx_usbhs_driver/device/core/inc
C_INCS += Driver/n32h7xx_usbhs_driver/driver/inc
endif
## NTFx CODE END C includes ##

INCS_OBJS_DIR = $(foreach DIR2, $(C_INCS), $(wildcard $(DIR2)/*.h))
INCS_OBJS_PATH = $(sort $(dir $(INCS_OBJS_DIR)))
C_INCLUDES = $(addprefix -I,$(INCS_OBJS_PATH))

######################################
# Lib files
######################################
ifeq ($(USELIB), 1)
SRC_LIB_DIR += Driver/n32h7xx_algo_lib/lib
INC_LIB_DIR += Driver/n32h7xx_algo_lib/inc
LIB_SOURCES = $(foreach DIR3, $(SRC_LIB_DIR), $(wildcard $(DIR3)/*.lib))
LIB_SOURCES_L = $(addprefix -L,$(LIB_SOURCES))
C_LIBS = $(LIB_SOURCES_L)
LIB_INCS = $(foreach DIR4, $(INC_LIB_DIR), $(wildcard $(DIR4)/*.h))
LIB_INCS_PATH = $(sort $(dir $(LIB_INCS)))
LIB_INCS_PATH_I = $(addprefix -I,$(LIB_INCS_PATH))
C_INCLUDES += $(LIB_INCS_PATH_I)
endif

######################################
# Compile & Link flags
######################################
# cpu
ifeq ($(DCORE), CM4)
CPU = -mcpu=cortex-m4
else
CPU = -mcpu=cortex-m7
endif
# mcu
MCU = $(CPU) -mthumb $(FPU) $(FLOAT-ABI)

#CFLAGS
CFLAGS += $(MCU) -Wall
CFLAGS += $(OPT)
CFLAGS += -ffunction-sections -fdata-sections
#DEBUG
ifeq ($(DEBUG), 1)
CFLAGS += -g -gdwarf-2
endif
# Generate dependency information
CFLAGS += -MMD -MP -MF"$(@:%.o=%.d)"

#LFLAGS
LFLAGS += $(MCU)
LFLAGS += -Wl,--gc-sections 
LFLAGS += --specs=nosys.specs
LFLAGS += -Xlinker -Map=$(BUILD_DIR)/$(TARGET).map
# link script
ifeq ($(DCORE), CM4)
LDSCRIPT = LinkFile/h787_linux_CM4.ld
else
LDSCRIPT = LinkFile/h787_linux_CM7.ld
endif

######################################
# Objects
######################################
all: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).hex $(BUILD_DIR)/$(TARGET).bin

# list of objects
OBJECTS = $(addprefix $(BUILD_DIR)/,$(notdir $(C_SOURCES:.c=.o)))
vpath %.c $(sort $(dir $(C_SOURCES)))
# list of ASM program objects
OBJECTS += $(addprefix $(BUILD_DIR)/,$(notdir $(ASM_SOURCES:.s=.o)))
vpath %.s $(sort $(dir $(ASM_SOURCES)))

$(BUILD_DIR)/%.o: %.c Makefile | $(BUILD_DIR) 
	@echo CC  $(notdir $<)
	@"$(CROSS_COMPILE)gcc" $(CFLAGS) $(DEFS) $(C_INCLUDES) $(C_LIBS) -c -Wa,-a,-ad,-alms=$(BUILD_DIR)/$(notdir $(<:.c=.lst)) $< -o $@
	
$(BUILD_DIR)/%.o: %.s Makefile | $(BUILD_DIR)
	@echo AS  $(notdir $<)
	@"$(CROSS_COMPILE)gcc" $(CFLAGS) -c $< -o $@
	
$(BUILD_DIR)/$(TARGET).elf: $(OBJECTS) Makefile
	@echo LD  $(TARGET).elf
	@"$(CROSS_COMPILE)gcc" $(OBJECTS) $(LFLAGS) -T$(LDSCRIPT) -o $@
	@"$(CROSS_COMPILE)size" $@
	
$(BUILD_DIR)/$(TARGET).bin: $(BUILD_DIR)/$(TARGET).elf
	"$(CROSS_COMPILE)objcopy" -O binary -S $< $@

$(BUILD_DIR)/$(TARGET).hex: $(BUILD_DIR)/$(TARGET).elf
	"$(CROSS_COMPILE)objcopy" -O ihex -S $< $@
	
$(BUILD_DIR):
	@-mkdir -p $@

# SHELL=cmd.exe
#######################################
# clean up
#######################################
clean:
	@-rm -rf $(BUILD_DIR)

#######################################
# dependencies
#######################################
-include $(wildcard $(BUILD_DIR)/*.d)

#######################################
# download .hex/.bin by jlink
#######################################
#Your JLink installation directory
# The jlink.exe path can be defined by 3 methods:
# 1.Define it in make command via JLINK_PATH variable (> make download JLINK_PATH="D:\\Program Files (x86)\\SEGGER\\JLink_V632g\\")
# 2.Added it to the PATH environment variable
# 3.Define it in makefile ,such as :
#JLINK_PATH = 'D:\Program Files (x86)\SEGGER\JLink_V632g\'
ifdef JLINK_PATH
JK_DPATH = $(JLINK_PATH)
else 
JK_DPATH=""
endif

#Jlink script store directory
JKS_DIR = JlinkCommanderScript
#Chip type
ifeq ($(DCORE), CM4)
CHIP_TYPE = N32H787xIx7:CM4
else
CHIP_TYPE = N32H787xIx7:CM7
endif

download_jlink:
ifeq ($(DCORE), CM4)
	@"$(JK_DPATH)JLink.exe" -device $(CHIP_TYPE) -if SWD -speed 4000 -autoconnect 1 -CommanderScript $(JKS_DIR)/h787_linux_CM4.jlink
else
	@"$(JK_DPATH)JLink.exe" -device $(CHIP_TYPE) -if SWD -speed 4000 -autoconnect 1 -CommanderScript $(JKS_DIR)/h787_linux_CM7.jlink
endif
	@echo "Download Completed!"

debug_jlink:
	@"$(JK_DPATH)JLinkGDBServer.exe" -select USB -device $(CHIP_TYPE) -if SWD -speed auto -noir -LocalhostOnly

#######################################
# download .bin by CMSIS-DAP (NSLINK)
#######################################
# The openocd.exe path can be defined by 3 methods:
# 1.Define it in make command via OPENOCD_PATH variable
# 2.Added it to the PATH environment variable
# 3.Define it in makefile ,such as :
#OPENOCD_PATH = D:/installVScodeBatTest/openocd-v0.12.0-i686-w64-mingw32/bin
# On POSIX the binary is plain "openocd"; on Windows it is openocd.exe, so
# point OPENOCD_PATH at its directory (or override OPENOCD outright) there.
ifdef OPENOCD_PATH
OPENOCD ?= $(OPENOCD_PATH)openocd.exe
else
OPENOCD ?= openocd
endif

# OpenOCD config file for CMSIS-DAP
ifeq ($(DCORE), CM4)
OPENOCD_CFG =OpenocdCfg/n32h7xx_cm4_cmsisdap.cfg
else
OPENOCD_CFG =OpenocdCfg/n32h7xx_cm7_cmsisdap.cfg
endif

download_cmsisdap: $(BUILD_DIR)/$(TARGET).bin
ifeq ($(DCORE), CM4)
	@"$(OPENOCD)" -d0 -f $(OPENOCD_CFG) \
		-c "init" \
		-c "reset halt" \
		-c "flash_program_bin $(BUILD_DIR)/$(TARGET).bin 0x15080000" \
		-c "reset run" \
		-c "shutdown" 2>/dev/null
else 
	@"$(OPENOCD)" -d0 -f $(OPENOCD_CFG) \
		-c "init" \
		-c "reset halt" \
		-c "flash_program_bin $(BUILD_DIR)/$(TARGET).bin 0x15000000" \
		-c "reset run" \
		-c "shutdown" 2>/dev/null
endif
	@echo "CMSIS-DAP Download Completed!"

debug_cmsisdap:
	@echo "======================================="
	@echo "Starting CMSIS-DAP GDB Server"
	@echo "Chip: $(CHIP_TYPE)"
	@echo "GDB port: 3333"
	@echo "Telnet port: 4444"
	@echo "Connect: arm-none-eabi-gdb $(BUILD_DIR)/$(TARGET).elf"
	@echo "  (gdb) target extended-remote localhost:3333"
	@echo "======================================="
	@"$(OPENOCD)" -d0 -f $(OPENOCD_CFG)

erase_cmsisdap:
	@"$(OPENOCD)" -d0 -f $(OPENOCD_CFG) \
		-c "init" \
		-c "reset init" \
		-c "flash_mass_erase" \
		-c "reset run" \
		-c "shutdown"
	@echo "CMSIS-DAP Mass Erase Completed!"
# *** EOF ***
