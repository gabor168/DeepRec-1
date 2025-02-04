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
#pragma GCC system_header

#ifndef TENSORFLOW_H_
#define TENSORFLOW_H_

#define EIGEN_USE_THREADS
#if GOOGLE_CUDA
#define EIGEN_USE_GPU
#endif  // GOOGLE_CUDA

#include "tensorflow/core/common_runtime/device.h"
#include "tensorflow/core/framework/bounds_check.h"
#include "tensorflow/core/framework/common_shape_fns.h"
#include "tensorflow/core/framework/dataset_stateful_op_whitelist.h"
#include "tensorflow/core/framework/device_attributes.pb.h"
#include "tensorflow/core/framework/node_def.pb.h"
#include "tensorflow/core/framework/numeric_op.h"
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/framework/resource_mgr.h"
#include "tensorflow/core/framework/shape_inference.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/framework/tensor_types.h"
#include "tensorflow/core/framework/types.h"
#include "tensorflow/core/lib/core/blocking_counter.h"
#include "tensorflow/core/lib/core/error_codes.pb.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/lib/core/threadpool.h"
#include "tensorflow/core/lib/strings/numbers.h"
#include "tensorflow/core/lib/strings/str_util.h"
#include "tensorflow/core/lib/strings/strcat.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/mutex.h"
#include "tensorflow/core/public/version.h"
#include "tensorflow/core/util/env_var.h"
#include "tensorflow/core/util/util.h"

#include "third_party/eigen3/Eigen/Core"
#include "third_party/eigen3/unsupported/Eigen/CXX11/Tensor"

#if GOOGLE_CUDA
#include "tensorflow/core/common_runtime/gpu/gpu_event_mgr.h"
#include "tensorflow/core/platform/cuda.h"
#include "tensorflow/stream_executor/stream_executor.h"
#endif  // GOOGLE_CUDA


#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <numeric>
#include <vector>

namespace tensorflow {

#define TF_RESOURCE_DEBUG_STRING_CONST const

class DataBuffer : public ResourceBase {
 public:
  explicit DataBuffer(int64 capacity)
      : capacity_(capacity), is_cancelled_(false), is_closed_(false) {}

  ~DataBuffer() { Cancel(); }

  Status Put(const std::vector<Tensor>& record, int64 timeout_millis) {
    std::unique_lock<std::mutex> lock(mu_);

    bool should_retry = !put_cv_.wait_for(
        lock, std::chrono::milliseconds(timeout_millis),
        [this]() { return buffer_.size() < capacity_ || is_cancelled_; });
    if (should_retry) {
      lock.unlock();
      LOG(WARNING) << "Prefetching was ignored since timeout.";
      return Status::OK();
    }

    if (TF_PREDICT_FALSE(is_cancelled_)) {
      lock.unlock();
      return Status(errors::Cancelled("Session was closed."));
    }

    buffer_.push_back(std::move(record));

    lock.unlock();
    take_cv_.notify_all();

    return Status::OK();
  }

  Status Take(std::vector<Tensor>* record) {
    std::unique_lock<std::mutex> lock(mu_);

    take_cv_.wait(lock, [this]() { return !buffer_.empty() || is_cancelled_; });

    if (TF_PREDICT_FALSE(is_closed_ && buffer_.empty())) {
      lock.unlock();
      return Status(errors::OutOfRange("EOF reached."));
    }

    if (TF_PREDICT_FALSE(is_cancelled_ && buffer_.empty())) {
      lock.unlock();
      return Status(errors::Cancelled("Session was closed."));
    }

    *record = std::move(buffer_.front());
    buffer_.pop_front();

    lock.unlock();
    put_cv_.notify_all();

    return Status::OK();
  }

  Status Cancel(bool is_cancelled = true) {
    std::unique_lock<std::mutex> lock(mu_);

    is_cancelled_ = is_cancelled;

    lock.unlock();
    put_cv_.notify_all();
    take_cv_.notify_all();
    return Status::OK();
  }

  Status Close() {
    std::unique_lock<std::mutex> lock(mu_);

    is_cancelled_ = true;
    is_closed_ = true;

    lock.unlock();
    put_cv_.notify_all();
    take_cv_.notify_all();
    return Status::OK();
  }

  Status GetSize(Tensor* size) {
    std::unique_lock<std::mutex> lock(mu_);
    size->scalar<int32>().setConstant(static_cast<int64>(buffer_.size()));
    return Status::OK();
  }

  string DebugString() TF_RESOURCE_DEBUG_STRING_CONST override {
    return strings::StrCat("DataBuffer(capacity=", capacity_, ")");
  }

  void Schedule(const string& name, int64 num_threads,
                std::function<void()> fn) {
    std::unique_lock<std::mutex> lock(mu_);
    if (threads_) {
      lock.unlock();
      threads_->Schedule(fn);
      return;
    }

    threads_.reset(
        new thread::ThreadPool(Env::Default(), ThreadOptions(),
                               strings::StrCat("data_buffer_threads_", name),
                               num_threads, false /* low_latency_hint */));

    lock.unlock();
    threads_->Schedule(fn);
  }

 private:
  std::deque<std::vector<Tensor> > buffer_;
  std::size_t capacity_;
  bool is_cancelled_;
  bool is_closed_;
  std::mutex mu_;
  std::condition_variable take_cv_;
  std::condition_variable put_cv_;
  std::shared_ptr<thread::ThreadPool> threads_;
};
}

#endif //TENSORFLOW_H_