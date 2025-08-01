// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "contrib_ops/cpu/cpu_contrib_kernels.h"
#include "core/graph/constants.h"
#include "core/framework/int4.h"
#include "core/mlas/inc/mlas.h"

namespace onnxruntime {
namespace contrib {

class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, SampleOp);

class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, GridSample);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, Attention);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, BeamSearch);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, WhisperBeamSearch);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, EmbedLayerNormalization);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, ExpandDims);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, FusedConv);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, FusedGemm);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, GreedySearch);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, MultiHeadAttention);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, GroupQueryAttention);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, MLFloat16, GroupQueryAttention);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, SparseAttention);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, MLFloat16, SparseAttention);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, RotaryEmbedding);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, MLFloat16, RotaryEmbedding);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, Sampling);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, AttnLSTM);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, string, Tokenizer);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, Range);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, WordConvEmbedding);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, GatherND);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, TransposeMatMul);  // backward compatibility
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, FusedMatMul);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, MatMulNBits);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, MLFloat16, MatMulNBits);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, MatMulBnb4);
class ONNX_OPERATOR_TWO_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, uint8_t, int32_t, GatherBlockQuantized);
class ONNX_OPERATOR_TWO_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, uint8_t, int64_t, GatherBlockQuantized);
class ONNX_OPERATOR_TWO_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, UInt4x2, int32_t, GatherBlockQuantized);
class ONNX_OPERATOR_TWO_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, UInt4x2, int64_t, GatherBlockQuantized);
class ONNX_OPERATOR_TWO_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, Int4x2, int32_t, GatherBlockQuantized);
class ONNX_OPERATOR_TWO_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, Int4x2, int64_t, GatherBlockQuantized);
#ifndef ORT_MINIMAL_BUILD
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, MatMulFpQ4);
#endif
#if !defined(DISABLE_SPARSE_TENSORS)
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, SparseToDenseMatMul);
#endif
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, MurmurHash3);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, MaxpoolWithMask);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, Pad);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, Unique);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, ConvTransposeWithDynamicPads);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, CropAndResize);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, CDist);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, double, CDist);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, Gelu);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, BiasGelu);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, FastGelu);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, NGramRepeatBlock);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, BifurcationDetector);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, QuickGelu);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, DecoderMaskedMultiHeadAttention);

// ******** Start: Quantization ******************* //
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, MatMulInteger16);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, QLinearGlobalAveragePool);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, QLinearConcat);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, QLinearWhere);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, QLinearAveragePool);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, uint8_t, DequantizeLinear);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, int8_t, DequantizeLinear);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, uint16_t, DequantizeLinear);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, int16_t, DequantizeLinear);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, int32_t, DequantizeLinear);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, UInt4x2, DequantizeLinear);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, Int4x2, DequantizeLinear);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, uint8_t, QuantizeLinear);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, int8_t, QuantizeLinear);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, uint16_t, QuantizeLinear);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, int16_t, QuantizeLinear);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, UInt4x2, QuantizeLinear);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, Int4x2, QuantizeLinear);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, uint8_t, QLinearLeakyRelu);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, int8_t, QLinearLeakyRelu);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, uint8_t, QLinearSigmoid);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, int8_t, QLinearSigmoid);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, QLinearSoftmax);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, uint8_t, QLinearAdd);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, int8_t, QLinearAdd);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, uint8_t, QLinearMul);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, int8_t, QLinearMul);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, QAttention);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, DynamicQuantizeMatMul);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, DynamicQuantizeLSTM);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, uint8_t, MatMulIntegerToFloat);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, uint8_t, QLinearConv);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, int8_t, QLinearConv);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, int8_t, MatMulIntegerToFloat);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, int8_t, NhwcMaxPool);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, uint8_t, NhwcMaxPool);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, QEmbedLayerNormalization);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, int8_t, QGemm);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, uint8_t, QGemm);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, QMoE);
// ******** End: Quantization ******************* //

