# SPDX-License-Identifier: BSD-3-Clause

project_dir := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
CROSS_COMPILE ?= ../../toolchain/bin/arm-uclinuxfdpiceabi-
CC := $(CROSS_COMPILE)gcc
CXX := $(CROSS_COMPILE)g++
AR := $(CROSS_COMPILE)ar
RANLIB := $(CROSS_COMPILE)ranlib

TFLM_DIR := $(project_dir)/third_party/tflite-micro
CMSIS_NN_DIR := $(project_dir)/third_party/cmsis-nn
N32JPEG_DIR := $(project_dir)/../../n32jpeg
N32JPEG_LIB := $(N32JPEG_DIR)/libn32jpeg.a
OBJ_DIR := $(project_dir)/obj
BIN := $(project_dir)/human-detect

CFLAGS ?= -O2 -Wall -Wextra
CXXFLAGS ?= -O2 -Wall -Wextra
CMSIS_NN_CFLAGS ?= -O2 -Wall -Wextra
CFLAGS += -ffunction-sections -fdata-sections
CXXFLAGS += -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti \
	-fno-threadsafe-statics -fno-unwind-tables -fno-asynchronous-unwind-tables \
	-fno-use-cxa-atexit
CMSIS_NN_CFLAGS += -ffunction-sections -fdata-sections
TFLM_STRIP_ERRORS ?= 1
CPPFLAGS += -I$(project_dir) -I$(TFLM_DIR) \
	-I$(project_dir)/third_party/flatbuffers/include \
	-I$(project_dir)/third_party/gemmlowp \
	-I$(CMSIS_NN_DIR) -I$(CMSIS_NN_DIR)/Include \
	-I$(N32JPEG_DIR) \
	-DCMSIS_NN \
	-DARM_NN_ENABLE_F16=0 -DARM_NN_ENABLE_F32=0 \
	-DTF_LITE_STATIC_MEMORY \
	-DTF_LITE_DISABLE_X86_NEON
ifeq ($(TFLM_STRIP_ERRORS),1)
CPPFLAGS += -DTF_LITE_STRIP_ERROR_STRINGS
endif
# This daemon uses no C++ runtime facilities that need libstdc++.  Linking it
# as a C program avoids a 1.3 MiB shared-library load, which is significant on
# a no-MMU target where the loader needs one large contiguous mapping.
LDFLAGS += -Wl,--gc-sections -Wl,-rpath,/system/lib -nostdlib++ -static-libgcc

TFLM_SOURCES := \
	tensorflow/lite/micro/debug_log.cc \
	tensorflow/lite/micro/fake_micro_context.cc \
	tensorflow/lite/micro/flatbuffer_utils.cc \
	tensorflow/lite/micro/hexdump.cc \
	tensorflow/lite/micro/memory_helpers.cc \
	tensorflow/lite/micro/micro_allocation_info.cc \
	tensorflow/lite/micro/micro_allocator.cc \
	tensorflow/lite/micro/micro_context.cc \
	tensorflow/lite/micro/micro_interpreter.cc \
	tensorflow/lite/micro/micro_interpreter_context.cc \
	tensorflow/lite/micro/micro_interpreter_graph.cc \
	tensorflow/lite/micro/micro_log.cc \
	tensorflow/lite/micro/micro_op_resolver.cc \
	tensorflow/lite/micro/micro_profiler.cc \
	tensorflow/lite/micro/micro_resource_variable.cc \
	tensorflow/lite/micro/micro_time.cc \
	tensorflow/lite/micro/micro_utils.cc \
	tensorflow/lite/micro/mock_micro_graph.cc \
	tensorflow/lite/micro/recording_micro_allocator.cc \
	tensorflow/lite/micro/system_setup.cc \
	tensorflow/lite/micro/test_helper_custom_ops.cc \
	tensorflow/lite/micro/test_helpers.cc \
	tensorflow/lite/micro/arena_allocator/non_persistent_arena_buffer_allocator.cc \
	tensorflow/lite/micro/arena_allocator/persistent_arena_buffer_allocator.cc \
	tensorflow/lite/micro/arena_allocator/recording_single_arena_buffer_allocator.cc \
	tensorflow/lite/micro/arena_allocator/single_arena_buffer_allocator.cc \
	tensorflow/lite/micro/memory_planner/greedy_memory_planner.cc \
	tensorflow/lite/micro/memory_planner/linear_memory_planner.cc \
	tensorflow/lite/micro/memory_planner/non_persistent_buffer_planner_shim.cc \
	tensorflow/lite/micro/tflite_bridge/flatbuffer_conversions_bridge.cc \
	tensorflow/lite/micro/tflite_bridge/micro_error_reporter.cc \
	tensorflow/compiler/mlir/lite/core/api/error_reporter.cc \
	tensorflow/compiler/mlir/lite/schema/schema_utils.cc \
	tensorflow/lite/core/api/tensor_utils.cc \
	tensorflow/lite/core/api/flatbuffer_conversions.cc \
	tensorflow/lite/core/c/common.cc \
	tensorflow/lite/kernels/internal/portable_tensor_utils.cc \
	tensorflow/lite/kernels/internal/tensor_utils.cc \
	tensorflow/lite/kernels/internal/reference/portable_tensor_utils.cc \
	tensorflow/lite/kernels/internal/common.cc \
	tensorflow/lite/kernels/internal/quantization_util.cc \
	tensorflow/lite/kernels/internal/tensor_ctypes.cc \
	tensorflow/lite/kernels/internal/runtime_shape.cc \
	tensorflow/lite/kernels/kernel_util.cc \
	tensorflow/lite/micro/kernels/cmsis_nn/conv.cc \
	tensorflow/lite/micro/kernels/conv_common.cc \
	tensorflow/lite/micro/kernels/cmsis_nn/depthwise_conv.cc \
	tensorflow/lite/micro/kernels/depthwise_conv_common.cc \
	tensorflow/lite/micro/kernels/micro_tensor_utils.cc \
	tensorflow/lite/micro/kernels/kernel_util.cc

