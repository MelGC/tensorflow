/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#ifndef TENSORFLOW_CONTRIB_LITE_EXPERIMENTAL_RISCV_KERNELS_OPTIMIZED_CONV_FLOAT_H_
#define TENSORFLOW_CONTRIB_LITE_EXPERIMENTAL_RISCV_KERNELS_OPTIMIZED_CONV_FLOAT_H_

#include "tensorflow/contrib/lite/experimental/riscv/kernels/common.h"
#include "tensorflow/contrib/lite/kernels/internal/types.h"
#include "tensorflow/contrib/lite/experimental/riscv/kernels/optimized/intrinsic/riscv_ml_extension.h"
#include "tensorflow/contrib/lite/experimental/riscv/kernels/optimized/optimized_ops_float.h"

namespace tflite {
namespace optimized_ops {

#ifdef RISCV
inline void ConvIm2Col(const ConvParams& params, const RuntimeShape& input_shape,
                 const float* input_data, const RuntimeShape& filter_shape,
                 const float* filter_data, const RuntimeShape& bias_shape,
                 const float* bias_data, const RuntimeShape& output_shape,
                 float* output_data, const RuntimeShape& im2col_shape,
                 float* im2col_data) {
  const int stride_width = params.stride_width;
  const int stride_height = params.stride_height;
  const int dilation_width_factor = params.dilation_width_factor;
  const int dilation_height_factor = params.dilation_height_factor;
  const float output_activation_min = params.float_activation_min;
  const float output_activation_max = params.float_activation_max;
  TFLITE_DCHECK_EQ(input_shape.DimensionsCount(), 4);
  TFLITE_DCHECK_EQ(filter_shape.DimensionsCount(), 4);
  TFLITE_DCHECK_EQ(output_shape.DimensionsCount(), 4);

  (void)im2col_data;
  (void)im2col_shape;
  // gemmlowp::ScopedProfilingLabel label("Conv");

  // NB: static_cast<float>(0x00000000h) == 0.0f
  const uint8 float_zero_byte = 0x00;
  const float* gemm_input_data = nullptr;
  const RuntimeShape* gemm_input_shape = nullptr;
  const int filter_width = filter_shape.Dims(2);
  const int filter_height = filter_shape.Dims(1);
  const bool need_dilated_im2col =
      dilation_width_factor != 1 || dilation_height_factor != 1;
  const bool need_im2col = stride_width != 1 || stride_height != 1 ||
                           filter_width != 1 || filter_height != 1;
  //  const bool use_kernel1x1 = stride_width == 1 && stride_height == 1 &&
  //                       filter_width == 1 && filter_height == 1;
  if (need_dilated_im2col) {
    DilatedIm2col(params, float_zero_byte, input_shape, input_data,
                  filter_shape, output_shape, im2col_data);
    gemm_input_data = im2col_data;
    gemm_input_shape = &im2col_shape;
  } else if (need_im2col) {
    TFLITE_DCHECK(im2col_data);
    Im2col(params, filter_height, filter_width, float_zero_byte, input_shape,
           input_data, im2col_shape, im2col_data);
    gemm_input_data = im2col_data;
    gemm_input_shape = &im2col_shape;
  } else {
    // TODO(aselle): We need to make sure to not send im2col if it is not
    // needed.
    TFLITE_DCHECK(!im2col_data);
    gemm_input_data = input_data;
    gemm_input_shape = &input_shape;
  }

  const Dims<4>& gemm_input_dims = ToRuntimeDims(*gemm_input_shape);
  const Dims<4>& output_dims = ToRuntimeDims(output_shape);
  const Dims<4>& filter_dims = ToRuntimeDims(filter_shape);
  const Dims<4>& bias_dims = ToRuntimeDims(bias_shape);

  const int batches = MatchingArraySize(gemm_input_dims, 3, output_dims, 3);
  const int input_depth =
      ArraySize(gemm_input_dims,
                0);  // MatchingArraySize(*gemm_input_dims, 3, filter_dims, 0);
  const int output_depth = MatchingArraySize(filter_dims, 3, output_dims, 0);
  if (bias_data) {
    TFLITE_DCHECK_EQ(bias_shape.FlatSize(), output_shape.Dims(3));
  }

  const int output_height = ArraySize(output_dims, 2);
  const int output_width = ArraySize(output_dims, 1);
  SetConfig(kElementWidthMax32, kMaxVectorLength32);
  for (int batch = 0; batch < batches; ++batch) {
    for (int out_y = 0; out_y < output_height; ++out_y) {
      for (int out_x = 0; out_x < output_width; ++out_x) {
        const float *input_address = gemm_input_data + out_x * gemm_input_dims.strides[1] +
                                     out_y * gemm_input_dims.strides[2] +
                                     batch * gemm_input_dims.strides[3];
        float *output_address = output_data + out_x * output_dims.strides[1] +
                                out_y * output_dims.strides[2] +
                                batch * output_dims.strides[3];

        Kernel1x1MultiplyAccumulate(filter_data, input_depth,
                                      output_depth, input_address,
                                      output_address);
        // if(use_kernel1x1) {
        //   Kernel1x1MultiplyAccumulate(filter_data, input_depth,
        //                               output_depth, input_address,
        //                               output_address);
        // } else {
        // for (int out_channel = 0; out_channel < output_depth; ++out_channel) {
        //           float total = 0.f;
        //     VectorVectorMultiplyAccumulate(input_address,
        //                                    filter_data + out_channel * filter_dims.strides[3],
        //                                    output_address + out_channel,
        //                                    input_depth);
        //  }
        // }
      }
    }
  }
  int flattened_len = output_shape.FlatSize();
  if (bias_data) {
    AddBiasActivationFunctionWithMinMax(output_data, bias_data,
                                        output_activation_min, output_activation_max,
                                        flattened_len, output_depth);
  }
}

inline void Conv(const ConvParams& params, const RuntimeShape& input_shape,
                 const float* input_data, const RuntimeShape& filter_shape,
                 const float* filter_data, const RuntimeShape& bias_shape,
                 const float* bias_data, const RuntimeShape& output_shape,
                 float* output_data, const RuntimeShape& im2col_shape,
                 float* im2col_data) {
  const int stride_width = params.stride_width;
  const int stride_height = params.stride_height;
  const int dilation_width_factor = params.dilation_width_factor;
  const int dilation_height_factor = params.dilation_height_factor;
  const int pad_width = params.padding_values.width;
  const int pad_height = params.padding_values.height;
  const float output_activation_min = params.float_activation_min;
  const float output_activation_max = params.float_activation_max;
  TFLITE_DCHECK_EQ(input_shape.DimensionsCount(), 4);
  TFLITE_DCHECK_EQ(filter_shape.DimensionsCount(), 4);
  TFLITE_DCHECK_EQ(output_shape.DimensionsCount(), 4);

  const Dims<4>& input_dims = ToRuntimeDims(input_shape);
  const Dims<4>& output_dims = ToRuntimeDims(output_shape);
  const Dims<4>& filter_dims = ToRuntimeDims(filter_shape);

  (void)im2col_data;   // only used in optimized code.
  (void)im2col_shape;  // only used in optimized code.
  const int batches = MatchingDim(input_shape, 0, output_shape, 0);
  const int input_depth = MatchingDim(input_shape, 3, filter_shape, 3);
  const int output_depth = MatchingDim(filter_shape, 0, output_shape, 3);
  if (bias_data) {
    TFLITE_DCHECK_EQ(bias_shape.FlatSize(), output_depth);
  }
  const int input_height = input_shape.Dims(1);
  const int input_width = input_shape.Dims(2);
  const int filter_height = filter_shape.Dims(1);
  const int filter_width = filter_shape.Dims(2);
  const int output_height = output_shape.Dims(1);
  const int output_width = output_shape.Dims(2);
  float temp_ = 0.0;
  for (int batch = 0; batch < batches; ++batch) {
    for (int out_y = 0; out_y < output_height; ++out_y) {
      for (int out_x = 0; out_x < output_width; ++out_x) {
        for (int out_channel = 0; out_channel < output_depth; ++out_channel) {
          const int in_x_origin = (out_x * stride_width) - pad_width;
          const int in_y_origin = (out_y * stride_height) - pad_height;
          float total = 0.f;
          for (int filter_y = 0; filter_y < filter_height; ++filter_y) {
            for (int filter_x = 0; filter_x < filter_width; ++filter_x) {
              const int in_x = in_x_origin + dilation_width_factor * filter_x;
              const int in_y =
                  in_y_origin + dilation_height_factor * filter_y;
              // If the location is outside the bounds of the input image,
              // use zero as a default value.
              if ((in_x >= 0) && (in_x < input_width) && (in_y >= 0) &&
                  (in_y < input_height)) {
                const float* input_address =
                    input_data + Offset(input_dims, 0, in_x, in_y, batch);
                const float* filter_address =
                    filter_data +
                    Offset(filter_dims, 0, filter_x, filter_y, out_channel);
                VectorVectorMultiplyAccumulate(input_address, filter_address,
                                               &temp_, input_depth);
                total += temp_;
              }
            }
          }
          float bias_value = 0.0f;
          if (bias_data) {
            bias_value = bias_data[out_channel];
          }
          output_data[Offset(output_dims, out_channel, out_x, out_y, batch)]
              = ActivationFunctionWithMinMax(total + bias_value,
                                           output_activation_min,
                                           output_activation_max);
        }
      }
    }
  }
}
#endif


}  // namespace optimized_ops
}  // namespace tflite

#endif  // TENSORFLOW_CONTRIB_LITE_EXPERIMENTAL_RISCV_KERNELS_OPTIMIZED_FULLY_CONNECTED_FLOAT_H_
