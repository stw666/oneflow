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
#ifndef ONEFLOW_CORE_OPERATOR_USER_OP_H_
#define ONEFLOW_CORE_OPERATOR_USER_OP_H_

#include "oneflow/core/framework/user_op_registry_manager.h"
#include "oneflow/core/operator/operator.h"

namespace oneflow {

class UserOp final : public Operator {
 public:
  OF_DISALLOW_COPY_AND_MOVE(UserOp);
  UserOp() = default;
  ~UserOp() = default;

  using ArgVec = std::vector<std::pair<std::string, int32_t>>;

  Maybe<void> InitFromOpConf() override;
  Maybe<void> InferInternalBlobDescs(
      const std::function<BlobDesc*(const std::string&)>& GetBlobDesc4BnInOp,
      const ParallelContext* parallel_ctx, const JobDesc* job_desc) const override;
  Maybe<void> InferOutBlobDescs(
      const std::function<BlobDesc*(const std::string&)>& GetBlobDesc4BnInOp,
      const ParallelContext* parallel_ctx) const override;
  Maybe<void> InferLogicalOutBlobDescs(
      const std::function<BlobDesc*(const std::string&)>& BlobDesc4BnInOp,
      const ParallelDesc& parallel_desc) const override;
  Maybe<void> InferInplaceObn2Ibn(
      HashMap<std::string, std::string>* mut_inplace_obn2ibn,
      HashMap<std::string, std::string>* con_inplace_obn2ibn,
      const std::function<BlobDesc*(const std::string&)>& GetBlobDesc4BnInOp,
      const ParallelContext* parallel_ctx) const override;
  Symbol<OperatorConf> GetOpConfWithoutOpNameAndLbn() const override;
  const user_op::UserOpConfWrapper& user_op_conf() const;
  const ArgVec& inputs() const { return inputs_; }
  const ArgVec& outputs() const { return outputs_; }

 private:
  LogicalBlobId lbi4ibn(const std::string& input_bn) const override;
  LogicalBlobId lbi4obn(const std::string& output_bn) const override;
  Maybe<void> InferSbpSignature(
      cfg::SbpSignature* sbp_signature, const cfg::SbpSignature& sbp_sig_conf,
      const std::function<int32_t(const cfg::SbpSignature&)>& CalcOrderValue4SbpSig,
      std::function<Maybe<const SbpInferHint*>(const std::string&)> SbpInferHint4Ibn,
      const ParallelDesc& parallel_desc) const override;
  Maybe<void> GetSbpSignatures(
      const std::function<Maybe<const BlobDesc&>(const std::string&)>& LogicalBlobDesc4Ibn,
      const ParallelDesc& parallel_desc, cfg::SbpSignatureList* sbp_sig_list) const override;
  Maybe<void> InferOpTimeShape(
      const std::function<Maybe<const Shape>(const std::string&)>& GetTimeShape4BnInOp,
      std::shared_ptr<const Shape>* time_shape) const override;
  Maybe<void> InferNdSbpSignature(cfg::NdSbpSignature* nd_sbp_signature,
                                  const cfg::NdSbpSignature& nd_sbp_constraints,
                                  const ParallelDesc& parallel_desc,
                                  std::function<Maybe<const NdSbpInferHint*>(const std::string&)>
                                      NdSbpInferHint4Ibn) const override;
  void VirtualGenKernelConf(std::function<const BlobDesc*(const std::string&)> GetBlobDesc4BnInOp,
                            const ParallelContext* parallel_ctx,
                            KernelConf* kernel_conf) const override;

  const user_op::OpRegistryResult* val_;
  std::unique_ptr<user_op::UserOpConfWrapper> user_op_conf_;
  ArgVec inputs_;
  ArgVec outputs_;
};

}  // namespace oneflow

#endif  // ONEFLOW_CORE_OPERATOR_USER_OP_H_
