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
#include "oneflow/core/common/balanced_splitter.h"
#include "oneflow/core/framework/framework.h"
#include "oneflow/user/ops/reshape_user_op_util.h"
#include "oneflow/core/operator/operator.h"
#include "oneflow/core/framework/op_generated.h"

namespace oneflow {

/*static*/ Maybe<void> ReshapeOp::GetSbp(user_op::SbpContext* ctx) {
  const auto& in_shape = ctx->LogicalTensorDesc4InputArgNameAndIndex("in", 0).shape();
  const Shape& shape = ctx->Attr<Shape>("shape");
  const auto& outshape = JUST(ReshapeUserOpUtil::GetLogicalOutBlobShape(in_shape, shape));
  user_op::UserOpSbpSignatureBuilder builder = ctx->NewBuilder();
  return ReshapeUserOpUtil::GetReshapeUserOpSbpSignatures(
      in_shape, *outshape, {{"in", 0}}, {{"out", 0}}, ctx->parallel_num(), &builder);
}

/*static*/ Maybe<void> ReshapeOp::InferNdSbp(user_op::InferNdSbpFnContext* ctx) {
  const Shape& in_shape = ctx->LogicalTensorDesc4InputArgNameAndIndex("in", 0).shape();
  const Shape& shape = ctx->user_op_conf().attr<Shape>("shape");
  const auto& out_shape = JUST(ReshapeUserOpUtil::GetLogicalOutBlobShape(in_shape, shape));
  return ReshapeUserOpUtil::InferNdSbp(ctx, in_shape, *out_shape);
}

/*static*/ Maybe<void> ReshapeOp::InferLogicalTensorDesc(user_op::InferContext* ctx) {
  Shape shape = ctx->Attr<Shape>("shape");
  const user_op::TensorDesc& in_tensor_desc = ctx->InputTensorDesc("in", 0);
  user_op::TensorDesc* out_tensor_desc = ctx->OutputTensorDesc("out", 0);
  const Shape& in_shape = in_tensor_desc.shape();
  Shape* out_shape = out_tensor_desc->mut_shape();
  CHECK_OR_RETURN(in_tensor_desc.is_dynamic() == false);
  *out_tensor_desc = in_tensor_desc;
  if (in_shape.NumAxes() == 0 || shape.NumAxes() == 0) {
    // NOTE(chengcheng): input/output Scalar
    // do nothing
  } else {
    CHECK_GE_OR_RETURN(shape.NumAxes(), 1);
    CHECK_GE_OR_RETURN(in_shape.NumAxes(), 1);
    int need_infer_axis = -1;
    size_t count = 1;
    for (int i = 0; i < shape.NumAxes(); ++i) {
      if (shape.At(i) == -1) {
        CHECK_EQ_OR_RETURN(need_infer_axis, -1)
            << "Shape " << shape.ToString() << " has more than 1 axis that needs to be infered.";
        need_infer_axis = i;
      } else {
        count *= shape.At(i);
      }
    }
    if (need_infer_axis != -1) { shape.Set(need_infer_axis, in_shape.elem_cnt() / count); }
  }
  *out_shape = shape;
  CHECK_EQ_OR_RETURN(out_shape->elem_cnt(), in_shape.elem_cnt());
  return Maybe<void>::Ok();
}

/*static*/ Maybe<void> ReshapeOp::InferPhysicalTensorDesc(user_op::InferContext* ctx) {
  Shape logical_shape = ctx->Attr<Shape>("shape");
  const user_op::TensorDesc& in_tensor_desc = ctx->InputTensorDesc("in", 0);
  user_op::TensorDesc* out_tensor_desc = ctx->OutputTensorDesc("out", 0);
  const Shape& in_shape = in_tensor_desc.shape();
  Shape* out_shape = out_tensor_desc->mut_shape();
  *out_tensor_desc->mut_shape() = in_tensor_desc.shape();
  *out_tensor_desc->mut_is_dynamic() = in_tensor_desc.is_dynamic();
  if (in_shape.NumAxes() == 0 || logical_shape.NumAxes() == 0) {
    // NOTE(chengcheng): input/output Scalar
    // do nothing
  } else {
    CHECK_GE_OR_RETURN(logical_shape.NumAxes(), 1);
    CHECK_GE_OR_RETURN(in_shape.NumAxes(), 1);
    const auto& in_nd_sbp = ctx->NdSbp4ArgNameAndIndex("in", 0);
    const Shape in_logical_shape =
        *JUST(GetLogicalShape(in_shape, in_nd_sbp, ctx->parallel_desc()));
    int need_infer_axis = -1;
    size_t count = 1;
    for (int i = 0; i < logical_shape.NumAxes(); ++i) {
      if (logical_shape.At(i) == -1) {
        CHECK_EQ_OR_RETURN(need_infer_axis, -1)
            << "Shape " << logical_shape.ToString()
            << " has more than 1 axis that needs to be infered.";
        need_infer_axis = i;
      } else {
        count *= logical_shape.At(i);
      }
    }
    if (need_infer_axis != -1) {
      logical_shape.Set(need_infer_axis, in_logical_shape.elem_cnt() / count);
    }
  }
  const auto& nd_sbp = ctx->NdSbp4ArgNameAndIndex("out", 0);
  *out_shape =
      *JUST(GetPhysicalShape(logical_shape, nd_sbp, ctx->parallel_desc(), ctx->parallel_ctx()));
  CHECK_EQ_OR_RETURN(out_shape->elem_cnt(), in_shape.elem_cnt())
      << " Reshape infer ERROR! in op_name: " << ctx->op_name()
      << " input shape is : " << in_shape.ToString()
      << " , output shape is : " << out_shape->ToString() << " , output logical shape is "
      << logical_shape.ToString()
      << " , And reshape shape conf is : " << ctx->Attr<Shape>("shape").ToString()
      << " op_loc: " << ctx->op_loc();
  return Maybe<void>::Ok();
}

/*static*/ Maybe<void> ReshapeOp::InferDataType(user_op::InferContext* ctx) {
  *ctx->OutputDType("out", 0) = ctx->InputDType("in", 0);
  return Maybe<void>::Ok();
}

namespace {

REGISTER_USER_OP_GRAD("reshape").SetGenBackwardOpConfFn([](const user_op::UserOpWrapper& op,
                                                           user_op::AddOpFn AddOp) -> Maybe<void> {
  if (op.NeedGenGradTensor4OpInput("in", 0)) {
    const auto& in_desc = op.TensorDesc4ArgNameAndIndex("in", 0);
    user_op::UserOpConfWrapperBuilder builder(op.op_name() + "_grad");
    if (in_desc.is_dynamic()) {
      user_op::UserOpConfWrapper reshape_grad_op =
          builder.Op("reshape_like")
              .Input("in", op.GetGradTensorWithOpOutput("out", 0))
              .Input("like", op.input("in", 0))
              .Output("out")
              .Build();
      op.BindGradTensorWithOpInput(reshape_grad_op.output("out", 0), "in", 0);
      AddOp(reshape_grad_op);
    } else {
      user_op::UserOpConfWrapper reshape_grad_op =
          builder.Op("reshape")
              .Input("in", op.GetGradTensorWithOpOutput("out", 0))
              .Attr("shape", in_desc.shape())
              .Output("out")
              .Build();
      op.BindGradTensorWithOpInput(reshape_grad_op.output("out", 0), "in", 0);
      AddOp(reshape_grad_op);
    }
  }
  return Maybe<void>::Ok();
});

}  // namespace
}  // namespace oneflow
