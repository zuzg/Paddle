/* Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

   http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License. */

#include "paddle/fluid/framework/data_layout_transform.h"
#include "paddle/fluid/framework/op_registry.h"
#include "paddle/fluid/memory/malloc.h"
#include "paddle/fluid/operators/conv_op.h"
#include "paddle/fluid/platform/mkldnn_helper.h"
#include "paddle/fluid/platform/mkldnn_reuse.h"

namespace paddle {
namespace operators {

using Tensor = framework::Tensor;
using framework::DataLayout;

inline dnnl::memory::dims GetWeightsTz(const Tensor* filter, const int groups) {
  auto weights_tz = phi::vectorize(filter->dims());
  int g = std::max(groups, 1);
  int g_dim = (g > 1) ? 1 : 0;
  platform::GetGroupConvWeightsTz(weights_tz, g);
  // gIOHW -> gOIHW || IOHW -> OIHW
  std::swap(weights_tz[g_dim + 0], weights_tz[g_dim + 1]);
  return weights_tz;
}

static dnnl::memory::data_type GetDstType(bool is_int8,
                                          bool is_bfloat16,
                                          bool force_fp32_output,
                                          std::string fuse_activation) {
  auto dst_dt = dnnl::memory::data_type::f32;
  if (is_int8) {
    dst_dt = (fuse_activation == "relu" || fuse_activation == "relu6")
                 ? dnnl::memory::data_type::u8
                 : dnnl::memory::data_type::s8;
    if (force_fp32_output) {
      dst_dt = dnnl::memory::data_type::f32;
    }  
  } else {
    if (!force_fp32_output && is_bfloat16) {
      dst_dt = dnnl::memory::data_type::bf16;
    }
  }
  return dst_dt;
}

template <typename T, typename K, typename T_out>
class ConvTransposeMKLDNNHandlerT
    : public platform::MKLDNNHandlerNoCachingT<T, dnnl::deconvolution_forward> {
 public:
  ConvTransposeMKLDNNHandlerT(const framework::ExecutionContext& ctx,
                              const dnnl::engine mkldnn_engine,
                              const Tensor* input,
                              const Tensor* filter,
                              const Tensor* bias,
                              Tensor* output)
      : platform::MKLDNNHandlerNoCachingT<T, dnnl::deconvolution_forward>(
            mkldnn_engine, ctx.GetPlace()),
        is_test_(ctx.Attr<bool>("is_test")) {

    PADDLE_ENFORCE_EQ(is_test_,
                      true,
                      platform::errors::InvalidArgument(
                          "ConvTransposeMKLDNN works only for inference. "
                          "The attribute \'is_test\' value should be set to "
                          "True, but got is_test=False."));

    PADDLE_ENFORCE_EQ(
        input->layout(),
        DataLayout::kMKLDNN,
        platform::errors::InvalidArgument(
            "Got wrong layout = %d for Input tensor.", input->layout()));

    PADDLE_ENFORCE_EQ(
        filter->layout(),
        DataLayout::kMKLDNN,
        platform::errors::InvalidArgument(
            "The filter tensor's layout should be %d, but got %d.",
            DataLayout::kMKLDNN,
            filter->layout()));

    PADDLE_ENFORCE_EQ(
        input->dims().size(),
        4,
        platform::errors::InvalidArgument("Input must be with 4 dimensions, "
                                          "i.e. NCHW. but got dimension =%d",
                                          input->dims().size()));
    PADDLE_ENFORCE_EQ(
        filter->dims().size(),
        4,
        platform::errors::InvalidArgument("Filter must be with 4 dimensions, "
                                          "i.e. OIHW, but got dimension =%d",
                                          filter->dims().size()));

    if (bias) {
      PADDLE_ENFORCE_EQ(
          bias->layout(),
          DataLayout::kMKLDNN,
          platform::errors::InvalidArgument(
              "The bias tensor's laytout should be %d, but got %d.",
              DataLayout::kMKLDNN,
              bias->layout()));

      PADDLE_ENFORCE_EQ(
          bias->dims().size(),
          1,
          platform::errors::InvalidArgument("Bias must only have 1 dimension, "
                                            "i.e. X, but got dimension = %d .",
                                            bias->dims().size()));
    }

    std::vector<int> strides_temp = ctx.Attr<std::vector<int>>("strides");
    dnnl::memory::dims strides(begin(strides_temp), end(strides_temp));

    std::vector<int> paddings_temp = ctx.Attr<std::vector<int>>("paddings");
    dnnl::memory::dims paddings(begin(paddings_temp), end(paddings_temp));

    std::vector<int> dilations_temp = ctx.Attr<std::vector<int>>("dilations");
    dnnl::memory::dims dilations(begin(dilations_temp), end(dilations_temp));

    int groups = ctx.Attr<int>("groups");
    std::string padding_algorithm = ctx.Attr<std::string>("padding_algorithm");

    PADDLE_ENFORCE_EQ(
        strides.size(),
        2,
        platform::errors::Unimplemented(
            "Now we only support 2d oneDNN convolution transpose op"));

    const auto& input_dims = input->dims();
    const auto data_dims = phi::slice_ddim(input_dims, 2, input_dims.size());
    const auto& filter_dims = filter->dims();
    const auto filter_data_dims =
        phi::slice_ddim(filter_dims, 2, filter_dims.size());

    const auto ksize = phi::vectorize(filter_data_dims);
    
    UpdatePaddingAndDilation(
        &paddings, &dilations, padding_algorithm, data_dims, strides, ksize);

    std::transform(
        dilations.begin(), dilations.end(), dilations.begin(), [](int64_t i) {
          return i - 1;
        });

    const auto src_tz = phi::vectorize(input->dims());
    const auto weights_tz = GetWeightsTz(filter, groups);
    const auto dst_tz = phi::vectorize(output->dims());
    const auto mkldnn_paddings = platform::ToMkldnnPadding(paddings);

    /* create memory descriptor for convolution without specified format
     * ('any') which lets a primitive (convolution in this case) choose
     * the memory format preferred for best performance
     */
    const auto chosen_memory_format = MKLDNNMemoryFormat::any;

    auto data_type = dnnl::memory::data_type::f32;
    if (ctx.Attr<std::string>("mkldnn_data_type") == "bfloat16" ||
        std::is_same<T_out, platform::bfloat16>::value)
      data_type = dnnl::memory::data_type::bf16;

    dnnl::memory::desc src_md, weights_md;
      if (platform::is_int8<T>()) {
        src_md = platform::MKLDNNMemDesc(
            src_tz,
            framework::ToMKLDNNDataType(
                framework::TransToProtoVarType(input->dtype())),
            chosen_memory_format);
        weights_md = platform::MKLDNNMemDesc(
            weights_tz, dnnl::memory::data_type::s8, chosen_memory_format);
      } else {
        src_md =
            platform::MKLDNNMemDesc(src_tz, data_type, chosen_memory_format);
        weights_md = platform::MKLDNNMemDesc(
            weights_tz, data_type, MKLDNNMemoryFormat::any);
      }
    const auto dst_md = platform::MKLDNNMemDesc(
        dst_tz, platform::MKLDNNGetDataType<T_out>(), chosen_memory_format);
    const dnnl::primitive_attr conv_trans_attr = CreateConvAttrs(ctx);
    auto fwd_prop_kind = is_test_ ? dnnl::prop_kind::forward_inference
                                  : dnnl::prop_kind::forward_training;
    if (bias) {
      std::vector<int64_t> bias_tz = phi::vectorize(bias->dims());
      dnnl::memory::desc bias_md;
      if (platform::is_int8<T>()) {
        bias_md = platform::MKLDNNMemDesc(
            bias_tz, dnnl::memory::data_type::s32, MKLDNNMemoryFormat::x);
      } else {
        bias_md = platform::MKLDNNMemDesc(
          bias_tz, data_type, MKLDNNMemoryFormat::x);
      }
      this->AcquireForwardPrimitiveDescriptor(
          conv_trans_attr,
          fwd_prop_kind,
          dnnl::algorithm::deconvolution_direct,
          src_md,
          weights_md,
          bias_md,
          dst_md,
          strides,
          dilations,
          mkldnn_paddings[0],
          mkldnn_paddings[1]);
    } else {
      this->AcquireForwardPrimitiveDescriptor(
          conv_trans_attr,
          fwd_prop_kind,
          dnnl::algorithm::deconvolution_direct,
          src_md,
          weights_md,
          dst_md,
          strides,
          dilations,
          mkldnn_paddings[0],
          mkldnn_paddings[1]);
    }
  }

    std::shared_ptr<std::tuple<float, std::vector<float>>> get_int8_bias_scales(
      const framework::ExecutionContext& ctx) {
    // Get scales int8 bias key
    const std::string key_bs = this->key_ + "@bs";

    // Scales for int8 bias are to be cached to avoid
    // computing them each iteration
    auto bias_scale_tuple =
        std::static_pointer_cast<std::tuple<float, std::vector<float>>>(
            this->dev_ctx_.GetBlob(key_bs));
    if (bias_scale_tuple) return bias_scale_tuple;

    const auto* filter = ctx.Input<Tensor>("Filter");
    const auto& weights_tz = phi::vectorize(filter->dims());
    const int groups = std::max(ctx.Attr<int>("groups"), 1);

    const auto& scale_weights_data =
        ctx.Attr<std::vector<float>>("Scale_weights");
    const auto& scale_in_data = ctx.Attr<float>("Scale_in");

    bool is_multi_channel = scale_weights_data.size() > 1;
    int mask_reorder = is_multi_channel ? 1 << 0 : 1;

    int count = 1;
    if (is_multi_channel) {
      count *= weights_tz[0];
      if (groups > 1) {
        count *= weights_tz[1];
      }
    }

    bias_scale_tuple =
        std::make_shared<std::tuple<float, std::vector<float>>>(std::make_tuple(
            static_cast<float>(mask_reorder), std::vector<float>(count)));
    for (int i = 0; i < count; i++) {
      std::get<1>(*bias_scale_tuple)[i] = scale_in_data * scale_weights_data[i];
    }

    this->dev_ctx_.SetBlob(key_bs, bias_scale_tuple);

    return bias_scale_tuple;
  }

  std::tuple<float, std::vector<float>, float> get_int8_scales(
      const framework::ExecutionContext& ctx) const {
    const auto* filter = ctx.Input<Tensor>("Filter");
    const auto& weights_tz = phi::vectorize(filter->dims());
    const bool& force_fp32_output = ctx.HasAttr("force_fp32_output") ? ctx.Attr<bool>("force_fp32_output") : false;
    const int groups = std::max(ctx.Attr<int>("groups"), 1);

    const auto& scale_in_data = ctx.HasAttr("Scale_in") ? ctx.Attr<float>("Scale_in") : 1.0f;
    auto scale_weights_data = ctx.HasAttr("Scale_weights") ? ctx.Attr<std::vector<float>>("Scale_weights") : std::vector<float>(1.0f);
    bool is_multi_channel = scale_weights_data.size() > 1;
    float scale_out = ctx.HasAttr("Scale_out") ? ctx.Attr<float>("Scale_out") : 1.0f;
    bool has_activation = !ctx.Attr<std::string>("fuse_activation").empty();
    float activation_scale = (!force_fp32_output && has_activation)
                                 ? scale_out
                                 : 1.0f;

    float scale_out_data = (force_fp32_output || has_activation)
                               ? 1.0f
                               : scale_out;
    float sum_scale = 1.0f;
    int count =
        is_multi_channel
            ? (groups > 1 ? (weights_tz)[1] * (weights_tz)[0] : (weights_tz)[0])
            : 1;
    std::vector<float> output_shift_scale(count);

#pragma omp parallel for if (count > 50)
    for (int i = 0; i < count; i++) {
      if (scale_weights_data[i] == 0.0)
        // weights data will contain 0 in some models, then weights
        // scale couldn't be calculated
        output_shift_scale[i] = scale_out_data;
      else
        output_shift_scale[i] =
            static_cast<float>(static_cast<double>(scale_out_data) /
                               (static_cast<double>(scale_in_data) *
                                static_cast<double>(scale_weights_data[i])));
    }

    return std::make_tuple(sum_scale, output_shift_scale, activation_scale);
  }

  dnnl::primitive_attr CreateConvAttrs(const framework::ExecutionContext& ctx) {
    dnnl::primitive_attr conv_attr;
    dnnl::post_ops post_operations;

    float sum_scale = 1.0f;
    float activation_scale = 1.0f;
    std::vector<float> output_shift_scale;

     if (platform::is_int8<T>()) {
      if (ctx.HasAttr("Sum_scale")) {
        sum_scale = ctx.Attr<float>("Sum_scale");
        activation_scale = ctx.Attr<float>("Activation_scale");
        output_shift_scale = ctx.Attr<std::vector<float>>("Output_shift_scale");
      } else {
        std::tie(sum_scale, output_shift_scale, activation_scale) =
            get_int8_scales(ctx);
      }

      if (output_shift_scale.size() > 0) {
        int mask = output_shift_scale.size() > 1 ? 1 << 1 : 0;
        conv_attr.set_output_scales(mask, output_shift_scale);
      }
    }

    platform::AppendActivation(ctx, post_operations, activation_scale);

    conv_attr.set_post_ops(post_operations);
    return conv_attr;
  }

  std::shared_ptr<dnnl::memory> AcquireSrcMemoryWithReorder(
      const framework::Tensor* input) {
    const T* input_data = input->data<T>();
    return platform::MKLDNNHandlerNoCachingT<T, dnnl::deconvolution_forward>::
        AcquireMemoryWithReorder(input->mem_desc(),
                                 this->fwd_pd_->src_desc(),
                                 platform::to_void_cast<T>(input_data));
  }

  std::shared_ptr<dnnl::memory> AcquireWeightsMemoryWithReorder(
      const platform::MKLDNNDeviceContext& dev_ctx,
      const std::string& key,
      const framework::Tensor* filter,
      const int& groups,
      const std::vector<float>& scale_data = {1.0f}) {
    const K* filter_data = filter->data<K>();
    auto weights_tz = GetWeightsTz(filter, groups);
    int g = std::max(groups, 1);

    auto user_src_md = platform::MKLDNNMemDesc(
        weights_tz,
        platform::MKLDNNGetDataType<K>(),
        (g == 1) ? MKLDNNMemoryFormat::iohw : MKLDNNMemoryFormat::giohw);

    return this->template AcquireMemoryWithReorder<K>(
        dev_ctx,
        user_src_md,
        this->fwd_pd_->weights_desc(),
        platform::to_void_cast<K>(filter_data),
        key,
        "@weights_mem_p",
        is_test_,
        scale_data);
  }

  template <typename F = T>
  std::shared_ptr<dnnl::memory> AcquireMemoryWithReorder(
      const platform::MKLDNNDeviceContext& dev_ctx,
      const dnnl::memory::desc& user_md,
      const dnnl::memory::desc& target_md,
      void* ptr,
      const std::string& key,
      const std::string& suffix,
      bool is_persistent = false,
      const std::vector<float>& scale_data = {1.0f},
      int mask = 0) {
    const auto target_key = key + suffix + "_target";
    const auto key_reorder_p = key + suffix + "reorder_p";
    const auto user_key = key + suffix + "_user";

    auto target_memory_p =
        std::static_pointer_cast<dnnl::memory>(dev_ctx.GetBlob(target_key));

    if (target_memory_p == nullptr) {
      auto user_memory_p =
          std::make_shared<dnnl::memory>(user_md, this->engine_, ptr);
      if (user_md != target_md) {
        target_memory_p =
            std::make_shared<dnnl::memory>(target_md, this->engine_);
        dnnl::reorder::primitive_desc reorder_pdesc;
        if (platform::is_int8<T>()) {
          dnnl::primitive_attr attr;
          attr.set_output_scales(mask, scale_data);
          reorder_pdesc = dnnl::reorder::primitive_desc(
              *user_memory_p, *target_memory_p, attr);
        } else {
          reorder_pdesc =
              dnnl::reorder::primitive_desc(*user_memory_p, *target_memory_p);
        }
        auto reorder_p = std::make_shared<dnnl::reorder>(reorder_pdesc);
        dev_ctx.SetBlob(key_reorder_p, reorder_p);

        auto& astream = platform::MKLDNNDeviceContext::tls().get_stream();
        platform::RecordEvent record_reorder(
            "int_reorder",
            platform::TracerEventType::UserDefined,
            2,
            platform::EventRole::kUniqueOp);
        reorder_p->execute(
            astream,
            {{DNNL_ARG_FROM, *user_memory_p}, {DNNL_ARG_TO, *target_memory_p}});
        astream.wait();
      } else {
        target_memory_p = user_memory_p;
      }
      dev_ctx.SetBlob(user_key, user_memory_p);
      dev_ctx.SetBlob(target_key, target_memory_p);
    } else if (!is_persistent) {
      auto& astream = platform::MKLDNNDeviceContext::tls().get_stream();

      auto user_memory_p =
          std::static_pointer_cast<dnnl::memory>(dev_ctx.GetBlob(user_key));
      user_memory_p->set_data_handle(ptr);

      // TODO(jczaja): Here we detect if reorder is cached it means it is needed
      // need to change this to get rid of keys
      auto reorder_p = std::static_pointer_cast<dnnl::reorder>(
          dev_ctx.GetBlob(key_reorder_p));
      if (reorder_p != nullptr) {
        platform::RecordEvent record_reorder(
            "int_reorder",
            platform::TracerEventType::UserDefined,
            2,
            platform::EventRole::kUniqueOp);
        reorder_p->execute(
            astream,
            {{DNNL_ARG_FROM, *user_memory_p}, {DNNL_ARG_TO, *target_memory_p}});
        astream.wait();
      }
    }
    return target_memory_p;
  }

  std::shared_ptr<dnnl::memory> AcquireBiasMemoryWithReorder(
      const platform::MKLDNNDeviceContext& dev_ctx,
      const std::string& key,
      const framework::Tensor* bias,
      const std::vector<float>& scale_data = {1.0f}) {
    const K* bias_data = bias->data<K>();
    auto user_bias_md =
        platform::MKLDNNMemDesc(phi::vectorize(bias->dims()),
                                platform::MKLDNNGetDataType<K>(),
                                MKLDNNMemoryFormat::x);
    return this->AcquireMemoryWithReorder(dev_ctx,
                                          user_bias_md,
                                          this->fwd_pd_->bias_desc(),
                                          platform::to_void_cast<K>(bias_data),
                                          key,
                                          "@bias_mem_p",
                                          is_test_,
                                          scale_data);
  }

 private:
  const bool is_test_;
};

template <typename T, typename K>
class ConvTransposeMKLDNNOpKernel : public framework::OpKernel<T> {
 public:
  void Compute(const framework::ExecutionContext& ctx) const override {
    PADDLE_ENFORCE_EQ(platform::is_cpu_place(ctx.GetPlace()),
                      true,
                      platform::errors::PreconditionNotMet(
                          "Operator DNNL ConvTranspose must use CPUPlace"));
    const bool is_INT8 =
        std::is_same<T, int8_t>::value || std::is_same<T, uint8_t>::value;
    const bool is_BFLOAT16 =
        ctx.Attr<std::string>("mkldnn_data_type") == "bfloat16";
    std::string fuse_activation = ctx.Attr<std::string>("fuse_activation");
    const bool force_fp32_output = ctx.Attr<bool>("force_fp32_output");
    auto dst_dt = GetDstType(is_INT8,
                             is_BFLOAT16,
                             force_fp32_output,
                             fuse_activation);
    if (!is_INT8) {
      if (dst_dt == dnnl::memory::data_type::f32) {
        ComputeFP32<float>(ctx);
      } else if (dst_dt == dnnl::memory::data_type::bf16) {
        ComputeFP32<platform::bfloat16>(ctx);
      }
    } else {
      if (dst_dt == dnnl::memory::data_type::f32) {
        ComputeINT8<float>(ctx);
      } else if (dst_dt == dnnl::memory::data_type::u8) {
        ComputeINT8<uint8_t>(ctx);
      } else if (dst_dt == dnnl::memory::data_type::s8) {
        ComputeINT8<int8_t>(ctx);
      }
    }
  }

