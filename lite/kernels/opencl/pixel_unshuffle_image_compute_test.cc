// Copyright (c) 2019 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include <gtest/gtest.h>
#include <random>
#include "lite/backends/opencl/target_wrapper.h"
#include "lite/core/op_registry.h"
#include "lite/core/tensor.h"
#include "lite/kernels/opencl/test_helper.h"

#define FP16_MAX_DIFF (5e-1)

namespace paddle {
namespace lite {

TEST(pixel_unshuffle_image2d, compute) {
  LOG(INFO) << "create kernel ...";
  auto kernels = KernelRegistry::Global().Create("pixel_unshuffle",
                                                 TARGET(kOpenCL),
                                                 PRECISION(kFP16),
                                                 DATALAYOUT(kImageDefault));
  ASSERT_FALSE(kernels.empty());

  const int INPUT_N = 1;
  const int INPUT_C = 1;
  const int INPUT_H = 4;
  const int INPUT_W = 4;
  const int DOWNSCALE_FACTOR = 2;

  auto kernel = std::move(kernels.front());

  LOG(INFO) << "prepare to test kernel ====> " << kernel->doc();

  lite::Tensor x, out;
  operators::PixelUnShuffleParam param;
  param.x = &x;
  param.output = &out;
  param.downscale_factor = DOWNSCALE_FACTOR;

  std::unique_ptr<KernelContext> context(new KernelContext);
  context->As<OpenCLContext>().InitOnce();

  kernel->SetParam(param);
  std::unique_ptr<KernelContext> pixel_unshuffle_context(new KernelContext);
  context->As<OpenCLContext>().CopySharedTo(
      &(pixel_unshuffle_context->As<OpenCLContext>()));

  kernel->SetContext(std::move(pixel_unshuffle_context));

  const DDim in_dim =
      DDim(std::vector<DDim::value_type>{INPUT_N, INPUT_C, INPUT_H, INPUT_W});
  const DDim out_dim = DDim(std::vector<DDim::value_type>{
      INPUT_N,
      INPUT_C * DOWNSCALE_FACTOR * DOWNSCALE_FACTOR,
      INPUT_H / DOWNSCALE_FACTOR,
      INPUT_W / DOWNSCALE_FACTOR});
  LOG(INFO) << "in_dim: " << in_dim;
  LOG(INFO) << "DOWNSCALE_FACTOR: " << DOWNSCALE_FACTOR;
  LOG(INFO) << "out_dim: " << out_dim;

  x.Resize(in_dim);
  out.Resize(out_dim);

  std::default_random_engine engine;
  std::uniform_real_distribution<float> dist(-2, 2);
  std::vector<float> input_v(INPUT_N * INPUT_C * INPUT_H * INPUT_W);

  int index = 0;
  for (auto& i : input_v) {
    i = index++;
  }
  VLOG(1) << "input_v ..... ";
  for (size_t i = 0; i < input_v.size(); i++) {
    VLOG(10) << input_v[i];
  }

  LOG(INFO) << "prepare input";
  CLImageConverterDefault* default_converter = new CLImageConverterDefault();
  DDim x_image_shape = default_converter->InitImageDimInfoWith(in_dim);
  LOG(INFO) << "x_image_shape = " << x_image_shape[0] << " "
            << x_image_shape[1];
  std::vector<half_t> x_image_data(x_image_shape.production() * 4);  // 4 : RGBA
  default_converter->NCHWToImage(input_v.data(), x_image_data.data(), in_dim);
  auto* x_image = x.mutable_data<half_t, cl::Image2D>(
      x_image_shape[0], x_image_shape[1], x_image_data.data());
  VLOG(1) << "x_image_data ..... ";
  for (size_t i = 0; i < x_image_data.size(); i++) {
    VLOG(10) << Half2Float(x_image_data[i]);
  }
  DDim out_image_shape = default_converter->InitImageDimInfoWith(out_dim);
  LOG(INFO) << "out_image_shape = " << out_image_shape[0] << " "
            << out_image_shape[1];
  auto* out_image = out.mutable_data<half_t, cl::Image2D>(out_image_shape[0],
                                                          out_image_shape[1]);
  kernel->Launch();
  CLRuntime::Global()->command_queue().finish();
  std::vector<float> out_data_v{
      0, 2, 8, 10, 1, 3, 9, 11, 4, 6, 12, 14, 5, 7, 13, 15};

  const size_t cl_image2d_row_pitch{0};
  const size_t cl_image2d_slice_pitch{0};
  half_t* out_image_data = new half_t[out_image_shape.production() * 4];
  TargetWrapperCL::ImgcpySync(out_image_data,
                              out_image,
                              out_image_shape[0],
                              out_image_shape[1],
                              cl_image2d_row_pitch,
                              cl_image2d_slice_pitch,
                              IoDirection::DtoH);
  VLOG(1) << "out_image_data ..... ";
  for (size_t i = 0; i < out_image_shape.production() * 4; i++) {
    VLOG(10) << Half2Float(out_image_data[i]);
  }
  float* out_data = new float[out_image_shape.production() * 4];
  default_converter->ImageToNCHW(
      out_image_data, out_data, out_image_shape, out_dim);

  VLOG(1) << "out_data ..... ";
  for (int i = 0; i < out_dim.production(); i++) {
    VLOG(10) << out_data[i];
  }

  for (int i = 0; i < out_dim.production(); i++) {
    auto abs_diff = abs(out_data[i] - out_data_v[i]);
    auto relative_diff = COMPUTE_RELATIVE_DIFF(out_data[i], out_data_v[i]);
    EXPECT_EQ((relative_diff <= FP16_MAX_DIFF) || (abs_diff <= FP16_MAX_DIFF),
              true);
    if ((relative_diff > FP16_MAX_DIFF) && (abs_diff > FP16_MAX_DIFF)) {
      LOG(ERROR) << "error idx:" << i << " out_data[" << i
                 << "]:" << out_data[i] << " "
                                           "out_ref["
                 << i << "]:" << out_data_v[i] << " abs_diff:" << abs_diff
                 << " relative_diff:" << relative_diff
                 << " FP16_MAX_DIFF:" << FP16_MAX_DIFF;
    }
  }
}

}  // namespace lite
}  // namespace paddle

USE_LITE_KERNEL(pixel_unshuffle, kOpenCL, kFP16, kImageDefault, image2d);
