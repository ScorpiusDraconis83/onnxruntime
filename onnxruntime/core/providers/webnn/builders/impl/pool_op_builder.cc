// Copyright (c) Microsoft Corporation. All rights reserved.
// Copyright (c) Intel Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/providers/common.h"
#include "core/providers/shared/utils/utils.h"
#include "core/providers/webnn/builders/helper.h"
#include "core/providers/webnn/builders/model_builder.h"
#include "core/providers/webnn/builders/op_builder_factory.h"

#include "base_op_builder.h"
#include "builder_utils.h"

namespace onnxruntime {
namespace webnn {

class PoolOpBuilder : public BaseOpBuilder {
  // Add operator related.
 private:
  Status AddToModelBuilderImpl(ModelBuilder& model_builder, const Node& node,
                               const logging::Logger& logger) const override ORT_MUST_USE_RESULT;

  // Operator support related.
 private:
  bool IsOpSupportedImpl(const GraphViewer&, const Node& node,
                         const WebnnDeviceType /* device_type */, const logging::Logger& logger) const override;
};

// Add operator related.

Status PoolOpBuilder::AddToModelBuilderImpl(ModelBuilder& model_builder,
                                            const Node& node,
                                            const logging::Logger& logger) const {
  const auto& op_type = node.OpType();
  const auto& input_defs = node.InputDefs();

  emscripten::val input = model_builder.GetOperand(input_defs[0]->Name());

  bool is_global_pooling = false;
  std::string webnn_op_name;
  if (op_type == "GlobalAveragePool") {
    is_global_pooling = true;
    webnn_op_name = "averagePool2d";
  } else if (op_type == "GlobalMaxPool") {
    is_global_pooling = true;
    webnn_op_name = "maxPool2d";
  } else if (op_type == "GlobalLpPool") {
    is_global_pooling = true;
    webnn_op_name = "l2Pool2d";
  } else if (op_type == "AveragePool") {
    webnn_op_name = "averagePool2d";
  } else if (op_type == "MaxPool") {
    webnn_op_name = "maxPool2d";
  } else if (op_type == "LpPool") {
    webnn_op_name = "l2Pool2d";
  } else {
    return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "PoolOpBuilder, unknown op: ", op_type);
  }

  emscripten::val options = emscripten::val::object();
  options.set("label", node.Name());
  NodeAttrHelper helper(node);
  const bool is_nhwc = model_builder.GetPreferredLayout() == DataLayout::NHWC;

  const auto onnx_kernel_shape = helper.Get("kernel_shape", std::vector<int32_t>{0, 0});
  if (!is_global_pooling) {
    options.set("windowDimensions", emscripten::val::array(onnx_kernel_shape));
  }
  const auto strides = helper.Get("strides", std::vector<int32_t>{1, 1});
  options.set("strides", emscripten::val::array(strides));
  const auto dilations = helper.Get("dilations", std::vector<int32_t>{1, 1});
  options.set("dilations", emscripten::val::array(dilations));
  if (is_nhwc) {
    options.set("layout", emscripten::val("nhwc"));
  } else {
    options.set("layout", emscripten::val("nchw"));
  }

  // Add Padding.
  // Usually using auto padding is more efficient than using explicit padding.
  // Try to see if we can map explicit padding to auto padding.
  const auto onnx_strides = helper.Get("strides", std::vector<int64_t>{1, 1});
  const auto onnx_pads = helper.Get("pads", std::vector<int64_t>{0, 0, 0, 0});
  auto pads = helper.Get("pads", std::vector<uint32_t>{0, 0, 0, 0});
  std::vector<int64_t> input_shape;
  ORT_RETURN_IF_NOT(GetShape(*input_defs[0], input_shape, logger), "Cannot get shape");
  AutoPadType auto_pad_type = StringToAutoPadType(helper.Get("auto_pad", "NOTSET"));
  if (AutoPadType::SAME_UPPER == auto_pad_type || AutoPadType::SAME_LOWER == auto_pad_type) {
    std::vector<int64_t> pads_out;
    ORT_RETURN_IF_ERROR(HandleAutoPad(input_shape, onnx_kernel_shape[0], onnx_kernel_shape[1],
                                      onnx_pads,
                                      helper.Get("strides", std::vector<int64_t>{1, 1}),
                                      helper.Get("dilations", std::vector<int64_t>{1, 1}),
                                      auto_pad_type,
                                      pads_out,
                                      !is_nhwc));
    pads = GetNarrowedIntFromInt64<uint32_t>(pads_out);
  }
  // Permute the ONNX's pads, which is [beginning_height, beginning_width, ending_height, ending_width],
  // while WebNN's padding is [beginning_height, ending_height, beginning_width, ending_width].
  const std::vector<uint32_t> padding{pads[0], pads[2], pads[1], pads[3]};
  options.set("padding", emscripten::val::array(padding));