#ifdef MLAS_F16VEC_INTRINSICS_SUPPORTED
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, MLFloat16, FusedConv);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, MLFloat16, NhwcFusedConv);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSInternalNHWCDomain, 12, MLFloat16, MaxPool);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSInternalNHWCDomain, 11, MLFloat16, AveragePool);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSInternalNHWCDomain, 1, MLFloat16, GlobalAveragePool);
#endif

// This section includes all op kernel declarations for former experimental ops which have now been removed from onnx.
// To maintain backward compatibility these are added as contrib ops.
// Note: the domain for all contrib ops should be MSDomain. However since these ops started out as onnx domain ops
// we cannot change the domain now as this will break backward compatibility.
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kOnnxDomain, 1, Affine);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kOnnxDomain, 1, Crop);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kOnnxDomain, 1, DynamicSlice);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kOnnxDomain, 1, ImageScaler);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kOnnxDomain, 1, 8, MeanVarianceNormalization);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kOnnxDomain, 1, ParametricSoftplus);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kOnnxDomain, 1, ScaledTanh);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kOnnxDomain, 1, 9, ThresholdedRelu);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kOnnxDomain, 1, Scale);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSNchwcDomain, 1, float, ReorderInput);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSNchwcDomain, 1, float, ReorderOutput);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSNchwcDomain, 1, float, Conv);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSNchwcDomain, 1, float, MaxPool);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSNchwcDomain, 1, float, GlobalMaxPool);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSNchwcDomain, 1, float, AveragePool);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSNchwcDomain, 1, float, GlobalAveragePool);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSNchwcDomain, 1, float, Upsample);
// LayerNormalization is now in the ONNX spec. As the contrib op (incorrectly) used kOnnxDomain we need to version it
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kOnnxDomain, 1, 16, float, LayerNormalization);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kOnnxDomain, 1, 16, double, LayerNormalization);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kOnnxDomain, 1, 16, MLFloat16, LayerNormalization);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kOnnxDomain, 1, float, SimplifiedLayerNormalization);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kOnnxDomain, 1, double, SimplifiedLayerNormalization);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kOnnxDomain, 1, MLFloat16, SimplifiedLayerNormalization);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, SkipLayerNormalization);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, double, SkipLayerNormalization);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, MLFloat16, SkipLayerNormalization);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, SkipSimplifiedLayerNormalization);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, double, SkipSimplifiedLayerNormalization);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, MLFloat16, SkipSimplifiedLayerNormalization);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, Inverse);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, Trilu);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, UnfoldTensor);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, DynamicTimeWarping);

#ifdef ENABLE_ATEN
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kPytorchAtenDomain, 1, ATen);
#endif

#ifdef ENABLE_TRAINING_OPS
// Should remove the shrunken_gather include from ENABLE_TRAINING_OPS once 1). compute optimizer is enabled for inference or
// 2). this is needed by inference for other purpose.
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, ShrunkenGather);
#endif

template <>
KernelCreateInfo BuildKernelCreateInfo<void>() {
  KernelCreateInfo info;
  return info;
}

Status RegisterNchwcKernels(KernelRegistry& kernel_registry) {
  static const BuildKernelCreateInfoFn function_table[] = {
      BuildKernelCreateInfo<void>,  // default entry to avoid the list become empty after ops-reducing
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSNchwcDomain, 1, float, ReorderInput)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSNchwcDomain, 1, float, ReorderOutput)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSNchwcDomain, 1, float, Conv)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSNchwcDomain, 1, float, MaxPool)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSNchwcDomain, 1, float, GlobalMaxPool)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSNchwcDomain, 1, float, AveragePool)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSNchwcDomain, 1, float, GlobalAveragePool)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSNchwcDomain, 1, float, Upsample)>,
  };

  for (auto& function_table_entry : function_table) {
    KernelCreateInfo info = function_table_entry();
    if (info.kernel_def != nullptr) {  // filter disabled entries where type is void
      ORT_RETURN_IF_ERROR(kernel_registry.Register(std::move(info)));
    }
  }

  return Status::OK();
}

