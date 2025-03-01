/*
Copyright 2020 The OneFlow Authors. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
#ifndef ONEFLOW_USER_KERNELS_COMMUNICATE_UTIL_H_
#define ONEFLOW_USER_KERNELS_COMMUNICATE_UTIL_H_

#include "oneflow/core/common/data_type.h"
#include "oneflow/core/ep/include/stream.h"

namespace oneflow {

// Send data from in to rank dst, if cur rank equal dst, memcopy will happen.
// Rank dst needs to call Recv with the same datatype and the same count from this rank.
template<DeviceType device_type>
Maybe<void> Send(const void* in, size_t elem_cnt, DataType dtype, int64_t dst, ep::Stream* stream);

// Receive data from rank src into out, if cur rank equal src, memcopy will happen.
// Rank src needs to call Send with the same datatype and the same count to this rank.
template<DeviceType device_type>
Maybe<void> Recv(void* out, size_t elem_cnt, DataType dtype, int64_t src, ep::Stream* stream);

}  // namespace oneflow

#endif  // ONEFLOW_USER_KERNELS_COMMUNICATE_UTIL_H_