  template <typename T_out>
  void ComputeFP32(const framework::ExecutionContext& ctx) const {
    auto& dev_ctx =
        ctx.template device_context<platform::MKLDNNDeviceContext>();
    const auto& mkldnn_engine = dev_ctx.GetEngine();

    const auto* input = ctx.Input<Tensor>("Input");
    const auto* filter = ctx.Input<Tensor>("Filter");
    const auto* bias =
        ctx.HasInput("Bias") ? ctx.Input<Tensor>("Bias") : nullptr;
    auto* output = ctx.Output<Tensor>("Output");
    ConvTransposeMKLDNNHandlerT<T, K, T_out> handler(
        ctx, mkldnn_engine, input, filter, bias, output);
    auto src_memory_p = handler.AcquireSrcMemoryWithReorder(input);
    // Caching Key for weights is needed
    std::string key = platform::CreateKey(dev_ctx,
                                          ctx.InputName("Input"),
                                          ctx.InputName("Filter"),
                                          (bias ? ctx.InputName("Bias") : ""));
    key = platform::ExtendKeyWithThreadInfoIfNeeded(dev_ctx, key);
    auto weights_memory_p = handler.AcquireWeightsMemoryWithReorder(
        dev_ctx, key, filter, ctx.Attr<int>("groups"));
    std::shared_ptr<dnnl::memory> dst_memory_p =
        handler.template AcquireDstMemory<T_out>(output);
    
    auto conv_p = handler.AcquireForwardPrimitive();

    std::unordered_map<int, dnnl::memory> args = {
        {DNNL_ARG_SRC, *src_memory_p},
        {DNNL_ARG_WEIGHTS, *weights_memory_p},
        {DNNL_ARG_DST, *dst_memory_p}};

    if (bias) {
      auto bias_memory_p =
          handler.AcquireBiasMemoryWithReorder(dev_ctx, key, bias);
      args.insert({DNNL_ARG_BIAS, *bias_memory_p});
    }
    auto& astream = platform::MKLDNNDeviceContext::tls().get_stream();
    conv_p->execute(astream, args);
    astream.wait();
    output->set_mem_desc(dst_memory_p->get_desc());
  }