  const auto ceil_mode = helper.Get("ceil_mode", 0);
  options.set("roundingType", ceil_mode == 0 ? emscripten::val("floor")
                                             : emscripten::val("ceil"));

  // WebNN doesn't support AveragePool with count_include_pad == 1, emulate it by pad + averagePool2d.
  if (op_type == "AveragePool" && helper.Get("count_include_pad", 0) == 1) {
    std::vector<uint32_t> beginning_padding{0, 0, pads[0], pads[1]};
    std::vector<uint32_t> ending_padding{0, 0, pads[2], pads[3]};
    // Unset padding option, because we will use pad op instead.
    options.set("padding", emscripten::val::array(std::vector<uint32_t>{0, 0, 0, 0}));
    if (is_nhwc) {
      beginning_padding = {0, pads[0], pads[1], 0};
      ending_padding = {0, pads[2], pads[3], 0};
    }

    emscripten::val pad_options = emscripten::val::object();
    pad_options.set("label", node.Name() + "_pad");
    input = model_builder.GetBuilder().call<emscripten::val>("pad", input, emscripten::val::array(beginning_padding),
                                                             emscripten::val::array(ending_padding), pad_options);
  }

  emscripten::val output = model_builder.GetBuilder().call<emscripten::val>(webnn_op_name.c_str(), input, options);
  model_builder.AddOperand(node.OutputDefs()[0]->Name(), std::move(output));
  return Status::OK();
}

// Operator support related.
bool PoolOpBuilder::IsOpSupportedImpl(const GraphViewer&,
                                      const Node& node,
                                      const WebnnDeviceType /* device_type */,
                                      const logging::Logger& logger) const {
  const auto& op_type = node.OpType();
  NodeAttrHelper helper(node);
  if (op_type == "AveragePool" || op_type == "LpPool" || op_type == "MaxPool") {
    if (helper.Get("kernel_shape", std::vector<int32_t>{1, 1}).size() != 2) {
      LOGS(logger, VERBOSE) << "Only pooling 2d is supported";
      return false;
    }
  }

  if (op_type == "MaxPool") {
    if (helper.Get("storage_order", 0) == 1) {
      LOGS(logger, VERBOSE) << "MaxPool storage_order == 1 is not supported";
      return false;
    }
    if (node.OutputDefs().size() != 1) {
      LOGS(logger, VERBOSE) << "MaxPool only supports one output";
      return false;
    }
  }

  if (op_type == "GlobalLpPool" || op_type == "LpPool") {
    // WebNN only supports l2Pool2d.
    if (helper.Get("p", 2) != 2) {
      LOGS(logger, VERBOSE) << op_type << " only supports p == 2";
      return false;
    }
  }

  return true;
}

void CreatePoolOpBuilder(const std::string& op_type, OpBuilderRegistrations& op_registrations) {
  if (op_registrations.op_builder_map.count(op_type) > 0)
    return;

  static std::vector<std::string> op_types =
      {
          "AveragePool",
          "GlobalAveragePool",
          "GlobalMaxPool",
          "GlobalLpPool",
          "LpPool",
          "MaxPool",
      };

  op_registrations.builders.push_back(std::make_unique<PoolOpBuilder>());
  for (const auto& type : op_types) {
    op_registrations.op_builder_map.emplace(type, op_registrations.builders.back().get());
  }
}

}  // namespace webnn
}  // namespace onnxruntime
