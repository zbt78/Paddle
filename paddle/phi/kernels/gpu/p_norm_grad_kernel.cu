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
__device__ __forceinline__ int inline_sgn(T val) {
  return (T(0) < val) - (val < T(0));
}

template <typename T>
__device__ __forceinline__ T inline_pow(T base, T exponent) {
  return static_cast<T>(
      pow(static_cast<float>(base), static_cast<float>(exponent)));
}

template <>
__device__ __forceinline__ double inline_pow(double base, double exponent) {
  return pow(base, exponent);
}

template <typename T>
__device__ __forceinline__ T inline_abs(T x) {
  return static_cast<T>(abs(static_cast<float>(x)));
}

template <>
__device__ __forceinline__ double inline_abs(double x) {
  return abs(x);
}

template <typename T>
struct InfinityNormGradScalarDirectCUDAFunctor {
 private:
  const T y0_;
  const T dy0_;

 public:
  HOSTDEVICE inline InfinityNormGradScalarDirectCUDAFunctor(const T* y,
                                                            const T* dy)
      : y0_(y[0]), dy0_(dy[0]) {}

  HOSTDEVICE inline T operator()(const T x) const {
    return static_cast<T>(dy0_ * static_cast<T>(inline_sgn<T>(x)) *
                          static_cast<T>((inline_abs<T>(x) == y0_)));
  }
};

template <typename T>
struct InfinityNormGradTensorDirectCUDAFunctor {
  HOSTDEVICE inline T operator()(const T x, const T y, const T dy) const {
    return static_cast<T>(dy * static_cast<T>(inline_sgn<T>(x)) *
                          static_cast<T>(inline_abs<T>(x) == y));
  }
};

template <typename T>
struct OneNormGradScalarDirectCUDAFunctor {
 private:
  const T dy0_;

 public:
  HOSTDEVICE inline OneNormGradScalarDirectCUDAFunctor(const T* dy)
      : dy0_(dy[0]) {}

  HOSTDEVICE inline T operator()(const T x) const {
    return static_cast<T>(dy0_ * static_cast<T>(inline_sgn<T>(x)));
  }
};

template <typename T>
struct OneNormGradTensorDirectCUDAFunctor {
  HOSTDEVICE inline T operator()(const T x, const T y, const T dy) const {
    return static_cast<T>(dy * static_cast<T>(inline_sgn<T>(x)));
  }
};

template <typename T>
struct TwoNormGradScalarDirectCUDAFunctor {
 private:
  const T scalar_;

 public:
  HOSTDEVICE inline TwoNormGradScalarDirectCUDAFunctor(const T* y,
                                                       const T* dy,
                                                       const T epsilon)
      : scalar_(dy[0] / (y[0] + epsilon)) {}

  HOSTDEVICE inline T operator()(const T x) const { return scalar_ * x; }
};

template <typename T>
struct TwoNormGradTensorDirectCUDAFunctor {
 private:
  const T epsilon_;

 public:
  HOSTDEVICE inline TwoNormGradTensorDirectCUDAFunctor(const T epsilon)
      : epsilon_(epsilon) {}

  HOSTDEVICE inline T operator()(const T x, const T y, const T dy) const {
    return static_cast<T>(dy * x / (y + epsilon_));
  }
};

template <typename T>
struct PNormGradScalarDirectCUDAFunctor {
 private:
  const T porder_;
  const T scalar_;

 public:
  HOSTDEVICE inline PNormGradScalarDirectCUDAFunctor(const T* y,
                                                     const T* dy,
                                                     const T epsilon,
                                                     const T porder)
      : porder_(porder - static_cast<T>(1.)),
        scalar_(dy[0] *
                inline_pow<T>(y[0] + epsilon, static_cast<T>(-1) * porder_)) {}

  HOSTDEVICE inline T operator()(const T x) const {
    return static_cast<T>(static_cast<T>(inline_sgn<T>(x)) *
                          inline_pow<T>(inline_abs<T>(x), porder_) * scalar_);
  }
};

