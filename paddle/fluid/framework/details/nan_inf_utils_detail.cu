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

#include "paddle/fluid/framework/details/nan_inf_utils.h"
#include "paddle/fluid/framework/details/nan_inf_utils_detail.h"

#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>

namespace paddle {
namespace framework {
namespace details {

static std::once_flag init_multi_gpu_op_var_map_flag;

// lazy init
static std::vector<std::unordered_map<std::string, memory::AllocationPtr>>&
multi_op_var2gpu_str() {
  static std::vector<std::unordered_map<std::string, memory::AllocationPtr>>
      _multi_op_var2gpu_str;
  return _multi_op_var2gpu_str;
}

static std::vector<std::mutex>& multi_op_var2gpu_str_mutex() {
  static std::vector<std::mutex> _multi_op_var2gpu_str_mutex;
  return _multi_op_var2gpu_str_mutex;
}

static void InitMultiGPUOpVarMap() {
  int dev_count = platform::GetCUDADeviceCount();
  PADDLE_ENFORCE_GT(dev_count, 0,
                    platform::errors::NotFound(
                        "cuda device must > 0, now dev_count=%d", dev_count));

  // https://stackoverflow.com/questions/16465633/how-can-i-use-something-like-stdvectorstdmutex
  std::vector<std::unordered_map<std::string, memory::AllocationPtr>> tmp_multi(
      dev_count);
  std::vector<std::mutex> tmp_multi_mutex(dev_count);

  multi_op_var2gpu_str().swap(tmp_multi);
  multi_op_var2gpu_str_mutex().swap(tmp_multi_mutex);
}

template <typename T>
__device__ __forceinline__ void PrintNanInfKernel(const T* value,
                                                  const size_t numel,
                                                  int print_num,
                                                  char* debug_info) {
  const size_t tid = threadIdx.x + blockIdx.x * blockDim.x;

  __shared__ unsigned int nan_count, inf_count, num_count;
  if (threadIdx.x == 0) nan_count = inf_count = num_count = 0;
  __syncthreads;

  for (size_t i = tid; i < numel; i += blockDim.x * gridDim.x) {
    unsigned int count = 0;
    if (isnan(value[i])) {
      count = atomicAdd(&nan_count, 1);
    } else if (isinf(value[i])) {
      count = atomicAdd(&inf_count, 1);
    } else {
      count = atomicAdd(&num_count, 1);
    }
    // for cuda, print in every block
    if (count < print_num) {
      printf("numel:%lu idx:%lu value:%f\n", static_cast<uint64_t>(numel),
             static_cast<uint64_t>(i), static_cast<float>(value[i]));
    }
  }
  __syncthreads;

  if (true && threadIdx.x == 0) {
    printf("In block %d, there has %u,%u,%u nan,inf,num\n", blockIdx.x,
           nan_count, inf_count, num_count);
    PADDLE_ENFORCE(false, "===ERROR: in %s find nan or inf===", debug_info);
  }
}

// Resnet 2gpus speed test, no check 270 images/s, this check 229 images/s
template <typename T>
__global__ void CheckNanInfKernel(const T* value, const size_t numel,
                                  int print_num, char* debug_info) {
  /// step 1, judge wheater has nan or inf
  __shared__ volatile int has_nan_inf;
  if (threadIdx.x == 0) has_nan_inf = false;
  __syncthreads();

  const size_t tid = threadIdx.x + blockIdx.x * blockDim.x;
  T sum = static_cast<T>(0.0);
  // Todo(wangxi). simd speed up
  for (size_t i = tid; i < numel; i += blockDim.x * gridDim.x) {
    sum += (value[i] - value[i]);
  }

  if (isnan(sum) || isinf(sum)) has_nan_inf = true;
  __syncthreads();

  /// Note. different blocks may behave differently
  if (!has_nan_inf) return;

  PrintNanInfKernel(value, numel, print_num, debug_info);
}

template <>
template <typename T>
void TensorCheckerVisitor<platform::CUDADeviceContext>::apply(
    typename std::enable_if<std::is_floating_point<T>::value>::type*) const {
  int print_num = 3;

  auto* dev_ctx = reinterpret_cast<platform::CUDADeviceContext*>(
      platform::DeviceContextPool::Instance().Get(tensor_.place()));
  int dev_id = BOOST_GET_CONST(platform::CUDAPlace, tensor_.place()).device;
  PADDLE_ENFORCE_EQ(
      (dev_id >= 0 && dev_id < multi_op_var2gpu_str_mutex().size()), true,
      platform::errors::OutOfRange("GPU dev_id must >=0 and < dev_count=%d",
                                   multi_op_var2gpu_str_mutex().size()));

  std::string op_var = "[op=" + op_type_ + "] [tensor=" + var_name_ + "]";
  char* gpu_str_ptr = NULL;

  {
    auto& op_var2gpu_str_mutex = multi_op_var2gpu_str_mutex().at(dev_id);
    auto& op_var2gpu_str = multi_op_var2gpu_str().at(dev_id);

    std::lock_guard<std::mutex> guard(op_var2gpu_str_mutex);
    if (op_var2gpu_str.find(op_var) == op_var2gpu_str.end()) {  // insert
      auto gpu_str_tensor =
          paddle::memory::Alloc(*dev_ctx, op_var.length() + 1);
      gpu_str_ptr = reinterpret_cast<char*>(gpu_str_tensor->ptr());

      op_var2gpu_str.emplace(op_var, std::move(gpu_str_tensor));

      auto iter = op_var2gpu_str.find(op_var);
      PADDLE_ENFORCE_EQ(iter != op_var2gpu_str.end(), true,
                        platform::errors::PreconditionNotMet(
                            "op_var=%s should successed insert into "
                            "op_var2gpu_str, but now failed",
                            op_var));

      PADDLE_ENFORCE_CUDA_SUCCESS(
          cudaMemcpyAsync(gpu_str_ptr, iter->first.c_str(), op_var.length() + 1,
                          cudaMemcpyHostToDevice, dev_ctx->stream()));
    } else {  // get
      auto iter = op_var2gpu_str.find(op_var);
      PADDLE_ENFORCE_EQ(iter != op_var2gpu_str.end(), true,
                        platform::errors::PreconditionNotMet(
                            "op_var=%s should be in the op_var2gpu_str, but "
                            "now can't find it",
                            op_var));
      gpu_str_ptr = reinterpret_cast<char*>(iter->second->ptr());
    }
  }

  const size_t threads = 1024;
  size_t blocks =
      std::min(static_cast<size_t>(128),
               static_cast<size_t>((tensor_.numel() + threads - 1) / threads));
  CheckNanInfKernel<<<blocks, threads, 0, dev_ctx->stream()>>>(
      tensor_.data<T>(), tensor_.numel(), print_num, gpu_str_ptr);
}

template <>
void tensor_check<platform::CUDADeviceContext>(const std::string& op_type,
                                               const std::string& var_name,
                                               const framework::Tensor& tensor,
                                               const platform::Place& place) {
  std::call_once(init_multi_gpu_op_var_map_flag, InitMultiGPUOpVarMap);

  TensorCheckerVisitor<platform::CUDADeviceContext> vistor(op_type, var_name,
                                                           tensor, place);
  VisitDataType(tensor.type(), vistor);
}

template <typename T>
__global__ void CountNanInfNumKernel(const size_t len, const T* val,
                                     unsigned int* nan_num,
                                     unsigned int* inf_num) {
  /* Per block accumulator */
  __shared__ unsigned int block_nan, block_inf;
  const size_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (threadIdx.x == 0) {
    block_nan = 0;
    block_inf = 0;
  }
  __syncthreads();

  if (i < len) {
    unsigned int count = 0;
    if (isnan(val[i])) {
      count = atomicAdd(&block_nan, 1);
    } else if (isinf(val[i])) {
      count = atomicAdd(&block_inf, 1);
    }
    // for cuda, print in every block
    //    if (count > 0) {
    //      printf("numel:%lu idx:%lu value:%f\n", static_cast<uint64_t>(len),
    //             static_cast<uint64_t>(i), static_cast<float>(val[i]));
    //    }
  }
  __syncthreads();

  if (threadIdx.x == 0) {
    atomicAdd(nan_num, block_nan);
    atomicAdd(inf_num, block_inf);
  }
}

bool CudaTensorCheckNanInf(const std::string& op_type,
                           const std::string& var_name,
                           const framework::Tensor& tensor) {
  auto* dev_ctx = reinterpret_cast<platform::CUDADeviceContext*>(
      platform::DeviceContextPool::Instance().Get(tensor.place()));
  int dev_id = BOOST_GET_CONST(platform::CUDAPlace, tensor.place()).device;
  auto stream = dev_ctx->stream();
  auto gpu_tensor = paddle::memory::Alloc(*dev_ctx, sizeof(unsigned int) * 2);
  unsigned int* num_ptr = reinterpret_cast<unsigned int*>(gpu_tensor->ptr());
  PADDLE_ENFORCE_CUDA_SUCCESS(
      cudaMemsetAsync(num_ptr, 0, sizeof(unsigned int) * 2, stream));

  size_t len = static_cast<size_t>(tensor.numel());
  const size_t threads = 1024;
  size_t blocks = std::min(static_cast<size_t>(128),
                           static_cast<size_t>((len + threads - 1) / threads));
  if (tensor.type() == proto::VarType::FP32) {
    CountNanInfNumKernel<<<blocks, threads, 0, dev_ctx->stream()>>>(
        len, tensor.data<float>(), &num_ptr[0], &num_ptr[1]);
  } else if (tensor.type() == proto::VarType::INT64) {
    CountNanInfNumKernel<<<blocks, threads, 0, dev_ctx->stream()>>>(
        len, tensor.data<int64_t>(), &num_ptr[0], &num_ptr[1]);
  } else if (tensor.type() == proto::VarType::FP64) {
    CountNanInfNumKernel<<<blocks, threads, 0, dev_ctx->stream()>>>(
        len, tensor.data<double>(), &num_ptr[0], &num_ptr[1]);
  } else {
    return false;
  }

  unsigned int nan_inf_num[2] = {0};
  PADDLE_ENFORCE_CUDA_SUCCESS(cudaMemcpyAsync(&nan_inf_num[0], num_ptr,
                                              sizeof(unsigned int) * 2,
                                              cudaMemcpyDeviceToHost, stream));
  PADDLE_ENFORCE_CUDA_SUCCESS(cudaStreamSynchronize(stream));
  if (nan_inf_num[0] > 0 || nan_inf_num[1] > 0) {
    printf("device [%d], op %s, name: %s, there has %u,%u,%u nan,inf,num\n",
           dev_id, op_type.c_str(), var_name.c_str(), nan_inf_num[0],
           nan_inf_num[1], len);
    return true;
  }
  return false;
}

}  // namespace details
}  // namespace framework
}  // namespace paddle
