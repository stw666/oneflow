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
#include "oneflow/core/framework/framework.h"
#include "oneflow/core/framework/device.h"
#include "oneflow/core/framework/op_generated.h"

namespace oneflow {

namespace {

Maybe<Symbol<Device>> MakeOpDevice(const Symbol<Device>& in_device,
                                   const Symbol<Device>& out_device) {
  if (JUST(in_device->of_type()) == "gpu" && JUST(out_device->of_type()) == "cpu") {
    return Device::New("cuda_d2h", in_device->device_id());
  } else if (JUST(in_device->of_type()) == "cpu" && JUST(out_device->of_type()) == "gpu") {
    return Device::New("cuda_h2d", out_device->device_id());
  } else {
    return Device::New(out_device->type(), out_device->device_id());
  }
}

}  // namespace

/* static */ Maybe<void> CopyOp::InferLogicalTensorDesc(user_op::InferContext* ctx) {
  *ctx->OutputShape("out", 0) = ctx->InputShape("in", 0);
  *ctx->OutputIsDynamic("out", 0) = ctx->InputIsDynamic("in", 0);
  return Maybe<void>::Ok();
}

/*static*/ Maybe<void> CopyOp::InferPhysicalTensorDesc(user_op::InferContext* ctx) {
  return InferLogicalTensorDesc(ctx);
}

/* static */ Maybe<void> CopyOp::GetSbp(user_op::SbpContext* ctx) {
  const auto& inputs = ctx->inputs();
  CHECK_EQ_OR_RETURN(inputs.size(), 1);
  const auto& input =
      ctx->LogicalTensorDesc4InputArgNameAndIndex(inputs[0].first, inputs[0].second);
  for (int64_t axis = 0; axis < input.shape().NumAxes(); ++axis) {
    ctx->NewBuilder().Split(inputs, axis).Split(ctx->outputs(), axis).Build();
  }
  ctx->NewBuilder().PartialSum(inputs).PartialSum(ctx->outputs()).Build();
  return Maybe<void>::Ok();
}

/* static */ Maybe<void> CopyOp::InferDataType(user_op::InferContext* ctx) {
  *ctx->OutputDType("out", 0) = ctx->InputDType("in", 0);
  return Maybe<void>::Ok();
}

/* static */ Maybe<Symbol<Device>> CopyOp::InferDevice(user_op::DeviceInferContext* ctx) {
  Symbol<Device> out_device =
      JUST(Device::New(ctx->Attr<std::string>("device_type"), ctx->Attr<int64_t>("device_id")));
  *ctx->OutputTensorDevice4ArgNameAndIndex("out", 0) = out_device;
  const Symbol<Device>& in_device = ctx->InputTensorDevice4ArgNameAndIndex("in", 0);
  return MakeOpDevice(in_device, out_device);
}

}  // namespace oneflow
