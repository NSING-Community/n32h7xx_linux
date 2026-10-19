/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * QVGA integer human detector.  The model input is the same 320x240 GREY
 * frame captured by the service, so no resize is needed.
 */
#include "detector.h"

#include <stdint.h>

#include "model/detect_lut.h"
#include "model/detect_model_data.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

namespace {

constexpr int kRows = 30;
constexpr int kCols = 40;
constexpr int kStride = 8;
constexpr int kFrameWidth = 320;
constexpr int kFrameHeight = 240;
constexpr size_t kTensorArenaBytes = 400 * 1024;
constexpr int kMaxCandidates = 64;

alignas(16) uint8_t tensor_arena[kTensorArenaBytes];

struct Candidate {
	int16_t x0;
	int16_t y0;
	int16_t x1;
	int16_t y1;
	uint8_t score;
};

bool is_tensor_shape(const TfLiteTensor *tensor, int height, int width,
		     int channels)
{
	return tensor && tensor->type == kTfLiteInt8 &&
	       tensor->dims->size == 4 && tensor->dims->data[0] == 1 &&
	       tensor->dims->data[1] == height &&
	       tensor->dims->data[2] == width &&
	       tensor->dims->data[3] == channels;
}

int clamp_int(int value, int low, int high)
{
	if (value < low)
		return low;
	if (value > high)
		return high;
	return value;
}

/* The model is trained so that nested boxes describe the same person.  Using
 * overlap of the smaller box avoids dropping one of those duplicates merely
 * because the outer box makes ordinary IoU look small. */
int32_t overlap_of_smaller(const Candidate &a, const Candidate &b)
{
	const int32_t left = a.x0 > b.x0 ? a.x0 : b.x0;
	const int32_t top = a.y0 > b.y0 ? a.y0 : b.y0;
	const int32_t right = a.x1 < b.x1 ? a.x1 : b.x1;
	const int32_t bottom = a.y1 < b.y1 ? a.y1 : b.y1;

	if (right <= left || bottom <= top)
		return 0;

	const int32_t intersection = (right - left) * (bottom - top);
	const int32_t area_a = (a.x1 - a.x0) * (a.y1 - a.y0);
	const int32_t area_b = (b.x1 - b.x0) * (b.y1 - b.y0);
	const int32_t smaller = area_a < area_b ? area_a : area_b;

	if (smaller <= 0)
		return 0;
	return (intersection * 1024) / smaller;
}

class HumanDetector {
public:
	HumanDetector()
		: model_(tflite::GetModel(g_detect_model_data)),
		  interpreter_(model_, resolver_, tensor_arena, sizeof(tensor_arena))
	{
		resolver_.AddConv2D();
		resolver_.AddDepthwiseConv2D();
	}

	bool init()
	{
		if (initialized_)
			return init_error_ == 0;

		initialized_ = true;
		if (!model_ || model_->version() != TFLITE_SCHEMA_VERSION) {
			init_error_ = 1;
			return false;
		}
		if (interpreter_.AllocateTensors() != kTfLiteOk) {
			init_error_ = 2;
			return false;
		}

		input_ = interpreter_.input(0);
		if (!is_tensor_shape(input_, kFrameHeight, kFrameWidth, 1)) {
			init_error_ = 4;
			return false;
		}

		for (size_t i = 0; i < interpreter_.outputs_size(); ++i) {
			TfLiteTensor *output = interpreter_.output(i);

			if (is_tensor_shape(output, kRows, kCols, 1))
				objectness_ = output;
			else if (is_tensor_shape(output, kRows, kCols, 4))
				box_ = output;
		}
		if (!objectness_ || !box_) {
			init_error_ = 5;
			return false;
		}
		init_error_ = 0;
		return true;
	}

	bool prepare_gray8(const uint8_t *gray8, size_t stride)
	{
		if (init_error_ != 0 || !input_ || !gray8 ||
		    stride < static_cast<size_t>(kFrameWidth))
			return false;

		int8_t *destination = input_->data.int8;
		for (int row = 0; row < kFrameHeight; ++row) {
			const uint8_t *source = gray8 + row * stride;
			for (int col = 0; col < kFrameWidth; ++col)
				destination[row * kFrameWidth + col] =
					static_cast<int8_t>(
						static_cast<int>(source[col]) - 128);
		}
		return true;
	}

	bool invoke(struct human_detect_result *result)
	{
		if (init_error_ != 0 || !result)
			return false;
		if (interpreter_.Invoke() != kTfLiteOk)
			return false;
		decode(result);
		return true;
	}

	int init_error() const { return init_error_; }
	size_t arena_bytes() const { return interpreter_.arena_used_bytes(); }