TFLM_OBJECTS := $(addprefix $(OBJ_DIR)/tflm/,$(TFLM_SOURCES:.cc=.o))
CMSIS_NN_SOURCE_DIRS := \
	$(CMSIS_NN_DIR)/Source/ActivationFunctions \
	$(CMSIS_NN_DIR)/Source/BasicMathFunctions \
	$(CMSIS_NN_DIR)/Source/ConcatenationFunctions \
	$(CMSIS_NN_DIR)/Source/ConvolutionFunctions \
	$(CMSIS_NN_DIR)/Source/FullyConnectedFunctions \
	$(CMSIS_NN_DIR)/Source/LSTMFunctions \
	$(CMSIS_NN_DIR)/Source/NNSupportFunctions \
	$(CMSIS_NN_DIR)/Source/PadFunctions \
	$(CMSIS_NN_DIR)/Source/PoolingFunctions \
	$(CMSIS_NN_DIR)/Source/ReshapeFunctions \
	$(CMSIS_NN_DIR)/Source/SVDFunctions \
	$(CMSIS_NN_DIR)/Source/SoftmaxFunctions \
	$(CMSIS_NN_DIR)/Source/TransposeFunctions
CMSIS_NN_SOURCES := $(foreach dir,$(CMSIS_NN_SOURCE_DIRS),$(wildcard $(dir)/*.c))
CMSIS_NN_OBJECTS := $(patsubst $(CMSIS_NN_DIR)/%.c,$(OBJ_DIR)/cmsis-nn/%.o,$(CMSIS_NN_SOURCES))
PROJECT_OBJECTS := \
	$(OBJ_DIR)/detector.o \
	$(OBJ_DIR)/service.o \
	$(OBJ_DIR)/model/detect_model_data.o

.PHONY: all clean
all: $(BIN)

$(BIN): $(PROJECT_OBJECTS) $(TFLM_OBJECTS) $(CMSIS_NN_OBJECTS) $(N32JPEG_LIB)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(N32JPEG_LIB): $(N32JPEG_DIR)/n32jpeg.c $(N32JPEG_DIR)/n32jpeg.h \
		$(N32JPEG_DIR)/Makefile
	$(MAKE) -C $(N32JPEG_DIR) libn32jpeg.a CC="$(CROSS_COMPILE)gcc" \
		AR="$(AR)" RANLIB="$(RANLIB)"

$(OBJ_DIR)/tflm/%.o: $(TFLM_DIR)/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Wno-unused-parameter \
		-Wno-missing-field-initializers -MMD -MP -c $< -o $@

$(OBJ_DIR)/cmsis-nn/%.o: $(CMSIS_NN_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CMSIS_NN_CFLAGS) -Wno-unused-parameter \
		-MMD -MP -c $< -o $@

$(OBJ_DIR)/%.o: $(project_dir)/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(PROJECT_OBJECTS:.o=.d) $(TFLM_OBJECTS:.o=.d) \
	$(CMSIS_NN_OBJECTS:.o=.d)

clean:
	rm -rf $(OBJ_DIR) $(BIN)
