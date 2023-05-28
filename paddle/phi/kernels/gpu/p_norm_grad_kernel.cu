// Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.
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

#include "paddle/phi/kernels/p_norm_grad_kernel.h"

#include "paddle/phi/core/kernel_registry.h"
#include "paddle/phi/kernels/funcs/math_function.h"
#include "paddle/phi/kernels/funcs/reduce_grad_functions.h"

#include "paddle/phi/backends/gpu/gpu_context.h"
#include "paddle/phi/kernels/funcs/broadcast_function.h"
#include "paddle/phi/kernels/funcs/elementwise_base.h"

namespace phi {

template <typename T>
struct AbsMaxAndMinGradFunctor {
  template <typename Context,
            typename X,
            typename Y,
            typename DX,
            typename DY,
            typename Dim>
  void operator()(const Context& place,
                  X* x,
                  Y* y,
                  DX* dx,
                  DY* dy,
                  const Dim& dim,
                  int size) {
    dx->device(place) = dy->broadcast(dim) * (*x).sign() *
                        ((*x).abs() == y->broadcast(dim)).template cast<T>();
  }
};

template <typename T>
struct PNormGradFunctor {
  HOSTDEVICE explicit inline PNormGradFunctor(float porder, float eps) {
    this->porder = static_cast<T>(porder - 1.);
    this->eps = static_cast<T>(eps);
  }
  template <typename Context,
            typename X,
            typename Y,
            typename DX,
            typename DY,
            typename Dim>
  void operator()(const Context& place,
                  X* x,
                  Y* y,
                  DX* dx,
                  DY* dy,
                  const Dim& dim,
                  int size) {
    dx->device(place) =
        (*x).abs().pow(this->porder) * (*x).sign() * dy->broadcast(dim) *
        (*y + y->constant(eps)).pow(-this->porder).broadcast(dim);
  }
  T porder;
  T eps;
};

template <typename T>
__device__ __forceinline__ int sgn(T val) {
  return (T(0) < val) - (val < T(0));
}

__device__ __forceinline__ dtype::float16 inline_abs(dtype::float16 x) {
  return static_cast<dtype::float16>(abs(static_cast<float>(x)));
}

__device__ __forceinline__ dtype::bfloat16 inline_abs(dtype::bfloat16 x) {
  return static_cast<dtype::bfloat16>(abs(static_cast<float>(x)));
}

__device__ __forceinline__ float inline_abs(float x) { return abs(x); }
__device__ __forceinline__ double inline_abs(double x) { return abs(x); }

__device__ __forceinline__ int inline_sign(dtype::float16 x) {
  return sgn<dtype::float16>(x);
}
__device__ __forceinline__ int inline_sign(dtype::bfloat16 x) {
  return sgn<dtype::bfloat16>(x);
}
__device__ __forceinline__ int inline_sign(float x) { return sgn<float>(x); }
__device__ __forceinline__ int inline_sign(double x) { return sgn<double>(x); }

__device__ __forceinline__ dtype::float16 inline_pow(dtype::float16 base,
                                                     dtype::float16 exponent) {
  return static_cast<dtype::float16>(
      pow(static_cast<float>(base), static_cast<float>(exponent)));
}
__device__ __forceinline__ dtype::bfloat16 inline_pow(
    dtype::bfloat16 base, dtype::bfloat16 exponent) {
  return static_cast<dtype::bfloat16>(
      pow(static_cast<float>(base), static_cast<float>(exponent)));
}
__device__ __forceinline__ float inline_pow(float base, float exponent) {
  return pow(base, exponent);
}
__device__ __forceinline__ double inline_pow(double base, double exponent) {
  return pow(base, exponent);
}

dtype::float16 host_pow(dtype::float16 base, dtype::float16 exponent) {
  return static_cast<dtype::float16>(
      pow(static_cast<float>(base), static_cast<float>(exponent)));
}
dtype::bfloat16 host_pow(dtype::bfloat16 base, dtype::bfloat16 exponent) {
  return static_cast<dtype::bfloat16>(
      pow(static_cast<float>(base), static_cast<float>(exponent)));
}

float host_pow(float base, float exponent) { return pow(base, exponent); }
double host_pow(double base, double exponent) { return pow(base, exponent); }

template <typename T>  // 这里T可能有问题
struct PNormGradScalarDirectCUDAFunctor {
  const T scalar_;
  const T porder_;

