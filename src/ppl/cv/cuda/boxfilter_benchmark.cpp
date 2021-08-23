/**
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements. See the NOTICE file distributed with this
 * work for additional information regarding copyright ownership. The ASF
 * licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License. You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#include "boxfilter.h"

#include <time.h>
#include <sys/time.h>

#include "opencv2/opencv.hpp"
#include "benchmark/benchmark.h"

#include "ppl/cv/debug.h"
#include "infrastructure.hpp"

using namespace ppl::cv;
using namespace ppl::cv::cuda;
using namespace ppl::cv::debug;

template <typename T, int channels, int ksize_x, int ksize_y,
          BorderType border_type>
void BM_BoxFilter_ppl_cuda(benchmark::State &state) {
  int width  = state.range(0);
  int height = state.range(1);
  cv::Mat src;
  src = createSourceImage(height, width,
                          CV_MAKETYPE(cv::DataType<T>::depth, channels));
  cv::Mat dst(height, width,
              CV_MAKETYPE(cv::DataType<T>::depth, channels));
  cv::cuda::GpuMat gpu_src(src);
  cv::cuda::GpuMat gpu_dst(dst);

  bool normalize = true;

  int iterations = 1000;
  struct timeval start, end;

  // warm up the GPU
  for (int i = 0; i < iterations; i++) {
    BoxFilter<T, channels>(0, gpu_src.rows, gpu_src.cols,
        gpu_src.step / sizeof(T), (T*)gpu_src.data, ksize_x, ksize_y, normalize,
        gpu_dst.step / sizeof(T), (T*)gpu_dst.data, border_type);
  }
  cudaDeviceSynchronize();

  for (auto _ : state) {
    gettimeofday(&start, NULL);
    for (int i = 0; i < iterations; i++) {
      BoxFilter<T, channels>(0, gpu_src.rows, gpu_src.cols,
          gpu_src.step / sizeof(T), (T*)gpu_src.data, ksize_x, ksize_y,
          normalize, gpu_dst.step / sizeof(T), (T*)gpu_dst.data, border_type);
    }
    cudaDeviceSynchronize();
    gettimeofday(&end, NULL);
    int time = ((end.tv_sec * 1000000 + end.tv_usec) -
                (start.tv_sec * 1000000 + start.tv_usec)) / iterations;
    state.SetIterationTime(time * 1e-6);
  }
  state.SetItemsProcessed(state.iterations() * 1);
}

template <typename T, int channels, int ksize_x, int ksize_y,
          BorderType border_type>
void BM_BoxFilter_opencv_cuda(benchmark::State &state) {
  int width  = state.range(0);
  int height = state.range(1);
  cv::Mat src;
  src = createSourceImage(height, width,
                          CV_MAKETYPE(cv::DataType<T>::depth, channels));
  cv::Mat dst(height, width,
              CV_MAKETYPE(cv::DataType<T>::depth, channels));
  cv::cuda::GpuMat gpu_src(src);
  cv::cuda::GpuMat gpu_dst(dst);

  int iterations = 1000;
  struct timeval start, end;

  cv::BorderTypes border = cv::BORDER_DEFAULT;
  if (border_type == BORDER_TYPE_REPLICATE) {
    border = cv::BORDER_REPLICATE;
  }
  else if (border_type == BORDER_TYPE_REFLECT) {
    border = cv::BORDER_REFLECT;
  }
  else if (border_type == BORDER_TYPE_REFLECT_101) {
    border = cv::BORDER_REFLECT_101;
  }
  else {
  }

  // warm up the GPU
  for (int i = 0; i < iterations; i++) {
    cv::Ptr<cv::cuda::Filter> filter =
      cv::cuda::createBoxFilter(gpu_src.type(), gpu_dst.type(),
                                cv::Size(ksize_x, ksize_x), cv::Point(-1, -1),
                                border);
    filter->apply(gpu_src, gpu_dst);
  }
  cudaDeviceSynchronize();

  for (auto _ : state) {
    gettimeofday(&start, NULL);
    for (int i = 0; i < iterations; i++) {
      cv::Ptr<cv::cuda::Filter> filter =
        cv::cuda::createBoxFilter(gpu_src.type(), gpu_dst.type(),
                                  cv::Size(ksize_x, ksize_x), cv::Point(-1, -1),
                                  border);
      filter->apply(gpu_src, gpu_dst);
    }
    cudaDeviceSynchronize();
    gettimeofday(&end, NULL);
    int time = ((end.tv_sec * 1000000 + end.tv_usec) -
                (start.tv_sec * 1000000 + start.tv_usec)) / iterations;
    state.SetIterationTime(time * 1e-6);
  }
  state.SetItemsProcessed(state.iterations() * 1);
}

template <typename T, int channels, int ksize_x, int ksize_y,
          BorderType border_type>
void BM_BoxFilter_opencv_x86_cuda(benchmark::State &state) {
  int width  = state.range(0);
  int height = state.range(1);
  cv::Mat src;
  src = createSourceImage(height, width,
                          CV_MAKETYPE(cv::DataType<T>::depth, channels));
  cv::Mat dst(height, width,
              CV_MAKETYPE(cv::DataType<T>::depth, channels));

  cv::BorderTypes border = cv::BORDER_DEFAULT;
  if (border_type == BORDER_TYPE_REPLICATE) {
    border = cv::BORDER_REPLICATE;
  }
  else if (border_type == BORDER_TYPE_REFLECT) {
    border = cv::BORDER_REFLECT;
  }
  else if (border_type == BORDER_TYPE_REFLECT_101) {
    border = cv::BORDER_REFLECT_101;
  }
  else {
  }

  bool normalize = true;

  for (auto _ : state) {
    cv::boxFilter(src, dst, dst.depth(), cv::Size(ksize_x, ksize_y),
                  cv::Point(-1, -1), normalize, border);
  }
  state.SetItemsProcessed(state.iterations() * 1);
}

#define RUN_BENCHMARK0(type, ksize_x, ksize_y, border_type, width, height)     \
BENCHMARK_TEMPLATE(BM_BoxFilter_opencv_x86_cuda, type, c1, ksize_x, ksize_y,   \
                   border_type)->Args({width, height});                        \
BENCHMARK_TEMPLATE(BM_BoxFilter_ppl_cuda, type, c1, ksize_x, ksize_y,          \
                   border_type)->Args({width, height})->UseManualTime()->      \
                   Iterations(10);                                             \
BENCHMARK_TEMPLATE(BM_BoxFilter_opencv_x86_cuda, type, c3, ksize_x, ksize_y,   \
                   border_type)->Args({width, height});                        \
BENCHMARK_TEMPLATE(BM_BoxFilter_ppl_cuda, type, c3, ksize_x, ksize_y,          \
                   border_type)->Args({width, height})->UseManualTime()->      \
                   Iterations(10);                                             \
BENCHMARK_TEMPLATE(BM_BoxFilter_opencv_x86_cuda, type, c4, ksize_x, ksize_y,   \
                   border_type)->Args({width, height});                        \
BENCHMARK_TEMPLATE(BM_BoxFilter_ppl_cuda, type, c4, ksize_x, ksize_y,          \
                   border_type)->Args({width, height})->UseManualTime()->      \
                   Iterations(10);

// RUN_BENCHMARK0(uchar, 5, 5, BORDER_TYPE_REPLICATE, 640, 480)
// RUN_BENCHMARK0(uchar, 5, 5, BORDER_TYPE_REFLECT, 640, 480)
// RUN_BENCHMARK0(uchar, 5, 5, BORDER_TYPE_REFLECT_101, 640, 480)
// RUN_BENCHMARK0(uchar, 17, 17, BORDER_TYPE_REPLICATE, 640, 480)
// RUN_BENCHMARK0(uchar, 17, 17, BORDER_TYPE_REFLECT, 640, 480)
// RUN_BENCHMARK0(uchar, 17, 17, BORDER_TYPE_REFLECT_101, 640, 480)
// RUN_BENCHMARK0(uchar, 25, 25, BORDER_TYPE_REPLICATE, 640, 480)
// RUN_BENCHMARK0(uchar, 25, 25, BORDER_TYPE_REFLECT, 640, 480)
// RUN_BENCHMARK0(uchar, 25, 25, BORDER_TYPE_REFLECT_101, 640, 480)
// RUN_BENCHMARK0(uchar, 31, 31, BORDER_TYPE_REPLICATE, 640, 480)
// RUN_BENCHMARK0(uchar, 31, 31, BORDER_TYPE_REFLECT, 640, 480)
// RUN_BENCHMARK0(uchar, 31, 31, BORDER_TYPE_REFLECT_101, 640, 480)


// RUN_BENCHMARK0(float, 5, 5, BORDER_TYPE_REPLICATE, 640, 480)
// RUN_BENCHMARK0(float, 5, 5, BORDER_TYPE_REFLECT, 640, 480)
// RUN_BENCHMARK0(float, 5, 5, BORDER_TYPE_REFLECT_101, 640, 480)
// RUN_BENCHMARK0(float, 17, 17, BORDER_TYPE_REPLICATE, 640, 480)
// RUN_BENCHMARK0(float, 17, 17, BORDER_TYPE_REFLECT, 640, 480)
// RUN_BENCHMARK0(float, 17, 17, BORDER_TYPE_REFLECT_101, 640, 480)
// RUN_BENCHMARK0(float, 25, 25, BORDER_TYPE_REPLICATE, 640, 480)
// RUN_BENCHMARK0(float, 25, 25, BORDER_TYPE_REFLECT, 640, 480)
// RUN_BENCHMARK0(float, 25, 25, BORDER_TYPE_REFLECT_101, 640, 480)
// RUN_BENCHMARK0(float, 31, 31, BORDER_TYPE_REPLICATE, 640, 480)
// RUN_BENCHMARK0(float, 31, 31, BORDER_TYPE_REFLECT, 640, 480)
// RUN_BENCHMARK0(float, 31, 31, BORDER_TYPE_REFLECT_101, 640, 480)

#define RUN_BENCHMARK1(ksize_x, ksize_y, border_type, width, height)           \
BENCHMARK_TEMPLATE(BM_BoxFilter_opencv_cuda, uchar, c1, ksize_x, ksize_y,      \
                   border_type)->Args({width, height})->                       \
                   UseManualTime()->Iterations(10);                            \
BENCHMARK_TEMPLATE(BM_BoxFilter_ppl_cuda, uchar, c1, ksize_x, ksize_y,         \
                   border_type)->Args({width, height})->UseManualTime()->      \
                   Iterations(10);                                             \
BENCHMARK_TEMPLATE(BM_BoxFilter_opencv_cuda, float, c1, ksize_x, ksize_y,      \
                   border_type)->Args({width, height})->                       \
                   UseManualTime()->Iterations(10);                            \
BENCHMARK_TEMPLATE(BM_BoxFilter_ppl_cuda, float, c1, ksize_x, ksize_y,         \
                   border_type)->Args({width, height})->UseManualTime()->      \
                   Iterations(10);                                             \
BENCHMARK_TEMPLATE(BM_BoxFilter_opencv_cuda, uchar, c4, ksize_x, ksize_y,      \
                   border_type)->Args({width, height})->                       \
                   UseManualTime()->Iterations(10);                            \
BENCHMARK_TEMPLATE(BM_BoxFilter_ppl_cuda, uchar, c4, ksize_x, ksize_y,         \
                   border_type)->Args({width, height})->UseManualTime()->      \
                   Iterations(10);

// RUN_BENCHMARK1(5, 5, BORDER_TYPE_REPLICATE, 640, 480)
// RUN_BENCHMARK1(5, 5, BORDER_TYPE_REFLECT, 640, 480)
// RUN_BENCHMARK1(5, 5, BORDER_TYPE_REFLECT_101, 640, 480)
// RUN_BENCHMARK1(17, 17, BORDER_TYPE_REPLICATE, 640, 480)
// RUN_BENCHMARK1(17, 17, BORDER_TYPE_REFLECT, 640, 480)
// RUN_BENCHMARK1(17, 17, BORDER_TYPE_REFLECT_101, 640, 480)
// RUN_BENCHMARK1(25, 25, BORDER_TYPE_REPLICATE, 640, 480)
// RUN_BENCHMARK1(25, 25, BORDER_TYPE_REFLECT, 640, 480)
// RUN_BENCHMARK1(25, 25, BORDER_TYPE_REFLECT_101, 640, 480)
// RUN_BENCHMARK1(31, 31, BORDER_TYPE_REPLICATE, 640, 480)
// RUN_BENCHMARK1(31, 31, BORDER_TYPE_REFLECT, 640, 480)
// RUN_BENCHMARK1(31, 31, BORDER_TYPE_REFLECT_101, 640, 480)
// RUN_BENCHMARK1(43, 43, BORDER_TYPE_REPLICATE, 640, 480)
// RUN_BENCHMARK1(43, 43, BORDER_TYPE_REFLECT, 640, 480)
// RUN_BENCHMARK1(43, 43, BORDER_TYPE_REFLECT_101, 640, 480)

#define RUN_OPENCV_TYPE_FUNCTIONS(type, ksize_x, ksize_y, border_type)         \
BENCHMARK_TEMPLATE(BM_BoxFilter_opencv_x86_cuda, type, c1, ksize_x, ksize_y,   \
                   border_type)->Args({640, 480});                             \
BENCHMARK_TEMPLATE(BM_BoxFilter_opencv_x86_cuda, type, c3, ksize_x, ksize_y,   \
                   border_type)->Args({640, 480});                             \
BENCHMARK_TEMPLATE(BM_BoxFilter_opencv_x86_cuda, type, c4, ksize_x, ksize_y,   \
                   border_type)->Args({640, 480});

#define RUN_PPL_CV_TYPE_FUNCTIONS(type, ksize_x, ksize_y, border_type)         \
BENCHMARK_TEMPLATE(BM_BoxFilter_ppl_cuda, type, c1, ksize_x, ksize_y,          \
                   border_type)->Args({640, 480})->                            \
                   UseManualTime()->Iterations(10);                            \
BENCHMARK_TEMPLATE(BM_BoxFilter_ppl_cuda, type, c3, ksize_x, ksize_y,          \
                   border_type)->Args({640, 480})->                            \
                   UseManualTime()->Iterations(10);                            \
BENCHMARK_TEMPLATE(BM_BoxFilter_ppl_cuda, type, c4, ksize_x, ksize_y,          \
                   border_type)->Args({640, 480})->                            \
                   UseManualTime()->Iterations(10);

RUN_OPENCV_TYPE_FUNCTIONS(uchar, 5, 5, BORDER_TYPE_REPLICATE)
RUN_OPENCV_TYPE_FUNCTIONS(uchar, 5, 5, BORDER_TYPE_REFLECT)
RUN_OPENCV_TYPE_FUNCTIONS(uchar, 5, 5, BORDER_TYPE_REFLECT_101)
RUN_OPENCV_TYPE_FUNCTIONS(float, 5, 5, BORDER_TYPE_REPLICATE)
RUN_OPENCV_TYPE_FUNCTIONS(float, 5, 5, BORDER_TYPE_REFLECT)
RUN_OPENCV_TYPE_FUNCTIONS(float, 5, 5, BORDER_TYPE_REFLECT_101)

RUN_OPENCV_TYPE_FUNCTIONS(uchar, 17, 17, BORDER_TYPE_REPLICATE)
RUN_OPENCV_TYPE_FUNCTIONS(uchar, 17, 17, BORDER_TYPE_REFLECT)
RUN_OPENCV_TYPE_FUNCTIONS(uchar, 17, 17, BORDER_TYPE_REFLECT_101)
RUN_OPENCV_TYPE_FUNCTIONS(float, 17, 17, BORDER_TYPE_REPLICATE)
RUN_OPENCV_TYPE_FUNCTIONS(float, 17, 17, BORDER_TYPE_REFLECT)
RUN_OPENCV_TYPE_FUNCTIONS(float, 17, 17, BORDER_TYPE_REFLECT_101)

RUN_OPENCV_TYPE_FUNCTIONS(uchar, 25, 25, BORDER_TYPE_REPLICATE)
RUN_OPENCV_TYPE_FUNCTIONS(uchar, 25, 25, BORDER_TYPE_REFLECT)
RUN_OPENCV_TYPE_FUNCTIONS(uchar, 25, 25, BORDER_TYPE_REFLECT_101)
RUN_OPENCV_TYPE_FUNCTIONS(float, 25, 25, BORDER_TYPE_REPLICATE)
RUN_OPENCV_TYPE_FUNCTIONS(float, 25, 25, BORDER_TYPE_REFLECT)
RUN_OPENCV_TYPE_FUNCTIONS(float, 25, 25, BORDER_TYPE_REFLECT_101)

RUN_OPENCV_TYPE_FUNCTIONS(uchar, 31, 31, BORDER_TYPE_REPLICATE)
RUN_OPENCV_TYPE_FUNCTIONS(uchar, 31, 31, BORDER_TYPE_REFLECT)
RUN_OPENCV_TYPE_FUNCTIONS(uchar, 31, 31, BORDER_TYPE_REFLECT_101)
RUN_OPENCV_TYPE_FUNCTIONS(float, 31, 31, BORDER_TYPE_REPLICATE)
RUN_OPENCV_TYPE_FUNCTIONS(float, 31, 31, BORDER_TYPE_REFLECT)
RUN_OPENCV_TYPE_FUNCTIONS(float, 31, 31, BORDER_TYPE_REFLECT_101)

RUN_PPL_CV_TYPE_FUNCTIONS(uchar, 5, 5, BORDER_TYPE_REPLICATE)
RUN_PPL_CV_TYPE_FUNCTIONS(uchar, 5, 5, BORDER_TYPE_REFLECT)
RUN_PPL_CV_TYPE_FUNCTIONS(uchar, 5, 5, BORDER_TYPE_REFLECT_101)
RUN_PPL_CV_TYPE_FUNCTIONS(float, 5, 5, BORDER_TYPE_REPLICATE)
RUN_PPL_CV_TYPE_FUNCTIONS(float, 5, 5, BORDER_TYPE_REFLECT)
RUN_PPL_CV_TYPE_FUNCTIONS(float, 5, 5, BORDER_TYPE_REFLECT_101)

RUN_PPL_CV_TYPE_FUNCTIONS(uchar, 17, 17, BORDER_TYPE_REPLICATE)
RUN_PPL_CV_TYPE_FUNCTIONS(uchar, 17, 17, BORDER_TYPE_REFLECT)
RUN_PPL_CV_TYPE_FUNCTIONS(uchar, 17, 17, BORDER_TYPE_REFLECT_101)
RUN_PPL_CV_TYPE_FUNCTIONS(float, 17, 17, BORDER_TYPE_REPLICATE)
RUN_PPL_CV_TYPE_FUNCTIONS(float, 17, 17, BORDER_TYPE_REFLECT)
RUN_PPL_CV_TYPE_FUNCTIONS(float, 17, 17, BORDER_TYPE_REFLECT_101)

RUN_PPL_CV_TYPE_FUNCTIONS(uchar, 25, 25, BORDER_TYPE_REPLICATE)
RUN_PPL_CV_TYPE_FUNCTIONS(uchar, 25, 25, BORDER_TYPE_REFLECT)
RUN_PPL_CV_TYPE_FUNCTIONS(uchar, 25, 25, BORDER_TYPE_REFLECT_101)
RUN_PPL_CV_TYPE_FUNCTIONS(float, 25, 25, BORDER_TYPE_REPLICATE)
RUN_PPL_CV_TYPE_FUNCTIONS(float, 25, 25, BORDER_TYPE_REFLECT)
RUN_PPL_CV_TYPE_FUNCTIONS(float, 25, 25, BORDER_TYPE_REFLECT_101)

RUN_PPL_CV_TYPE_FUNCTIONS(uchar, 31, 31, BORDER_TYPE_REPLICATE)
RUN_PPL_CV_TYPE_FUNCTIONS(uchar, 31, 31, BORDER_TYPE_REFLECT)
RUN_PPL_CV_TYPE_FUNCTIONS(uchar, 31, 31, BORDER_TYPE_REFLECT_101)
RUN_PPL_CV_TYPE_FUNCTIONS(float, 31, 31, BORDER_TYPE_REPLICATE)
RUN_PPL_CV_TYPE_FUNCTIONS(float, 31, 31, BORDER_TYPE_REFLECT)
RUN_PPL_CV_TYPE_FUNCTIONS(float, 31, 31, BORDER_TYPE_REFLECT_101)