#ifdef MLAS_F16VEC_INTRINSICS_SUPPORTED
Status RegisterFp16Kernels(KernelRegistry& kernel_registry) {
  static const BuildKernelCreateInfoFn function_table[] = {
      BuildKernelCreateInfo<void>,  // default entry to avoid the list become empty after ops-reducing
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, MLFloat16, FusedConv)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, MLFloat16, NhwcFusedConv)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSInternalNHWCDomain, 12, MLFloat16, MaxPool)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSInternalNHWCDomain, 11, MLFloat16, AveragePool)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSInternalNHWCDomain, 1, MLFloat16, GlobalAveragePool)>,
  };

  for (auto& function_table_entry : function_table) {
    KernelCreateInfo info = function_table_entry();
    if (info.kernel_def != nullptr) {  // filter disabled entries where type is void
      ORT_RETURN_IF_ERROR(kernel_registry.Register(std::move(info)));
    }
  }

  return Status::OK();
}
#endif

Status RegisterQuantizationKernels(KernelRegistry& kernel_registry) {
  static const BuildKernelCreateInfoFn function_table[] = {
      BuildKernelCreateInfo<void>,  // default entry to avoid the list become empty after ops-reducing
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, MatMulInteger16)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, QLinearGlobalAveragePool)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, QLinearConcat)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, QLinearWhere)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, QLinearAveragePool)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, uint8_t,
                                                                  DequantizeLinear)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, int8_t,
                                                                  DequantizeLinear)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, uint16_t,
                                                                  DequantizeLinear)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, int16_t,
                                                                  DequantizeLinear)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, int32_t,
                                                                  DequantizeLinear)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, UInt4x2,
                                                                  DequantizeLinear)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, Int4x2,
                                                                  DequantizeLinear)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, uint8_t,
                                                                  QuantizeLinear)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, int8_t,
                                                                  QuantizeLinear)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, uint16_t,
                                                                  QuantizeLinear)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, int16_t,
                                                                  QuantizeLinear)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, UInt4x2,
                                                                  QuantizeLinear)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, Int4x2,
                                                                  QuantizeLinear)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, uint8_t, QLinearLeakyRelu)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, int8_t, QLinearLeakyRelu)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, uint8_t, QLinearSigmoid)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, int8_t, QLinearSigmoid)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, QLinearSoftmax)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, uint8_t, QLinearAdd)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, int8_t, QLinearAdd)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, uint8_t, QLinearMul)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, int8_t, QLinearMul)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, QAttention)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, DynamicQuantizeMatMul)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, DynamicQuantizeLSTM)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, uint8_t, MatMulIntegerToFloat)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, uint8_t, QLinearConv)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, int8_t, QLinearConv)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, int8_t, MatMulIntegerToFloat)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, int8_t, NhwcMaxPool)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, uint8_t, NhwcMaxPool)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, QEmbedLayerNormalization)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, int8_t, QGemm)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, uint8_t, QGemm)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, QMoE)>,
  };

  for (auto& function_table_entry : function_table) {
    KernelCreateInfo info = function_table_entry();
    if (info.kernel_def != nullptr) {  // filter disabled entries where type is void
      ORT_RETURN_IF_ERROR(kernel_registry.Register(std::move(info)));
    }
  }

  return Status::OK();
}