  HOSTDEVICE inline PNormGradScalarDirectCUDAFunctor(const T scalar,
                                                     const T porder)
      : scalar_(scalar), porder_(porder) {}

  HOSTDEVICE inline T operator()(const T x) const {
    return static_cast<T>(
        static_cast<T>(inline_sign(x)) *
        static_cast<T>(inline_pow(static_cast<T>(inline_abs(x)),
                                  static_cast<T>(porder_))) *
        scalar_);
  }
};

template <typename T>  // 这里T可能有问题
struct PNormGradTensorDirectCUDAFunctor {
  const T epsilon_;
  const T porder_;

  HOSTDEVICE inline PNormGradTensorDirectCUDAFunctor(const T epsilon,
                                                     const T porder)
      : epsilon_(epsilon), porder_(porder) {}

  // template <typename X, typename Y, typename DY>
  // HOSTDEVICE inline T operator()(const X x, const Y y, const DY dy) const {
  HOSTDEVICE inline T operator()(const T x, const T y, const T dy) const {
    // return inline_sign(x) *
    //        static_cast<T>(inline_pow(inline_abs(static_cast<T>(x)),
    //                                  static_cast<T>(porder_))) *
    //        dy *
    //        static_cast<T>(inline_pow(
    //            static_cast<T>(static_cast<T>(y) + static_cast<T>(epsilon_)),
    //            static_cast<T>(-1.0) * porder_));

    return static_cast<T>(inline_sign(x)) * inline_pow(inline_abs(x), porder_) *
           dy * inline_pow(y + epsilon_, static_cast<T>(-1.0) * porder_);
  }
};

template <typename T, typename Context>
void PNormGradKernel(const Context& dev_ctx,
                     const DenseTensor& x,
                     const DenseTensor& out,
                     const DenseTensor& out_grad,
                     float porder,
                     int axis,
                     float epsilon,
                     bool keepdim,
                     bool asvector,
                     DenseTensor* x_grad) {
  auto* in_x = &x;
  auto* in_norm = &out;
  auto* in_norm_dy = &out_grad;
  auto* out_dx = x_grad;
  dev_ctx.template Alloc<T>(out_dx);

  auto xdim = in_x->dims();
  bool reduce_all = (in_norm->numel() == 1);
  if (axis < 0) axis = xdim.size() + axis;
  const std::vector<int> dims = {axis};

  if (porder == 0) {
    phi::funcs::SetConstant<Context, T> set_zero;
    set_zero(dev_ctx, out_dx, static_cast<T>(0));
  } else if (porder == INFINITY || porder == -INFINITY) {
    AbsMaxAndMinGradFunctor<T> functor;
    funcs::LaunchReduceGradKernel<Context, T, AbsMaxAndMinGradFunctor<T>>(
        dev_ctx, in_x, in_norm, in_norm_dy, out_dx, functor, dims, reduce_all);

  } else {
    // dims没处理
    if (reduce_all) {
      auto functor = PNormGradScalarDirectCUDAFunctor<T>(
          static_cast<T>(static_cast<T>((*out_grad.data<T>())) *
                         host_pow(static_cast<T>((*out.data<T>())) +
                                      static_cast<T>(epsilon),
                                  static_cast<T>(-1.0 * porder))),
          static_cast<T>(porder));
      std::vector<const DenseTensor*> inputs = {&x};
      std::vector<DenseTensor*> outputs = {x_grad};
      funcs::ElementwiseKernel<T>(dev_ctx, inputs, &outputs, functor);
    } else {
      auto functor = PNormGradTensorDirectCUDAFunctor<T>(
          static_cast<T>(epsilon), static_cast<T>(porder));
      std::vector<const DenseTensor*> inputs = {&x, &out, &out_grad};
      std::vector<DenseTensor*> outputs = {x_grad};
      funcs::BroadcastKernel<T>(dev_ctx, inputs, &outputs, functor, 0);
    }

    // auto functor = PNormGradFunctor<T>(porder, epsilon);
    // funcs::LaunchReduceGradKernel<Context, T, PNormGradFunctor<T>>(
    //     dev_ctx, in_x, in_norm, in_norm_dy, out_dx, functor, dims,
    //     reduce_all);
  }
}
}  // namespace phi
PD_REGISTER_KERNEL(p_norm_grad,
                   GPU,
                   ALL_LAYOUT,
                   phi::PNormGradKernel,
                   float,
                   double,
                   phi::dtype::float16,
                   phi::dtype::bfloat16) {}
