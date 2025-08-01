// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include <string>
#include <vector>

#include "core/providers/qnn/ort_api.h"
#include "core/providers/qnn/builder/opbuilder/base_op_builder.h"
#include "core/providers/qnn/builder/qnn_model_wrapper.h"
#include "core/providers/qnn/builder/op_builder_factory.h"
#include "core/providers/qnn/builder/qnn_utils.h"

namespace onnxruntime {
namespace qnn {

class TransposeOpBuilder : public BaseOpBuilder {
 public:
  TransposeOpBuilder() : BaseOpBuilder("TransposeOpBuilder") {}
  ORT_DISALLOW_COPY_ASSIGNMENT_AND_MOVE(TransposeOpBuilder);

 protected:
  Status ProcessAttributesAndOutputs(QnnModelWrapper& qnn_model_wrapper,
                                     const NodeUnit& node_unit,
                                     std::vector<std::string>&& input_names,
                                     const logging::Logger& logger,
                                     bool do_op_validation) const override ORT_MUST_USE_RESULT;

 private:
  Status ProcessPermAttribute(QnnModelWrapper& qnn_model_wrapper,
                              const NodeUnit& node_unit,
                              std::vector<std::string>& param_tensor_names) const;
};

Status TransposeOpBuilder::ProcessPermAttribute(QnnModelWrapper& qnn_model_wrapper,
                                                const NodeUnit& node_unit,
                                                std::vector<std::string>& param_tensor_names) const {
  auto inputs = node_unit.Inputs();
  std::vector<uint32_t> input_shape;
  ORT_RETURN_IF_NOT(qnn_model_wrapper.GetOnnxShape(inputs[0].node_arg, input_shape), "Cannot get shape");
  // set default perm
  uint32_t rank = static_cast<uint32_t>(input_shape.size());
  std::vector<int64_t> transpose_perm(rank);
  for (uint32_t i = 0; i < rank; ++i) {
    transpose_perm[i] = rank - 1 - i;
  }

  NodeAttrHelper node_helper(node_unit);
  transpose_perm = node_helper.Get("perm", transpose_perm);
  auto perm_size = static_cast<uint32_t>(transpose_perm.size());
  std::vector<uint32_t> perm_shape{perm_size};
  std::vector<uint32_t> perm_data;
  perm_data.resize(perm_size);
  std::transform(transpose_perm.begin(), transpose_perm.end(), perm_data.begin(),
                 [](int64_t item) { return SafeInt<uint32_t>(item); });

  QnnParamWrapper transpose_param(node_unit.Index(), node_unit.Name(), QNN_OP_TRANSPOSE_PARAM_PERM,
                                  std::move(perm_shape), std::move(perm_data));
  param_tensor_names.push_back(transpose_param.GetParamTensorName());
  qnn_model_wrapper.AddParamWrapper(std::move(transpose_param));

  return Status::OK();
}

Status TransposeOpBuilder::ProcessAttributesAndOutputs(QnnModelWrapper& qnn_model_wrapper,
                                                       const NodeUnit& node_unit,
                                                       std::vector<std::string>&& input_names,
                                                       const logging::Logger& logger,
                                                       bool do_op_validation) const {
  ORT_UNUSED_PARAMETER(logger);

  if (input_names.size() < 1) {
    return Status::OK();
  }

  std::vector<std::string> param_tensor_names;
  ORT_RETURN_IF_ERROR(ProcessPermAttribute(qnn_model_wrapper, node_unit, param_tensor_names));

  const auto& output_name = node_unit.Outputs()[0].node_arg.Name();
  std::vector<std::string> output_names;

  bool is_graph_output = qnn_model_wrapper.IsGraphOutput(output_name);
  Qnn_TensorType_t tensor_type = is_graph_output ? QNN_TENSOR_TYPE_APP_READ : QNN_TENSOR_TYPE_NATIVE;

  struct CastNodeInfo {
    std::string node_name;
    std::string input_name;
    std::string output_name;
  };
  std::vector<CastNodeInfo> cast_node_info_vec;

  // Check if we need to add a cast node for int64
  bool needs_int64_cast = false;
  if (is_graph_output) {
    for (const auto& input_name : input_names) {
      if (input_name.find("_cast_int32") != std::string::npos) {
        needs_int64_cast = true;
        break;
      }
    }
  }

  const auto& transpose_output = node_unit.Outputs()[0];
  // Get the output info for the gather output tensor
  TensorInfo output_info = {};
  ORT_RETURN_IF_ERROR(qnn_model_wrapper.GetTensorInfo(transpose_output, output_info));
  std::vector<uint32_t> output_shape;
  ORT_RETURN_IF_NOT(qnn_model_wrapper.GetOnnxShape(node_unit.Outputs()[0].node_arg, output_shape),
                    "Cannot get shape");

  const QnnTensorWrapper& input_tensor_wrapper = qnn_model_wrapper.GetQnnTensorWrapper(input_names[0]);

  // If a cast to int64 is needed, add the cast node
  if (needs_int64_cast) {
    std::string cast_node_name = output_name + "_cast_int64";
    std::string cast_input_name = output_name + "_cast_int64_aux";
    std::string cast_output_name = output_name;

    // Create the cast input tensor wrapper
    QnnTensorWrapper cast_input_tensorwrapper(cast_input_name,
                                              QNN_TENSOR_TYPE_NATIVE,
                                              output_info.qnn_data_type,
                                              output_info.quant_param.Copy(),
                                              std::move(output_shape));

    ORT_RETURN_IF_NOT(qnn_model_wrapper.AddTensorWrapper(std::move(cast_input_tensorwrapper)), "Failed to add tensor.");
    cast_node_info_vec.push_back({cast_node_name, cast_input_name, cast_output_name});
  }

  // Transpose output uses same data type and quantization parameter with input
  // 1. In QDQ model, the optimization may create scenario like Q -> Transpose -> DQ, Transpose is single node
  // Input tensor is created by previous node which is quantized tensor,
  // so output just copy the same data type and quantization parameters
  // 2. In QDQ model, Transpose also support non-quantized data like int32.
  QnnTensorWrapper output_tensorwrapper(output_name,
                                        tensor_type,
                                        input_tensor_wrapper.GetTensorDataType(),
                                        input_tensor_wrapper.GetQnnQuantParams().Copy(),
                                        std::move(output_shape));

  ORT_RETURN_IF_NOT(qnn_model_wrapper.AddTensorWrapper(std::move(output_tensorwrapper)), "Failed to add tensor.");

  output_names.push_back(output_name);
  ORT_RETURN_IF_NOT(qnn_model_wrapper.CreateQnnNode(utils::GetNodeName(node_unit),
                                                    QNN_OP_PACKAGE_NAME_QTI_AISW,
                                                    QNN_OP_TRANSPOSE,
                                                    std::move(input_names),
                                                    std::move(output_names),
                                                    std::move(param_tensor_names),
                                                    do_op_validation),
                    "Failed to add node.");

  if (needs_int64_cast) {
    for (const auto& cast_node_info : cast_node_info_vec) {
      // Insert cast node.
      ORT_RETURN_IF_NOT(qnn_model_wrapper.CreateQnnNode(cast_node_info.node_name,
                                                        QNN_OP_PACKAGE_NAME_QTI_AISW,
                                                        QNN_OP_CAST,
                                                        {cast_node_info.input_name},
                                                        {cast_node_info.output_name},
                                                        {}),
                        " Failed to add Cast node");
    }
  }
  return Status::OK();
}

void CreateTransposeOpBuilder(const std::string& op_type, OpBuilderRegistrations& op_registrations) {
  op_registrations.AddOpBuilder(op_type, std::make_unique<TransposeOpBuilder>());
}

}  // namespace qnn
}  // namespace onnxruntime
