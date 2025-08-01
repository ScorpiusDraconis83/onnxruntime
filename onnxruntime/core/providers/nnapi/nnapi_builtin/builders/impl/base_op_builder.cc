// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/graph/graph_viewer.h"
#include "core/providers/nnapi/nnapi_builtin/builders/impl/base_op_builder.h"

namespace onnxruntime {
namespace nnapi {

// Add operator related

Status BaseOpBuilder::AddToModelBuilder(ModelBuilder& model_builder, const NodeUnit& node_unit) const {
  OpSupportCheckParams params{
      model_builder.GetEffectiveFeatureLevel(),
      model_builder.UseNCHW(),
  };

  // We checked supported in IExecutionProvider::GetCapability.
  // Checking again in AddToModelBuilder which is called in IExecutionProvider::Compile is redundant.
  // ORT_RETURN_IF_NOT(IsOpSupported(model_builder.GetGraphViewer(), node_unit, params),
  //                  "Unsupported operator ", node_unit.OpType());

#ifndef NDEBUG
  model_builder.SetDebugCurrentOnnxNodeIndex(node_unit.Index());
#endif
  ORT_RETURN_IF_ERROR(AddToModelBuilderImpl(model_builder, node_unit));
  LOGS_DEFAULT(VERBOSE) << "Operator name: [" << node_unit.Name()
                        << "] type: [" << node_unit.OpType() << "] was added";
  return Status::OK();
}

// Operator support related

bool BaseOpBuilder::IsOpSupported(const GraphViewer& graph_viewer, const NodeUnit& node_unit,
                                  const OpSupportCheckParams& params) const {
  int32_t required_feature_level = GetMinSupportedNNAPIFeatureLevel(node_unit, params);
  if (required_feature_level > params.android_feature_level) {
    LOGS_DEFAULT(VERBOSE) << "Current Android API level [" << params.android_feature_level
                          << "], Operator [" << node_unit.OpType()
                          << "] is only supported on API >" << required_feature_level;
    return false;
  }

  if (!IsNodeUnitTypeSupported(node_unit))
    return false;

  if (!HasSupportedInputOutputs(graph_viewer, node_unit, params))
    return false;

  if (!HasSupportedOpSet(node_unit))
    return false;

  return IsOpSupportedImpl(graph_viewer, node_unit, params);
}

bool BaseOpBuilder::HasSupportedInputOutputs(const GraphViewer& graph_viewer, const NodeUnit& node_unit,
                                             const OpSupportCheckParams& params) const {
  // We do not support unknown(null) input shape
  auto has_supported_shape = [](const NodeArg& node_arg, const std::string& name, const std::string& op_type) {
    const auto* shape_proto = node_arg.Shape();
    if (!shape_proto) {
      LOGS_DEFAULT(VERBOSE) << "Node [" << name << "] type [" << op_type
                            << "] Input [" << node_arg.Name() << "] has no shape";
      return false;
    }

    // We do not support dynamic shape input for now
    for (const auto& dim : shape_proto->dim()) {
      if (!dim.has_dim_value()) {
        LOGS_DEFAULT(VERBOSE) << "Dynamic shape is not supported for now, for input:" << node_arg.Name();
        return false;
      }
    }
    return true;
  };

  for (const auto& input : node_unit.Inputs()) {
    if (!input.node_arg.Exists()) {
      continue;
    }
    if (!has_supported_shape(input.node_arg, node_unit.Name(), node_unit.OpType()))
      return false;

    if (input.quant_param.has_value()) {
      if (!has_supported_shape(input.quant_param->scale, node_unit.Name(), node_unit.OpType()))
        return false;

      // zero point is optional
      if (input.quant_param->zero_point &&
          !has_supported_shape(*input.quant_param->zero_point, node_unit.Name(), node_unit.OpType()))
        return false;
    }
  }

  return HasSupportedInputOutputsImpl(graph_viewer, node_unit, params);
}

bool BaseOpBuilder::HasSupportedInputOutputsImpl(const GraphViewer& /* graph_viewer */, const NodeUnit& node_unit,
                                                 const OpSupportCheckParams& /* params */) const {
  // We only check the type of input 0 by default
  // specific op builder can override this
  const auto& input = node_unit.Inputs()[0].node_arg;

  int32_t input_type;
  if (!GetType(input, input_type))
    return false;

  if (input_type != ONNX_NAMESPACE::TensorProto_DataType_FLOAT) {
    LOGS_DEFAULT(VERBOSE) << "[" << node_unit.OpType()
                          << "] Input type: [" << input_type
                          << "] is not supported for now";
    return false;
  }

  return true;
}

bool BaseOpBuilder::HasSupportedOpSet(const NodeUnit& node_unit) const {
  auto since_version = node_unit.SinceVersion();
  if (since_version < GetMinSupportedOpSet(node_unit) || since_version > GetMaxSupportedOpSet(node_unit)) {
    LOGS_DEFAULT(VERBOSE) << node_unit.OpType() << " opset [" << since_version
                          << "] is only supported for opset ["
                          << GetMinSupportedOpSet(node_unit) << ", "
                          << GetMaxSupportedOpSet(node_unit) << "]";
    return false;
  }

  return true;
}

bool BaseOpBuilder::IsNodeUnitTypeSupported(const NodeUnit& node_unit) const {
  if (node_unit.UnitType() == NodeUnit::Type::QDQGroup) {
    LOGS_DEFAULT(VERBOSE) << "QDQ NodeUnit [" << node_unit.OpType()
                          << "] is not supported for now";

    return false;
  }

  return true;
}

}  // namespace nnapi
}  // namespace onnxruntime
