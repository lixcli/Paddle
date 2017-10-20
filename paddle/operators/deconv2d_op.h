/* Copyright (c) 2016 PaddlePaddle Authors. All Rights Reserve.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#pragma once

#include "glog/logging.h"
#include "paddle/framework/eigen.h"
#include "paddle/framework/op_registry.h"
#include "paddle/operators/math/im2col.h"
#include "paddle/operators/math/math_function.h"

namespace paddle {
namespace operators {

using Tensor = framework::Tensor;
using DDim = framework::DDim;

// Define Op classes in .h file so that other deconv
// operator implementations can reuse the code.
class Deconv2DOpMaker : public framework::OpProtoAndCheckerMaker {
 public:
  Deconv2DOpMaker(framework::OpProto* proto,
                  framework::OpAttrChecker* op_checker);
};

class Deconv2DOp : public framework::OperatorWithKernel {
 public:
  using framework::OperatorWithKernel::OperatorWithKernel;

 protected:
  void InferShape(framework::InferShapeContext* ctx) const override;
};

class Deconv2DOpGrad : public framework::OperatorWithKernel {
 public:
  using framework::OperatorWithKernel::OperatorWithKernel;

 protected:
  void InferShape(framework::InferShapeContext* ctx) const override;
};

template <typename Place, typename T>
class GemmDeconv2DKernel : public framework::OpKernel<T> {
 public:
  void Compute(const framework::ExecutionContext& context) const override {
    const Tensor* input = context.Input<Tensor>("Input");
    // The filter will be reshaped, so it should not be constant pointer
    Tensor filter = *context.Input<Tensor>("Filter");

    Tensor* output = context.Output<Tensor>("Output");

    std::vector<int> strides = context.Attr<std::vector<int>>("strides");

    // no paddings and groups allowed in deconv

    int N = input->dims()[0];
    int M = input->dims()[1];
    int H = input->dims()[2];
    int W = input->dims()[3];

    int K_H = filter.dims()[2];
    int K_W = filter.dims()[3];

    int C = output->dims()[1];  // output channels
    int O_H = output->dims()[2];
    int O_W = output->dims()[3];

    paddle::operators::math::Col2ImFunctor<
        paddle::operators::math::ColFormat::kCFO, Place, T>
        col2im;

    // use col_shape in the im2col and col2im calculation
    DDim col_shape = {C, K_H, K_W, H, W};

    // use col_matrix_shape in the gemm calculation
    DDim col_matrix_shape = {C * K_H * K_W, H * W};

    Tensor col;
    col.mutable_data<T>(col_shape, context.GetPlace());
    // col_matrix shares the same piece of data with col,
    // but will be reshaped into a two-dimensional matrix shape
    // to call the matrix multiplication interface.
    Tensor col_matrix = col;
    col_matrix.Resize(col_matrix_shape);

    DDim output_shape = {C, O_H, O_W};
    DDim input_matrix_shape = {M, H * W};

    DDim filter_matrix_shape = {M, C * K_H * K_W};
    filter.Resize(filter_matrix_shape);

    // deconvolution: gemm + col2im (similar to conv-backward on input)

    output->mutable_data<T>(context.GetPlace());
    auto t = framework::EigenVector<T>::Flatten(*output);
    t.device(context.GetEigenDevice<Place>()) = t.constant(static_cast<T>(0));

    for (int i = 0; i < N; i++) {
      // batch with size (M, H * W)
      Tensor input_batch = input->Slice<T>(i, i + 1).Resize(input_matrix_shape);
      // filter size: (M, C * K_H * K_W)

      // output size: (C, O_H, O_W)
      Tensor output_batch = output->Slice<T>(i, i + 1).Resize(output_shape);

      // col_matrix = filter * input_batch
      // of shape (C * K_H * K_W, H * W)
      math::matmul<Place, T>(context.device_context(), filter, true,
                             input_batch, false, T(1.0), &col_matrix, T(0.0));
      col2im(context.device_context(), output_batch, col, strides[0],
             strides[1], 0, 0);
    }
  }
};

template <typename Place, typename T>
class GemmDeconvGrad2DKernel : public framework::OpKernel<T> {
 public:
  void Compute(const framework::ExecutionContext& context) const override {
    const Tensor* input = context.Input<Tensor>("Input");
    const Tensor* output_grad =
        context.Input<Tensor>(framework::GradVarName("Output"));

    // For filter, we do not use const pointer b/c we will do reshape,
    // but we should avoid modifying its value.
    Tensor filter = *context.Input<Tensor>("Filter");

    Tensor* input_grad =
        context.Output<Tensor>(framework::GradVarName("Input"));
    Tensor* filter_grad =
        context.Output<Tensor>(framework::GradVarName("Filter"));

    std::vector<int> strides = context.Attr<std::vector<int>>("strides");
    // Actually, no paddings and groups allowed in deconv.
    std::vector<int> paddings = context.Attr<std::vector<int>>("paddings");

    int N = input->dims()[0];
    int M = input->dims()[1];
    int H = input->dims()[2];
    int W = input->dims()[3];

    int K_H = filter.dims()[2];
    int K_W = filter.dims()[3];

    int C = output_grad->dims()[1];  // output channels
    int O_H = output_grad->dims()[2];
    int O_W = output_grad->dims()[3];

    // Only im2col functor required for bp to get to the right shape
    paddle::operators::math::Im2ColFunctor<
        paddle::operators::math::ColFormat::kCFO, Place, T>
        im2col;

    // use col_shape in the im2col and col2im calculation
    DDim col_shape = {C, K_H, K_W, H, W};

    // use col_matrix_shape in the gemm calculation
    DDim col_matrix_shape_f = {C * H * W, K_H * K_W};

    Tensor col;
    col.mutable_data<T>(col_shape, context.GetPlace());
    // col_matrix shares the same piece of data with col,
    // but will be reshaped into a two-dimensional matrix shape
    // to call the matrix multiplication interface.

    DDim output_shape = {C, O_H, O_W};
    DDim input_matrix_shape = {M, H * W};

    DDim filter_matrix_shape = {M, C * K_H * K_W};
    filter.Resize(filter_matrix_shape);

    // deconvolution grad on input:
    // im2col + gemm (similar to conv-forward)
    // input need to compute gradient
    if (input_grad) {
      Tensor col_matrix = col;
      DDim col_matrix_shape = {C * K_H * K_W, H * W};
      col_matrix.Resize(col_matrix_shape);

      input_grad->mutable_data<T>(context.GetPlace());
      auto t = framework::EigenVector<T>::Flatten(*input_grad);
      t.device(context.GetEigenDevice<Place>()) = t.constant(static_cast<T>(0));

      for (int i = 0; i < N; i++) {
        // batch with size (C, O_H * O_W)
        Tensor output_grad_batch =
            output_grad->Slice<T>(i, i + 1).Resize(output_shape);
        // filter of size (M, C * K_H * K_W)

        // batch with size (M, H, W)
        Tensor input_grad_batch =
            input_grad->Slice<T>(i, i + 1).Resize(input_matrix_shape);

        // im2col: dy from (C, O_H, O_W) -> (C * K_H * K_W, H * W)
        im2col(context.device_context(), output_grad_batch, col, strides[0],
               strides[1], paddings[0], paddings[1]);

        // gemm: dx = filter * dy
        // (M, C * K_H * K_W) * (C * K_H * K_W, H * W) -> (M, C, H)
        math::matmul<Place, T>(context.device_context(), filter, false,
                               col_matrix, false, T(1.0), &input_grad_batch,
                               T(0.0));
      }
    }

    // filter gradient required
    if (filter_grad) {
      Tensor col_matrix_f = col;
      DDim col_matrix_shape_f = {C * H * W, K_H * K_W};
      col_matrix_f.Resize(col_matrix_shape_f);

      filter_grad->mutable_data<T>(context.GetPlace());
      Tensor filter_grad_ = *filter_grad;
      filter_grad_.Resize(filter_matrix_shape);
      auto t = framework::EigenVector<T>::Flatten(filter_grad_);
      t.device(context.GetEigenDevice<Place>()) = t.constant(static_cast<T>(0));

      for (int i = 0; i < N; ++i) {
        // batch with size (C, O_H, O_W)
        Tensor output_grad_batch =
            output_grad->Slice<T>(i, i + 1).Resize(output_shape);
        // input batch
        Tensor in_batch = input->Slice<T>(i, i + 1).Resize(input_matrix_shape);

        // im2col: (C * H * W, K_H * K_W)
        im2col(context.device_context(), output_grad_batch, col, strides[0],
               strides[1], paddings[0], paddings[1]);

        // gemm: d_filter = x * y_grad^T
        // (M, C * H * W) * (K_H * K_W, C * H * W) -> (M, C, H)
        math::matmul<Place, T>(context.device_context(), in_batch, false,
                               col_matrix_f, true, T(1.0), &filter_grad_,
                               T(1.0));
      }
    }
  }
};

}  // namespace operators
}  // namespace paddle