	void set_threshold(unsigned int threshold)
	{
		threshold_ = threshold > 255 ? 255 : threshold;
	}

private:
	void decode(struct human_detect_result *result)
	{
		const int8_t *scores = objectness_->data.int8;
		const int8_t *distances = box_->data.int8;
		Candidate candidates[kMaxCandidates];
		int candidate_count = 0;
		int weakest = 0;

		result->count = 0;
		for (int cell = 0; cell < kRows * kCols; ++cell) {
			const uint8_t score =
				kDetectScoreLut[scores[cell] + 128];
			if (score < threshold_)
				continue;

			int slot;
			if (candidate_count < kMaxCandidates) {
				slot = candidate_count++;
			} else {
				if (score <= candidates[weakest].score)
					continue;
				slot = weakest;
			}

			const int row = cell / kCols;
			const int col = cell % kCols;
			const int center_x = col * kStride + kStride / 2;
			const int center_y = row * kStride + kStride / 2;
			const int8_t *box = distances + cell * 4;
			const int left = kDetectDistanceLut[box[0] + 128];
			const int top = kDetectDistanceLut[box[1] + 128];
			const int right = kDetectDistanceLut[box[2] + 128];
			const int bottom = kDetectDistanceLut[box[3] + 128];
			Candidate &candidate = candidates[slot];

			candidate.x0 = static_cast<int16_t>(clamp_int(
				center_x - left, 0, kFrameWidth - 1));
			candidate.y0 = static_cast<int16_t>(clamp_int(
				center_y - top, 0, kFrameHeight - 1));
			candidate.x1 = static_cast<int16_t>(clamp_int(
				center_x + right, 1, kFrameWidth));
			candidate.y1 = static_cast<int16_t>(clamp_int(
				center_y + bottom, 1, kFrameHeight));
			candidate.score = score;

			weakest = 0;
			for (int i = 1; i < candidate_count; ++i) {
				if (candidates[i].score <
				    candidates[weakest].score)
					weakest = i;
			}
		}

		for (int i = 0; i < candidate_count; ++i) {
			int best = i;

			for (int j = i + 1; j < candidate_count; ++j) {
				if (candidates[j].score > candidates[best].score)
					best = j;
			}
			if (best != i) {
				const Candidate swap = candidates[i];
				candidates[i] = candidates[best];
				candidates[best] = swap;
			}
		}

			int survivors[HUMAN_DETECT_MAX_BOXES];
		int kept = 0;
		for (int i = 0; i < candidate_count &&
		     kept < HUMAN_DETECT_MAX_BOXES; ++i) {
			bool suppressed = false;

			for (int j = 0; j < kept; ++j) {
				if (overlap_of_smaller(candidates[i],
						       candidates[survivors[j]]) > 512) {
					suppressed = true;
					break;
				}
			}
			if (suppressed)
				continue;

			const Candidate &winner = candidates[i];
			struct human_detect_box &box = result->boxes[kept];
			box.x0 = static_cast<uint16_t>(
				winner.x0 * 65535 / kFrameWidth);
			box.y0 = static_cast<uint16_t>(
				winner.y0 * 65535 / kFrameHeight);
			box.x1 = static_cast<uint16_t>(
				winner.x1 * 65535 / kFrameWidth);
			box.y1 = static_cast<uint16_t>(
				winner.y1 * 65535 / kFrameHeight);
			box.score = winner.score;
			survivors[kept++] = i;
		}
		result->count = static_cast<uint8_t>(kept);
	}

	const tflite::Model *model_;
	tflite::MicroMutableOpResolver<2> resolver_;
	tflite::MicroInterpreter interpreter_;
	TfLiteTensor *input_ = nullptr;
	TfLiteTensor *objectness_ = nullptr;
	TfLiteTensor *box_ = nullptr;
	bool initialized_ = false;
	int init_error_ = -1;
	uint8_t threshold_ = 102;
};

alignas(16) HumanDetector detector;

} // namespace

extern "C" int human_detector_init(void)
{
	return detector.init() ? 0 : -1;
}

extern "C" int human_detector_prepare_gray8(const uint8_t *gray8, size_t stride)
{
	return detector.prepare_gray8(gray8, stride) ? 0 : -1;
}

extern "C" int human_detector_invoke(struct human_detect_result *result)
{
	return detector.invoke(result) ? 0 : -1;
}

extern "C" int human_detector_set_threshold(unsigned int threshold)
{
	detector.set_threshold(threshold);
	return 0;
}

extern "C" size_t human_detector_arena_bytes(void)
{
	return detector.arena_bytes();
}

extern "C" const char *human_detector_error_string(void)
{
	switch (detector.init_error()) {
	case 0:
		return "ready";
	case 1:
		return "unsupported model schema";
	case 2:
		return "tensor arena allocation failed";
	case 4:
		return "unexpected model input tensor";
	case 5:
		return "unexpected model output tensors";
	default:
		return "detector is not initialized";
	}
}
