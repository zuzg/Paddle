// Copyright (c) 2023 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WIdata_tHOUdata_t WARRANdata_tIES OR CONDIdata_tIONS OF ANY KIND, either
// express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <iostream>
#include <vector>

#include "paddle/extension.h"

template <typename data_t>
void add_forward_kernel(data_t* x_data, const data_t* y_data, int64_t numel) {
  for (size_t i = 0; i < numel; ++i) {
    x_data[i] += y_data[i];
  }
}

template <typename data_t>
void add_backward_kernel(data_t* y_grad_data,
                         const data_t* out_grad_data,
                         int64_t numel) {
  for (size_t i = 0; i < numel; ++i) {
    y_grad_data[i] = out_grad_data[i];
  }
}

template <typename data_t>
void relu_forward_kernel(data_t* x_data, int64_t numel) {
  for (size_t i = 0; i < numel; ++i) {
    x_data[i] = x_data[i] > 0 ? x_data[i] : 0;
  }
}

template <typename data_t>
void relu_backward_kernel(const data_t* out_data,
                          data_t* grad_out_data,
                          int64_t out_numel) {
  for (int64_t i = 0; i < out_numel; ++i) {
    grad_out_data[i] =
        grad_out_data[i] * (out_data[i] > static_cast<data_t>(0) ? 1. : 0.);
  }
}

void AddForward(paddle::Tensor& x, const paddle::Tensor& y) {  // NOLINT
  PD_CHECK(x.place() == paddle::PlaceType::kCPU, "x must be a CPU Tensor.");

  PD_DISPATCH_FLOATING_TYPES(x.type(), "AddForward", ([&] {
                               add_forward_kernel<data_t>(x.data<data_t>(),
                                                          y.data<data_t>(),
                                                          x.size());
                             }));
}

std::vector<paddle::DataType> AddInferDtype(const paddle::DataType& x_dtype,
                                            const paddle::DataType& y_dtype) {
  return {x_dtype};
}

std::vector<std::vector<int64_t>> AddInferShape(
    const std::vector<int64_t>& x_shape, const std::vector<int64_t>& y_shape) {
  return {x_shape};
}

std::vector<paddle::Tensor> AddBackward(const paddle::Tensor& x,
                                        const paddle::Tensor& y,
                                        paddle::Tensor& out_grad) {  // NOLINT
  PD_CHECK(x.place() == paddle::PlaceType::kCPU, "x must be a CPU Tensor.");
  PD_CHECK(y.place() == paddle::PlaceType::kCPU, "x must be a CPU Tensor.");

  paddle::Tensor y_grad = paddle::empty(x.shape(), x.dtype(), x.place());

  PD_DISPATCH_FLOATING_TYPES(
      out_grad.type(), "AddBackward", ([&] {
        add_backward_kernel<data_t>(
            y_grad.data<data_t>(), out_grad.data<data_t>(), out_grad.size());
      }));

  return {y_grad};
}

PD_BUILD_OP(custom_add)
    .Inputs({"X", "Y"})
    .Outputs({"Out"})
    .Inplace({{"X", "Out"}})
    .SetKernelFn(PD_KERNEL(AddForward))
    .SetInferShapeFn(PD_INFER_SHAPE(AddInferShape))
    .SetInferDtypeFn(PD_INFER_DTYPE(AddInferDtype));

PD_BUILD_GRAD_OP(custom_add)
    .Inputs({"X", "Y", paddle::Grad("Out")})
    .Outputs({paddle::Grad("X"), paddle::Grad("Y")})
    .Inplace({{paddle::Grad("Out"), paddle::Grad("X")}})
    .SetKernelFn(PD_KERNEL(AddBackward));

void ReluForwardInplace(paddle::Tensor& x) {  // NOLINT
  PD_CHECK(x.place() == paddle::PlaceType::kCPU, "x must be a CPU Tensor.");

  PD_DISPATCH_FLOATING_TYPES(x.type(), "ReluForward", ([&] {
                               relu_forward_kernel<data_t>(x.data<data_t>(),
                                                           x.size());
                             }));
}

void ReluBackwardInplace(const paddle::Tensor& x,
                         const paddle::Tensor& out,
                         paddle::Tensor& grad_out) {  // NOLINT
  PD_CHECK(out.place() == paddle::PlaceType::kCPU, "x must be a CPU Tensor.");

  PD_DISPATCH_FLOATING_TYPES(
      grad_out.type(), "ReluBackward", ([&] {
        relu_backward_kernel<data_t>(
            out.data<data_t>(), grad_out.data<data_t>(), grad_out.size());
      }));
}

PD_BUILD_OP(custom_relu_inplace)
    .Inputs({"X"})
    .Outputs({"Out"})
    .Inplace({{"X", "Out"}})
    .SetKernelFn(PD_KERNEL(ReluForwardInplace));

PD_BUILD_GRAD_OP(custom_relu_inplace)
    .Inputs({"X", "Out", paddle::Grad("Out")})
    .Outputs({paddle::Grad("X")})
    .Inplace({{paddle::Grad("Out"), paddle::Grad("X")}})
    .SetKernelFn(PD_KERNEL(ReluBackwardInplace));
