// Copyright (c) 2024 PaddlePaddle Authors. All Rights Reserved.
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

#pragma once

#include <cmath>
#include "paddle/phi/common/int_array.h"
#include "paddle/phi/core/dense_tensor.h"
#include "paddle/phi/kernels/cast_kernel.h"
#include "paddle/phi/kernels/elementwise_multiply_kernel.h"
#include "paddle/phi/kernels/fake_quantize_kernel.h"
#include "paddle/phi/kernels/full_kernel.h"
#include "paddle/phi/kernels/funcs/eigen/extensions.h"
#include "paddle/phi/kernels/funcs/fake_quantize_functor.h"
#include "paddle/phi/kernels/funcs/for_range.h"
#include "paddle/phi/kernels/reduce_sum_kernel.h"

namespace phi {
template <typename T>
struct QuantizeDataType {
  using type = T;
};

template <>
struct QuantizeDataType<phi::dtype::float16> {
  using type = float;
};

// for lsqplus
template <typename T, typename ComputeT>
struct GetIntermediateParams {
 public:
  GetIntermediateParams(const T* x,
                        const T* alpha,
                        const T* beta,
                        const T* g,
                        const T* out_grad,
                        const int Qn,
                        const int Qp,
                        const int round_type,
                        ComputeT* alpha_matrix,
                        ComputeT* beta_matrix,
                        T* mask)
      : x_(x),
        alpha_(alpha),
        beta_(beta),
        g_(g),
        out_grad_(out_grad),
        Qn_(Qn),
        Qp_(Qp),
        round_type_(round_type),
        alpha_matrix_(alpha_matrix),
        beta_matrix_(beta_matrix),
        mask_(mask) {}

  HOSTDEVICE void operator()(size_t i) {
    // using ComputeDataType = typename QuantizeDataType<T>::type;
    ComputeT trans_out;
    ComputeT round_out;

    ComputeT x = static_cast<ComputeT>(x_[i]);
    ComputeT alpha = static_cast<ComputeT>(alpha_[0]);
    ComputeT beta = static_cast<ComputeT>(beta_[0]);
    ComputeT g = static_cast<ComputeT>(g_[0]);
    ComputeT out_grad = static_cast<ComputeT>(out_grad_[i]);
    ComputeT Qn = static_cast<ComputeT>(Qn_);
    ComputeT Qp = static_cast<ComputeT>(Qp_);
    ComputeT alpha_mx;
    ComputeT beta_mx;
    ComputeT inv_alpha = phi::funcs::inverse(alpha);
    trans_out = (x - beta) * inv_alpha;

    if (round_type_ == 0) {
      round_out = phi::funcs::roundWithTiesToEven(trans_out);
    } else {
      round_out = std::round(trans_out);
    }
    ComputeT mask_n = 0;
    ComputeT mask_m = 0;
    ComputeT mask_p = 0;

    if (trans_out < Qn) {
      mask_n = 1;
      mask_m = 0;
      mask_p = 0;
    } else if (trans_out > Qp) {
      mask_n = 0;
      mask_m = 0;
      mask_p = 1;
    } else {
      mask_n = 0;
      mask_m = 1;
      mask_p = 0;
    }
    alpha_mx =
        (mask_n * Qn + mask_m * round_out + mask_p * Qp - mask_m * trans_out) *
        g * out_grad;
    alpha_matrix_[i] = alpha_mx;

    beta_mx = (mask_n + mask_p) * g * out_grad;
    beta_matrix_[i] = beta_mx;

    mask_[i] = static_cast<T>(mask_m);
  }

 private:
  const T* x_;
  const T* alpha_;
  const T* beta_;
  const T* g_;
  const T* out_grad_;
  const int Qn_;
  const int Qp_;
  const int round_type_;
  ComputeT* alpha_matrix_;
  ComputeT* beta_matrix_;
  T* mask_;
};

// for lsqplus
template <typename T, typename Context>
void FakeQuantizeDequantizeLsqplusGradKernel(const Context& dev_ctx,
                                             const DenseTensor& x,
                                             const DenseTensor& alpha,
                                             const DenseTensor& beta,
                                             const DenseTensor& g,
                                             const DenseTensor& out_grad,
                                             int bit_length,
                                             bool is_sign,
                                             int round_type,
                                             DenseTensor* x_grad,
                                             DenseTensor* alpha_grad,
                                             DenseTensor* beta_grad) {
  // space for x_grad, alpha_grad and beta_grad
  dev_ctx.template Alloc<T>(x_grad);
  dev_ctx.template Alloc<T>(alpha_grad);
  dev_ctx.template Alloc<T>(beta_grad);
  using ComputeT = typename QuantizeDataType<T>::type;
  // intermediate params initialization
  DenseTensor alpha_matrix = FullLike<ComputeT, Context>(dev_ctx, x, 0);
  DenseTensor beta_matrix = FullLike<ComputeT, Context>(dev_ctx, x, 0);
  DenseTensor mask = FullLike<T, Context>(dev_ctx, x, 0);

  // gradient scaling
  // DenseTensor gradient_scale = FullLike<T, Context>(dev_ctx, x, 0);
  // MultiplyKernel<T, Context>(dev_ctx, out_grad, g, &gradient_scale);

  // get intermedate params
  int Qn = 0;
  int Qp = 255;
  if (is_sign) {
    Qn = -std::pow(2, bit_length - 1);
    Qp = std::pow(2, bit_length - 1) - 1;
  } else {
    Qn = 0;
    Qp = std::pow(2, bit_length) - 1;
  }
  size_t numel = x.numel();
  phi::funcs::ForRange<Context> for_range(dev_ctx, numel);

  // note: cannot use alpha.data<T>()[0] and beta.data<T>()[0] in GPU version
  // otherwise, the kernel will be killed
  phi::GetIntermediateParams<T, ComputeT> get_intermediate_params(
      x.data<T>(),
      alpha.data<T>(),
      beta.data<T>(),
      g.data<T>(),
      out_grad.data<T>(),
      Qn,
      Qp,
      round_type,
      alpha_matrix.data<ComputeT>(),
      beta_matrix.data<ComputeT>(),
      mask.data<T>());

  for_range(get_intermediate_params);

  std::vector<int64_t> v_dims(x.dims().size());
  std::iota(v_dims.begin(), v_dims.end(), 0);
  IntArray v_axes(v_dims);
  // get alpha_grad
  DenseTensor compute_alpha_grad = Sum<ComputeT, Context>(
      dev_ctx, alpha_matrix, v_axes, alpha_matrix.dtype(), false);
  CastKernel<ComputeT, Context>(
      dev_ctx, compute_alpha_grad, alpha.dtype(), alpha_grad);
  alpha_grad->Resize(alpha.dims());

  // get beta_grad
  DenseTensor compute_beta_grad = Sum<ComputeT, Context>(
      dev_ctx, beta_matrix, v_axes, beta_matrix.dtype(), false);
  CastKernel<ComputeT, Context>(
      dev_ctx, compute_beta_grad, beta.dtype(), beta_grad);
  beta_grad->Resize(beta.dims());

  // get x_grad
  MultiplyKernel<T, Context>(dev_ctx, mask, out_grad, x_grad);
}

}  // namespace phi