Status RegisterCpuContribKernels(KernelRegistry& kernel_registry) {
  static const BuildKernelCreateInfoFn function_table[] = {
      BuildKernelCreateInfo<void>,  // default entry to avoid the list become empty after ops-reducing
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, SampleOp)>,

      // add more kernels here
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, GridSample)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, Attention)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, BeamSearch)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, WhisperBeamSearch)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, EmbedLayerNormalization)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, ExpandDims)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, FusedConv)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, FusedGemm)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, GreedySearch)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, MultiHeadAttention)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, GroupQueryAttention)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, MLFloat16, GroupQueryAttention)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, SparseAttention)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, MLFloat16, SparseAttention)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, RotaryEmbedding)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, MLFloat16, RotaryEmbedding)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, Sampling)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, AttnLSTM)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, string, Tokenizer)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, Range)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, WordConvEmbedding)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, GatherND)>,
#if !defined(DISABLE_SPARSE_TENSORS)
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, SparseToDenseMatMul)>,
#endif
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, MurmurHash3)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, TransposeMatMul)>,  // backward compatibility
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, FusedMatMul)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, MatMulNBits)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, MLFloat16, MatMulNBits)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, MatMulBnb4)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TWO_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, uint8_t, int32_t, GatherBlockQuantized)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TWO_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, uint8_t, int64_t, GatherBlockQuantized)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TWO_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, UInt4x2, int32_t, GatherBlockQuantized)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TWO_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, UInt4x2, int64_t, GatherBlockQuantized)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TWO_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, Int4x2, int32_t, GatherBlockQuantized)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TWO_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, Int4x2, int64_t, GatherBlockQuantized)>,
#ifndef ORT_MINIMAL_BUILD
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, MatMulFpQ4)>,
#endif
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, MaxpoolWithMask)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, Pad)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, Unique)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, ConvTransposeWithDynamicPads)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, CropAndResize)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, CDist)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, double, CDist)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, BiasGelu)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, Gelu)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, FastGelu)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, NGramRepeatBlock)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, BifurcationDetector)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, QuickGelu)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, DecoderMaskedMultiHeadAttention)>,
      // These ops were experimental ops in onnx domain which have been removed now. We add them here as
      // contrib ops to main backward compatibility
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kOnnxDomain, 1, Affine)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kOnnxDomain, 1, Crop)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kOnnxDomain, 1, DynamicSlice)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kOnnxDomain, 1, ImageScaler)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kOnnxDomain, 1, 8, MeanVarianceNormalization)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kOnnxDomain, 1, ParametricSoftplus)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kOnnxDomain, 1, ScaledTanh)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kOnnxDomain, 1, 9, ThresholdedRelu)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kOnnxDomain, 1, Scale)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kOnnxDomain, 1, 16, float, LayerNormalization)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kOnnxDomain, 1, 16, double, LayerNormalization)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kOnnxDomain, 1, 16, MLFloat16, LayerNormalization)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kOnnxDomain, 1, float, SimplifiedLayerNormalization)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kOnnxDomain, 1, double, SimplifiedLayerNormalization)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kOnnxDomain, 1, MLFloat16, SimplifiedLayerNormalization)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, SkipLayerNormalization)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, double, SkipLayerNormalization)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, MLFloat16, SkipLayerNormalization)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, float, SkipSimplifiedLayerNormalization)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, double, SkipSimplifiedLayerNormalization)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, MLFloat16, SkipSimplifiedLayerNormalization)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, Inverse)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, Trilu)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, UnfoldTensor)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, DynamicTimeWarping)>,

#ifdef ENABLE_ATEN
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kPytorchAtenDomain, 1, ATen)>,
#endif

#ifdef ENABLE_TRAINING_OPS
      // Should remove the shrunken_gather include from ENABLE_TRAINING_OPS once 1). compute optimizer is enabled for inference or
      // 2). this is needed by inference for other purpose.
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kCpuExecutionProvider, kMSDomain, 1, ShrunkenGather)>,
#endif

  };

  for (auto& function_table_entry : function_table) {
    KernelCreateInfo info = function_table_entry();
    if (info.kernel_def != nullptr) {  // filter disabled entries where type is void
      ORT_RETURN_IF_ERROR(kernel_registry.Register(std::move(info)));
    }
  }

  // Register the NCHWc kernels if supported by the platform.
  if (MlasNchwcGetBlockSize() > 1) {
    ORT_RETURN_IF_ERROR(RegisterNchwcKernels(kernel_registry));
  }

  ORT_RETURN_IF_ERROR(RegisterQuantizationKernels(kernel_registry));

#ifdef MLAS_F16VEC_INTRINSICS_SUPPORTED
  if (MlasFp16AccelerationSupported()) {
    ORT_RETURN_IF_ERROR(RegisterFp16Kernels(kernel_registry));
  }
#endif

  return Status::OK();
}

}  // namespace contrib
}  // namespace onnxruntime
