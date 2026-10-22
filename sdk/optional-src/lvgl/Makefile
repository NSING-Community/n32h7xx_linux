# SPDX-License-Identifier: MIT
#
# LVGL v9.6.0 for the N32H787 Linux framebuffer.
#
# Builds a static library and the LVGL demo program from the unmodified
# upstream tree.  Objects go under $(OUT_DIR)/lvgl, never next to the sources.
#
# Only what this board uses is compiled:
#   * src/**/*.c except src/libs/ -- every optional decoder is disabled in
#     lv_conf.h and those trees pull in third-party headers;
#   * only the Linux fbdev and DRM display drivers -- the other drivers target
#     panels this board does not have;
#   * only the Montserrat sizes enabled in lv_conf.h.  The other bundled font
#     sources define their own LV_FONT_MONTSERRAT_N macro, so compiling all of
#     src/font would pull every size into the binary regardless of lv_conf.h.
#
# The board is NOMMU: each PT_LOAD needs one physically contiguous block when
# exec runs.  -Os, -ffunction-sections and --gc-sections keep the text segment
# inside the 512 KiB buddy bucket that is still available after boot; check
# with `readelf -l $(OUT_DIR)/lvgl/lvgl-demo` after changes.

lvgl_dir := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
upstream := $(lvgl_dir)/upstream
TOP      := $(abspath $(lvgl_dir)/../../..)

OUT_DIR    ?= $(TOP)/out
LVGL_BUILD ?= $(OUT_DIR)/lvgl
LIBDRM_ROOT ?= $(OUT_DIR)/libdrm/rootfs
LIBDRM_SONAME := $(LIBDRM_ROOT)/usr/lib/libdrm.so.2

CROSS_COMPILE ?= $(TOP)/sdk/toolchain/bin/arm-uclinuxfdpiceabi-
CC := $(CROSS_COMPILE)gcc
AR := $(CROSS_COMPILE)ar

CPPFLAGS += -DLV_CONF_INCLUDE_SIMPLE -I$(lvgl_dir) -I$(upstream) \
            -I$(LIBDRM_ROOT)/usr/include -I$(LIBDRM_ROOT)/usr/include/libdrm \
            -idirafter $(TOP)/kernel/include/uapi
CFLAGS   ?= -Os -Wall -Wextra
CFLAGS   += -ffunction-sections -fdata-sections
LDFLAGS  += -Wl,--gc-sections -Wl,-rpath,/usr/lib
LDLIBS   += -L$(LIBDRM_ROOT)/usr/lib -ldrm -lm

# --- source selection -------------------------------------------------------
lvgl_all   := $(shell find $(upstream)/src -name '*.c')
lvgl_nolib := $(filter-out $(upstream)/src/libs/%,$(lvgl_all))
lvgl_nodrv := $(filter-out $(upstream)/src/drivers/%,$(lvgl_nolib))
lvgl_font  := $(upstream)/src/font/lv_font.c \
              $(shell find $(upstream)/src/font -mindepth 2 -name '*.c') \
              $(upstream)/src/font/lv_font_montserrat_14.c \
              $(upstream)/src/font/lv_font_montserrat_20.c \
              $(upstream)/src/font/lv_font_montserrat_24.c \
              $(upstream)/src/font/lv_font_montserrat_26.c
lvgl_srcs  := $(filter-out $(upstream)/src/font/%,$(lvgl_nodrv)) $(lvgl_font) \
              $(upstream)/src/drivers/display/fb/lv_linux_fbdev.c \
              $(upstream)/src/drivers/display/drm/lv_linux_drm.c \
              $(upstream)/src/drivers/evdev/lv_evdev.c

lvgl_objs := $(patsubst $(upstream)/%.c,$(LVGL_BUILD)/obj/%.o,$(lvgl_srcs))
lvgl_deps := $(lvgl_objs:.o=.d)
LVGL_LIB  := $(LVGL_BUILD)/liblvgl.a

# --- demo program -----------------------------------------------------------
DEMO_SRC := $(TOP)/sdk/example/lvgl-demo.c
GPU_DEMO_SRC := $(TOP)/sdk/example/lvgl-gpu-backend.c
DEMO_OBJ := $(LVGL_BUILD)/demo/lvgl-demo.o
GPU_DEMO_OBJ := $(LVGL_BUILD)/demo/lvgl-gpu-backend.o
DEMO_BIN := $(LVGL_BUILD)/lvgl-demo
TOUCH_DEMO_SRC := $(TOP)/sdk/example/lvgl-touch-demo.c
TOUCH_DEMO_OBJ := $(LVGL_BUILD)/demo/lvgl-touch-demo.o
TOUCH_DEMO_BIN := $(LVGL_BUILD)/lvgl-touch-demo
HUMAN_UI_DIR := $(TOP)/sdk/optional-src/human-detect
HUMAN_UI_SRC := $(HUMAN_UI_DIR)/ui.c
HUMAN_UI_OBJ := $(LVGL_BUILD)/optional/human-detect-ui.o
HUMAN_UI_BIN := $(LVGL_BUILD)/human-detect-ui

.PHONY: all lib touch-demo human-detect-ui clean
all: $(DEMO_BIN) $(TOUCH_DEMO_BIN)

lib: $(LVGL_LIB)

touch-demo: $(TOUCH_DEMO_BIN)

human-detect-ui: $(HUMAN_UI_BIN)

$(LVGL_LIB): $(lvgl_objs)
	@mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ $^

$(LVGL_BUILD)/obj/%.o: $(upstream)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(DEMO_OBJ): $(DEMO_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(GPU_DEMO_OBJ): $(GPU_DEMO_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(TOUCH_DEMO_OBJ): $(TOUCH_DEMO_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(HUMAN_UI_OBJ): $(HUMAN_UI_SRC) $(HUMAN_UI_DIR)/preview-gray8.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -I$(HUMAN_UI_DIR) -I$(TOP)/sdk/example \
		$(CFLAGS) -MMD -MP -c $< -o $@

$(LIBDRM_SONAME):
	$(MAKE) -C $(TOP)/sdk/optional-src/libdrm OUT_DIR=$(OUT_DIR) \
		CROSS_COMPILE=$(CROSS_COMPILE)

$(DEMO_BIN): $(DEMO_OBJ) $(GPU_DEMO_OBJ) $(LVGL_LIB) | $(LIBDRM_SONAME)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@
	@echo
	@$(CROSS_COMPILE)size $@

$(TOUCH_DEMO_BIN): $(TOUCH_DEMO_OBJ) $(GPU_DEMO_OBJ) $(LVGL_LIB) | $(LIBDRM_SONAME)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@
	@echo
	@$(CROSS_COMPILE)size $@

$(HUMAN_UI_BIN): $(HUMAN_UI_OBJ) $(GPU_DEMO_OBJ) $(LVGL_LIB) | $(LIBDRM_SONAME)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@
	@echo
	@$(CROSS_COMPILE)size $@

clean:
	rm -rf $(LVGL_BUILD)

-include $(lvgl_deps) $(DEMO_OBJ:.o=.d) $(GPU_DEMO_OBJ:.o=.d) \
	$(TOUCH_DEMO_OBJ:.o=.d) $(HUMAN_UI_OBJ:.o=.d)
