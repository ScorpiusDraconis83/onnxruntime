// Copyright (c) Microsoft Corporation. All rights reserved.
// Copyright (c) Intel Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/framework/tensorprotoutils.h"
#include "core/optimizer/initializer.h"
#include "core/providers/common.h"
#include "core/providers/cpu/tensor/reshape_helper.h"
#include "core/providers/shared/utils/utils.h"
#include "core/providers/webnn/builders/helper.h"
#include "core/providers/webnn/builders/model_builder.h"
#include "core/providers/webnn/builders/op_builder_factory.h"

#include "base_op_builder.h"

namespace onnxruntime {
namespace webnn {

class ExpandOpBuilder : public BaseOpBuilder {
  // Add operator related.
 public:
  void AddInitializersToSkip(ModelBuilder& model_builder, const Node& node) const override;

 private:
  Status AddToModelBuilderImpl(ModelBuilder& model_builder, const Node& node,
                               const logging::Logger& logger) const override ORT_MUST_USE_RESULT;

  // Operator support related.
 private:
  bool IsOpSupportedImpl(const GraphViewer& graph_viewer, const Node& node,
                         const WebnnDeviceType /* device_type */, const logging::Logger& logger) const override;
};

// Add operator related.

void ExpandOpBuilder::AddInitializersToSkip(ModelBuilder& model_builder, const Node& node) const {
  model_builder.AddInitializerToSkip(node.InputDefs()[1]->Name());
}

Status ExpandOpBuilder::AddToModelBuilderImpl(ModelBuilder& model_builder,
                                              const Node& node,
                                              const logging::Logger& logger) const {
  const auto& input_defs = node.InputDefs();
  const auto& initializers(model_builder.GetInitializerTensors());
  const auto& shape_tensor = *initializers.at(input_defs[1]->Name());
  std::vector<int64_t> new_shape;
  ORT_RETURN_IF_NOT(ReadIntArrayFrom1DTensor(shape_tensor, new_shape, model_builder.GetGraphViewer(), logger),
                    "Cannot get shape.");
  emscripten::val input = model_builder.GetOperand(input_defs[0]->Name());
  std::vector<int64_t> input_shape;
  ORT_RETURN_IF_NOT(GetShape(*input_defs[0], input_shape, logger), "Cannot get input's shape.");

  std::vector<int64_t> output_shape;
  ORT_RETURN_IF_NOT(GetBidirectionalBroadcastShape(input_shape, new_shape, output_shape), "Cannot get output shape.");

  emscripten::val options = emscripten::val::object();
  options.set("label", node.Name());

  emscripten::val output_shape_arr = emscripten::val::array(GetNarrowedIntFromInt64<uint32_t>(output_shape));
  emscripten::val output = model_builder.GetBuilder().call<emscripten::val>("expand", input, output_shape_arr, options);
  model_builder.AddOperand(node.OutputDefs()[0]->Name(), std::move(output));
  return Status::OK();
}

// Operator support related.

bool ExpandOpBuilder::IsOpSupportedImpl(const GraphViewer& graph_viewer,
                                        const Node& node,
                                        const WebnnDeviceType /* device_type */,
                                        const logging::Logger& logger) const {
  const auto& input_defs = node.InputDefs();
  const auto& shape_name = input_defs[1]->Name();

  // We need a constant which can not be overriden by input
  const auto* shape_init = graph_viewer.GetConstantInitializer(shape_name);
  if (!shape_init) {
    LOGS(logger, VERBOSE) << "The shape must be a constant initializer.";
    return false;
  }

  const auto& shape_tensor = *shape_init;
  if (shape_tensor.data_type() != ONNXTensorElementDataType::ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64) {
    LOGS(logger, VERBOSE) << "The type of tensor's element data must be INT64.";
    return false;
  }

  std::vector<int64_t> new_shape;
  if (!ReadIntArrayFrom1DTensor(shape_tensor, new_shape, graph_viewer, logger)) {
    return false;
  }
  if (std::any_of(new_shape.begin(), new_shape.end(), [](int64_t dimension) { return dimension == 0; })) {
    LOGS(logger, VERBOSE) << "WebNN expand does not support new shape with 0 dimension.";
    return false;
  }

  std::vector<int64_t> input_shape;
  if (!GetShape(*input_defs[0], input_shape, logger)) {
    LOGS(logger, VERBOSE) << "Cannot get input's shape.";
    return false;
  }

  std::vector<int64_t> output_shape;
  if (!GetBidirectionalBroadcastShape(input_shape, new_shape, output_shape)) {
    LOGS(logger, VERBOSE) << "The input cannot expand to shape " << GetShapeString(new_shape);
    return false;
  }

  return true;
}

void CreateExpandOpBuilder(const std::string& op_type, OpBuilderRegistrations& op_registrations) {
  op_registrations.builders.push_back(std::make_unique<ExpandOpBuilder>());
  op_registrations.op_builder_map.emplace(op_type, op_registrations.builders.back().get());
}

}  // namespace webnn
}  // namespace onnxruntime