  template <typename T_out>
  void ComputeINT8(const framework::ExecutionContext& ctx) const {
    auto& dev_ctx =
        ctx.template device_context<platform::MKLDNNDeviceContext>();
    const auto& mkldnn_engine = dev_ctx.GetEngine();

    const std::string& fuse_activation =
        ctx.Attr<std::string>("fuse_activation");
    const bool is_conv_transpose_3d = ctx.Attr<std::vector<int>>("strides").size() == 3U;

    bool unsigned_output =
        (fuse_activation == "relu" || fuse_activation == "relu6");
    bool need_s8_to_u8 = false;

    PADDLE_ENFORCE_NE(
        is_conv_transpose_3d,
        true,
        platform::errors::Unimplemented(
            "OneDNN int8 conv_transpose does not support 3D inputs currently"));

    auto* input = ctx.Input<Tensor>("Input");
    auto* filter = ctx.Input<Tensor>("Filter");
    auto* bias = ctx.HasInput("Bias") ? ctx.Input<Tensor>("Bias") : nullptr;
    auto* output = ctx.Output<Tensor>("Output");

    ConvTransposeMKLDNNHandlerT<T, K, T_out> handler(
        ctx, mkldnn_engine, input, filter, bias, output);

    auto src_memory_p = handler.AcquireSrcMemoryWithReorder(input);
    // Caching Key for weights is needed
    std::string key = platform::CreateKey(dev_ctx,
                                          ctx.InputName("Input"),
                                          ctx.InputName("Filter"),
                                          (bias ? ctx.InputName("Bias") : ""));
    key = platform::ExtendKeyWithThreadInfoIfNeeded(dev_ctx, key);

    const auto& scale_weights_data = ctx.HasAttr("Scale_weights") ?
        ctx.Attr<std::vector<float>>("Scale_weights") : std::vector<float>(1.0f);
    const bool is_multi_channel = scale_weights_data.size() > 1;
    const int& groups = ctx.Attr<int>("groups");
    int mask_reorder =
        is_multi_channel ? ((groups != 1) ? (1 << 1) + (1 << 0) : 1 << 0) : 0;
    auto weights_memory_p = handler.AcquireWeightsMemoryWithReorder(
        dev_ctx, key, filter, ctx.Attr<int>("groups"));

    std::shared_ptr<dnnl::memory> dst_memory_p = 
        handler.template AcquireDstMemory<T_out>(output);

    need_s8_to_u8 = (platform::MKLDNNGetDataType<T_out>() ==
                       dnnl::memory::data_type::s8) &&
                      unsigned_output;
    
    auto conv_p = handler.AcquireForwardPrimitive();

    std::unordered_map<int, dnnl::memory> args = {
        {DNNL_ARG_SRC, *src_memory_p},
        {DNNL_ARG_WEIGHTS, *weights_memory_p},
        {DNNL_ARG_DST, *dst_memory_p}};

    if (bias) {
      PADDLE_ENFORCE_EQ(
        ctx.HasAttr("Bias_scales"),
        true,
        platform::errors::NotFound(
            "No scales for quantization saved in the attributes"));

      std::vector<float> bias_scales;
      auto p_scales_tuple =
          std::make_shared<std::tuple<float, std::vector<float>>>(
              std::make_tuple(static_cast<float>(mask_reorder), bias_scales));

      bias_scales = ctx.Attr<std::vector<float>>("Bias_scales");
      p_scales_tuple =
            std::make_shared<std::tuple<float, std::vector<float>>>(
                std::make_tuple(static_cast<float>(mask_reorder), bias_scales));
      auto bias_memory_p =
          handler.AcquireBiasMemoryWithReorder(dev_ctx, key, bias,
                                               std::get<1>(*p_scales_tuple));
      args.insert({DNNL_ARG_BIAS, *bias_memory_p});
    }

    auto& astream = platform::MKLDNNDeviceContext::tls().get_stream();
    conv_p->execute(astream, args);
    astream.wait();

    if (need_s8_to_u8) {
      output->mutable_data<uint8_t>(ctx.GetPlace());
    }

    output->set_layout(framework::DataLayout::kMKLDNN);
    output->set_format(platform::GetMKLDNNFormat(*dst_memory_p));
  }
};

}  // namespace operators
}  // namespace paddle

namespace ops = paddle::operators;

REGISTER_OP_KERNEL(
    conv2d_transpose,
    MKLDNN,
    ::paddle::platform::CPUPlace,
    ops::ConvTransposeMKLDNNOpKernel<float, float>,
    ops::ConvTransposeMKLDNNOpKernel<paddle::platform::bfloat16, float>,
    ops::ConvTransposeMKLDNNOpKernel<uint8_t, float>,
    ops::ConvTransposeMKLDNNOpKernel<uint8_t, int8_t>,
    ops::ConvTransposeMKLDNNOpKernel<int8_t, float>,
    ops::ConvTransposeMKLDNNOpKernel<int8_t, int8_t>);
