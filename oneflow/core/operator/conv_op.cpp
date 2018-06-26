#include "oneflow/core/operator/conv_op.h"
#include "oneflow/core/common/balanced_splitter.h"
#include "oneflow/core/device/cuda_stream_handle.h"

namespace oneflow {

namespace {

void GetOutAndPad(const Shape& in_blob_shape, const PbMessage& conv_conf, std::vector<int64_t>* out,
                  std::vector<int32_t>* pad_small_side, std::vector<int32_t>* pad_large_side) {
  int32_t opkernel_dim = in_blob_shape.NumAxes() - 2;
  if (out) { out->assign(opkernel_dim, 0); }
  if (pad_small_side) { pad_small_side->assign(opkernel_dim, 0); }
  if (pad_large_side) { pad_large_side->assign(opkernel_dim, 0); }
  const auto& data_format = GetValFromPbMessage<std::string>(conv_conf, "data_format");
  const std::string& padding = GetValFromPbMessage<std::string>(conv_conf, "padding");
  const PbRf<int32_t>& dilation_rate = GetPbRfFromPbMessage<int32_t>(conv_conf, "dilation_rate");
  const auto& strides = GetPbRfFromPbMessage<int32_t>(conv_conf, "strides");
  const PbRf<int32_t>& kernel_size = GetPbRfFromPbMessage<int32_t>(conv_conf, "kernel_size");
  FOR_RANGE(int32_t, i, 0, opkernel_dim) {
    GetWindowedOutputSize(in_blob_shape.At(DhwOffset(data_format) + i), kernel_size.Get(i),
                          dilation_rate.Get(i), strides.Get(i), padding,
                          out ? &(out->at(i)) : nullptr,
                          pad_small_side ? &(pad_small_side->at(i)) : nullptr,
                          pad_large_side ? &(pad_large_side->at(i)) : nullptr);
  }
}

}  // namespace

#ifdef WITH_CUDA
CudnnConvDesc::~CudnnConvDesc() { CudaCheck(cudnnDestroyConvolutionDescriptor(val_)); }

CudnnConvDesc::CudnnConvDesc(const DataType& data_type, const Shape& in_blob_shape,
                             const PbMessage& conv_conf) {
  int32_t opkernel_dim = in_blob_shape.NumAxes() - 2;
  CudaCheck(cudnnCreateConvolutionDescriptor(&val_));
  std::vector<int32_t> pad_large_side;
  GetOutAndPad(in_blob_shape, conv_conf, nullptr, nullptr, &pad_large_side);
  const PbRf<int32_t>& strides = GetPbRfFromPbMessage<int32_t>(conv_conf, "strides");
  const PbRf<int32_t>& dilation_rate = GetPbRfFromPbMessage<int32_t>(conv_conf, "dilation_rate");
  if (opkernel_dim == 2) {
    CudaCheck(cudnnSetConvolution2dDescriptor(val_, pad_large_side[0], pad_large_side[1],
                                              strides.Get(0), strides.Get(1), dilation_rate.Get(0),
                                              dilation_rate.Get(1), CUDNN_CROSS_CORRELATION,
                                              GetCudnnDataType(data_type)));
  } else if (opkernel_dim == 1) {
    CudaCheck(cudnnSetConvolution2dDescriptor(val_, pad_large_side[0], 0, strides.Get(0), 1,
                                              dilation_rate.Get(0), 1, CUDNN_CROSS_CORRELATION,
                                              GetCudnnDataType(data_type)));
  } else {
    CudaCheck(cudnnSetConvolutionNdDescriptor(
        val_, opkernel_dim, pad_large_side.data(), strides.data(), dilation_rate.data(),
        CUDNN_CROSS_CORRELATION, GetCudnnDataType(data_type)));
  }
}
#endif  // WITH_CUDA

template<int32_t NDims>
void ConvOp<NDims>::InitFromOpConf() {
  StrFieldTolower("data_format");
  StrFieldTolower("padding");

  EnrollInputBn("in");
  EnrollOutputBn("out");
  EnrollModelBn("weight");
  if (GetValFromCustomizedConf<bool>("use_bias")) {
    EnrollModelBn("bias");
    EnrollConstBufBn("bias_multiplier");
  }
}

template<int32_t NDims>
bool ConvOp<NDims>::NeedOutWhenBackward() const {
  ActivationType activation = static_cast<ActivationType>(GetEnumFromCustomizedConf("activation"));
  if (activation != ActivationType::kNone) {
    return true;
  } else {
    return false;
  }
}

template<int32_t NDims>
void ConvOp<NDims>::InferBlobDescs(std::function<BlobDesc*(const std::string&)> GetBlobDesc4BnInOp,
                                   const ParallelContext* parallel_ctx, size_t* buf_size,
                                   std::function<void(OpContext*)> EnrollOpCtx) const {
  const std::string& data_format = GetValFromCustomizedConf<std::string>("data_format");

  // in
  const BlobDesc* in_blob_desc = GetBlobDesc4BnInOp("in");
  CHECK_EQ(in_blob_desc->shape().NumAxes(), NDims + 2);
  CHECK_EQ(in_blob_desc->data_type(), Global<JobDesc>::Get()->DefaultDataType());

  // out
  int64_t data_num = in_blob_desc->shape().At(0);
  int32_t filters = GetValFromCustomizedConf<int32_t>("filters");
  if (parallel_ctx->policy() == kModelParallel) {
    BalancedSplitter splitter(filters, parallel_ctx->parallel_num());
    filters = splitter.At(parallel_ctx->parallel_id()).size();
  }
  std::vector<int64_t> out;
  GetOutAndPad(in_blob_desc->shape(), GetCustomizedConf(), &out, nullptr, nullptr);
  std::vector<int64_t> out_shape = {data_num, filters};
  size_t dhw_offset = DhwOffset(data_format);
  for (size_t i = 0; i < NDims; ++i) {
    out_shape.insert(out_shape.begin() + dhw_offset + i, out[i]);
  }
  BlobDesc* out_blob_desc = GetBlobDesc4BnInOp("out");
  *out_blob_desc = *in_blob_desc;
  out_blob_desc->mut_shape() = Shape(out_shape);

  // weight
  std::vector<int64_t> weight_shape(in_blob_desc->shape().dim_vec());
  weight_shape[0] = filters;
  for (size_t i = 0; i < NDims; ++i) {
    weight_shape[dhw_offset + i] = GetPbRfFromCustomizedConf<int32_t>("kernel_size").Get(i);
  }
  GetBlobDesc4BnInOp("weight")->mut_shape() = Shape(weight_shape);

  if (GetValFromCustomizedConf<bool>("use_bias")) {
    // bias and bias_multiplier
    GetBlobDesc4BnInOp("bias")->mut_shape() = Shape({filters, 1});
    if (!UseCudnnOnGpu()) {
      std::vector<int64_t> bias_mul_shape(NDims + 1, 1);
      for (size_t i = 0; i != NDims; ++i) { bias_mul_shape[i + 1] = out_shape[dhw_offset + i]; }
      GetBlobDesc4BnInOp("bias_multiplier")->mut_shape() = Shape(bias_mul_shape);
    }
  }

  ConvOpCtx* conv_op_ctx = new ConvOpCtx();
  EnrollOpCtx(conv_op_ctx);

  if (device_type() == DeviceType::kCPU || !UseCudnnOnGpu()) {
    // col_buf
    size_t col_buf_elem_cnt = 1;
    for (size_t i = 0; i != NDims + 1; ++i) { col_buf_elem_cnt *= weight_shape[i + 1]; }
    for (size_t i = 0; i != NDims; ++i) { col_buf_elem_cnt *= out_shape[dhw_offset + i]; }
    *buf_size = col_buf_elem_cnt * GetSizeOfDataType(in_blob_desc->data_type());
  }

#ifdef WITH_CUDA
  if (device_type() == DeviceType::kGPU && UseCudnnOnGpu()) {
    // cudnn_buf
    InferCudnnAlgo(GetBlobDesc4BnInOp, &(conv_op_ctx->cudnn_conv_algo_ctx));
    *buf_size = std::max({conv_op_ctx->cudnn_conv_algo_ctx.fwd_ws_size,
                          conv_op_ctx->cudnn_conv_algo_ctx.bwd_filter_ws_size,
                          conv_op_ctx->cudnn_conv_algo_ctx.bwd_data_ws_size});
  }
#endif  // WITH_CUDA
}

template<int32_t NDims>
void ConvOp<NDims>::GenKernelConfWithoutCudnn(
    std::function<const BlobDesc*(const std::string&)> GetBlobDesc4BnInOp,
    ConvKernelConf* conv_conf) const {
  const Shape& in_shape = GetBlobDesc4BnInOp("in")->shape();
  const Shape& weight_shape = GetBlobDesc4BnInOp("weight")->shape();
  std::string data_format = GetValFromCustomizedConf<std::string>("data_format");
  std::vector<int64_t> in = {GetInDim(in_shape, data_format, 0, NDims),
                             GetInDim(in_shape, data_format, 1, NDims),
                             GetInDim(in_shape, data_format, 2, NDims)};
  std::vector<int64_t> out;
  std::vector<int32_t> weight =
      Get3DVecInOpConf(this->GetPbRfFromCustomizedConf<int32_t>("kernel_size"), NDims);
  std::vector<int32_t> strides =
      Get3DVecInOpConf(this->GetPbRfFromCustomizedConf<int32_t>("strides"), NDims);
  std::vector<int32_t> dilation_rate =
      Get3DVecInOpConf(this->GetPbRfFromCustomizedConf<int32_t>("dilation_rate"), NDims);
  std::vector<int32_t> pad_small_side;
  std::vector<int32_t> pad_large_side;
  Get3DOutputSize(in, weight, strides, GetValFromCustomizedConf<std::string>("padding"), &out,
                  &pad_small_side, &pad_large_side, &dilation_rate);
  FOR_RANGE(size_t, i, 0, 3) {
    conv_conf->mutable_strides()->Add(strides.at(i));
    conv_conf->mutable_pad_small_side()->Add(pad_small_side.at(i));
    conv_conf->mutable_pad_large_side()->Add(pad_large_side.at(i));
    conv_conf->mutable_dilation_rate()->Add(dilation_rate.at(i));
  }
  const Shape& out_shape = GetBlobDesc4BnInOp("out")->shape();
  if (data_format == "channels_first") {
    Shape({in_shape.At(0), in_shape.At(1), in.at(0), in.at(1), in.at(2)})
        .ToProto(conv_conf->mutable_in());
    Shape({out_shape.At(0), out_shape.At(1), out.at(0), out.at(1), out.at(2)})
        .ToProto(conv_conf->mutable_out());
    Shape({weight_shape.At(0), weight_shape.At(1), weight.at(0), weight.at(1), weight.at(2)})
        .ToProto(conv_conf->mutable_weight());
  } else if (data_format == "channels_last") {
    Shape({in_shape.At(0), in.at(0), in.at(1), in.at(2), in_shape.At(in_shape.NumAxes() - 1)})
        .ToProto(conv_conf->mutable_in());
    Shape({out_shape.At(0), out.at(0), out.at(1), out.at(2), out_shape.At(out_shape.NumAxes() - 1)})
        .ToProto(conv_conf->mutable_out());
    Shape({weight_shape.At(0), weight.at(0), weight.at(1), weight.at(2),
           weight_shape.At(weight_shape.NumAxes() - 1)})
        .ToProto(conv_conf->mutable_weight());
  } else {
    UNIMPLEMENTED();
  }
}

template<int32_t NDims>
void ConvOp<NDims>::GenKernelConfWithCudnn(
    std::function<const BlobDesc*(const std::string&)> GetBlobDesc4BnInOp, KernelConf* kernel_conf,
    ConvKernelConf* conv_conf, const OpContext* op_ctx) const {
  GetBlobDesc4BnInOp("in")->shape().ToProto(conv_conf->mutable_in());
  GetBlobDesc4BnInOp("out")->shape().ToProto(conv_conf->mutable_out());
  GetBlobDesc4BnInOp("weight")->shape().ToProto(conv_conf->mutable_weight());

  std::vector<int32_t> pad_small_side;
  std::vector<int32_t> pad_large_side;
  GetOutAndPad(GetBlobDesc4BnInOp("in")->shape(), GetCustomizedConf(), nullptr, &pad_small_side,
               &pad_large_side);

  for (size_t i = 0; i < NDims; ++i) {
    AddValToPbRfInCustomizedKernelConf(kernel_conf, "pad_small_side", pad_small_side[i]);
    AddValToPbRfInCustomizedKernelConf(kernel_conf, "pad_large_side", pad_large_side[i]);
  }
#ifdef WITH_CUDA
  if (device_type() == DeviceType::kGPU) {
    const ConvOpCtx* conv_op_ctx = static_cast<const ConvOpCtx*>(op_ctx);
    SetValInCustomizedKernelConf(kernel_conf, "cudnn_fwd_algo",
                                 static_cast<int32_t>(conv_op_ctx->cudnn_conv_algo_ctx.fwd_algo));
    SetValInCustomizedKernelConf(
        kernel_conf, "cudnn_bwd_filter_algo",
        static_cast<int32_t>(conv_op_ctx->cudnn_conv_algo_ctx.bwd_filter_algo));
    SetValInCustomizedKernelConf(
        kernel_conf, "cudnn_bwd_data_algo",
        static_cast<int32_t>(conv_op_ctx->cudnn_conv_algo_ctx.bwd_data_algo));
  }
#endif  // WITH_CUDA
}

template<int32_t NDims>
void ConvOp<NDims>::VirtualGenKernelConf(
    std::function<const BlobDesc*(const std::string&)> GetBlobDesc4BnInOp,
    const ParallelContext* parallel_ctx, KernelConf* kernel_conf, const OpContext* op_ctx) const {
  ConvKernelConf* conv_conf = kernel_conf->mutable_conv_conf();
  conv_conf->set_dim(NDims);
  if (!UseCudnnOnGpu()) {
    GenKernelConfWithoutCudnn(GetBlobDesc4BnInOp, conv_conf);
  } else {
    GenKernelConfWithCudnn(GetBlobDesc4BnInOp, kernel_conf, conv_conf, op_ctx);
  }
}

template<int32_t NDims>
PbMessage* ConvOp<NDims>::MutableCustomizedKernelConf(KernelConf* kernel_conf) const {
  return kernel_conf->mutable_conv_conf();
}

template<int32_t NDims>
int32_t ConvOp<NDims>::ModelSplitAxis() const {
  if (GetValFromCustomizedConf<std::string>("data_format") == "channels_first") {
    return 1;
  } else if (GetValFromCustomizedConf<std::string>("data_format") == "channels_last") {
    return NDims + 1;
  } else {
    UNIMPLEMENTED();
  }
}

template<int32_t NDims>
int32_t ConvOp<NDims>::MaxModelSplitNum() const {
  return GetValFromCustomizedConf<int32_t>("filters");
}

#ifdef WITH_CUDA
template<int32_t NDims>
void ConvOp<NDims>::InferCudnnAlgo(
    std::function<const BlobDesc*(const std::string)> GetBlobDesc4BnInOp,
    CudnnConvAlgoCtx* conv_ctx) const {
  CudaStreamHandle cuda_handle(nullptr);

  const BlobDesc* in_blob_desc = GetBlobDesc4BnInOp("in");
  const BlobDesc* out_blob_desc = GetBlobDesc4BnInOp("out");
  const BlobDesc* weight_blob_desc = GetBlobDesc4BnInOp("weight");

  DataType data_type = in_blob_desc->data_type();
  CudnnTensorDesc in_desc(data_type, in_blob_desc->shape(),
                          GetValFromCustomizedConf<std::string>("data_format"));
  CudnnTensorDesc out_desc(data_type, out_blob_desc->shape(),
                           GetValFromCustomizedConf<std::string>("data_format"));
  CudnnFilterDesc filter_desc(data_type, weight_blob_desc->shape(),
                              GetValFromCustomizedConf<std::string>("data_format"));
  CudnnConvDesc conv_desc(in_blob_desc->data_type(), in_blob_desc->shape(), GetCustomizedConf());

  CudaCheck(cudnnGetConvolutionForwardAlgorithm(*cuda_handle.cudnn_handle(), in_desc.Get(),
                                                filter_desc.Get(), conv_desc.Get(), out_desc.Get(),
                                                CUDNN_CONVOLUTION_FWD_SPECIFY_WORKSPACE_LIMIT,
                                                cudnn_buf_limit_byte(), &conv_ctx->fwd_algo));
  CudaCheck(cudnnGetConvolutionBackwardFilterAlgorithm(
      *cuda_handle.cudnn_handle(), in_desc.Get(), out_desc.Get(), conv_desc.Get(),
      filter_desc.Get(), CUDNN_CONVOLUTION_BWD_FILTER_SPECIFY_WORKSPACE_LIMIT,
      cudnn_buf_limit_byte(), &conv_ctx->bwd_filter_algo));
  CudaCheck(cudnnGetConvolutionBackwardDataAlgorithm(
      *cuda_handle.cudnn_handle(), filter_desc.Get(), out_desc.Get(), conv_desc.Get(),
      in_desc.Get(), CUDNN_CONVOLUTION_BWD_DATA_SPECIFY_WORKSPACE_LIMIT, cudnn_buf_limit_byte(),
      &conv_ctx->bwd_data_algo));
  CudaCheck(cudnnGetConvolutionForwardWorkspaceSize(
      *cuda_handle.cudnn_handle(), in_desc.Get(), filter_desc.Get(), conv_desc.Get(),
      out_desc.Get(), conv_ctx->fwd_algo, &conv_ctx->fwd_ws_size));
  CudaCheck(cudnnGetConvolutionBackwardFilterWorkspaceSize(
      *cuda_handle.cudnn_handle(), in_desc.Get(), out_desc.Get(), conv_desc.Get(),
      filter_desc.Get(), conv_ctx->bwd_filter_algo, &conv_ctx->bwd_filter_ws_size));
  CudaCheck(cudnnGetConvolutionBackwardDataWorkspaceSize(
      *cuda_handle.cudnn_handle(), filter_desc.Get(), out_desc.Get(), conv_desc.Get(),
      in_desc.Get(), conv_ctx->bwd_data_algo, &conv_ctx->bwd_data_ws_size));
}
#endif  // WITH_CUDA

template class ConvOp<1>;
template class ConvOp<2>;
template class ConvOp<3>;

}  // namespace oneflow