template <typename T>
struct PNormGradTensorDirectCUDAFunctor {
 private:
  const T epsilon_;
  const T porder_;

 public:
  HOSTDEVICE inline PNormGradTensorDirectCUDAFunctor(const T epsilon,
                                                     const T porder)
      : epsilon_(epsilon), porder_(porder - static_cast<T>(1.)) {}

  HOSTDEVICE inline T operator()(const T x, const T y, const T dy) const {
    return static_cast<T>(
        static_cast<T>(inline_sgn<T>(x)) *
        inline_pow<T>(inline_abs<T>(x), porder_) * dy *
        inline_pow<T>(y + epsilon_, static_cast<T>(-1.0) * porder_));
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
  dev_ctx.template Alloc<T>(x_grad);

  if (porder == 0) {
    phi::funcs::SetConstant<Context, T> set_zero;
    set_zero(dev_ctx, x_grad, static_cast<T>(0));
    return;
  }

  bool reduce_all = (out.numel() == 1);
  std::vector<DenseTensor*> outputs = {x_grad};

  if (reduce_all) {
    std::vector<const DenseTensor*> inputs = {};
    inputs.push_back(&x);
    const T* out_ptr = out.data<T>();
    const T* out_grad_ptr = out_grad.data<T>();

    if (porder == INFINITY || porder == -INFINITY) {
      auto functor =
          InfinityNormGradScalarDirectCUDAFunctor<T>(out_ptr, out_grad_ptr);
      funcs::ElementwiseKernel<T>(dev_ctx, inputs, &outputs, functor);
    } else if (porder == 1) {
      auto functor = OneNormGradScalarDirectCUDAFunctor<T>(out_grad_ptr);
      funcs::ElementwiseKernel<T>(dev_ctx, inputs, &outputs, functor);
    } else if (porder == 2) {
      auto functor = TwoNormGradScalarDirectCUDAFunctor<T>(
          out_ptr, out_grad_ptr, static_cast<T>(epsilon));
      funcs::ElementwiseKernel<T>(dev_ctx, inputs, &outputs, functor);
    } else {
      auto functor =
          PNormGradScalarDirectCUDAFunctor<T>(out_ptr,
                                              out_grad_ptr,
                                              static_cast<T>(epsilon),
                                              static_cast<T>(porder));
      funcs::ElementwiseKernel<T>(dev_ctx, inputs, &outputs, functor);
    }
  } else {
    DenseTensor out_copy(out);
    DenseTensor out_grad_copy(out_grad);

    if (!keepdim) {
      if (axis < 0) axis += x.dims().size();
      std::vector<int> shape;

      for (int i = 0; i < x.dims().size(); i++) {
        if (i < axis) {
          shape.push_back(out.dims()[i]);
        } else if (i == axis) {
          shape.push_back(1);
        } else {
          shape.push_back(out.dims()[i - 1]);
        }
      }

      DDim dims = phi::make_ddim(shape);
      out_copy.Resize(dims);
      out_grad_copy.Resize(dims);
    }
    std::vector<const DenseTensor*> inputs = {&x, &out_copy, &out_grad_copy};

    if (porder == INFINITY || porder == -INFINITY) {
      auto functor = InfinityNormGradTensorDirectCUDAFunctor<T>();
      funcs::BroadcastKernel<T>(dev_ctx, inputs, &outputs, functor);
    } else if (porder == 1) {
      auto functor = OneNormGradTensorDirectCUDAFunctor<T>();
      funcs::BroadcastKernel<T>(dev_ctx, inputs, &outputs, functor);
    } else if (porder == 2) {
      auto functor =
          TwoNormGradTensorDirectCUDAFunctor<T>(static_cast<T>(epsilon));
      funcs::BroadcastKernel<T>(dev_ctx, inputs, &outputs, functor);
    } else {
      auto functor = PNormGradTensorDirectCUDAFunctor<T>(
          static_cast<T>(epsilon), static_cast<T>(porder));
      funcs::BroadcastKernel<T>(dev_ctx, inputs, &outputs, functor);
    }
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
