// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "QnnInterface.h"
#include "nlohmann/json.hpp"

#include "core/providers/qnn/ort_api.h"
#include "core/providers/qnn/builder/qnn_def.h"
#include "core/providers/qnn/builder/qnn_quant_params_wrapper.h"
#include "core/providers/qnn/builder/qnn_utils.h"

namespace onnxruntime {
namespace qnn {

// Stores information about an ONNX input or output tensor.
// Filled out by QnnModelWrapper::GetTensorInfo()
struct TensorInfo {
  std::vector<uint32_t> shape;
  Qnn_DataType_t qnn_data_type;
  QnnQuantParamsWrapper quant_param;
  bool is_initializer;
  const ONNX_NAMESPACE::TensorProto* initializer_tensor;
};

struct ModelSettings {
  bool offload_graph_io_quantization = false;
  bool htp_shared_memory = false;
};

class QnnModelWrapper {
 public:
  QnnModelWrapper(const GraphViewer& graph_viewer,
                  const logging::Logger& logger,
                  const QNN_INTERFACE_VER_TYPE& qnn_interface,
                  const Qnn_BackendHandle_t& backend_handle,
                  const std::unordered_map<std::string, size_t>& input_index_map,
                  const std::unordered_map<std::string, size_t>& output_index_map,
                  QnnBackendType qnn_backend_type,
                  const ModelSettings& model_settings)
      : graph_viewer_(graph_viewer),
        logger_(logger),
        qnn_interface_(qnn_interface),
        backend_handle_(backend_handle),
        input_index_map_(input_index_map),
        output_index_map_(output_index_map),
        qnn_backend_type_(qnn_backend_type),
        model_settings_(model_settings) {
  }
  ORT_DISALLOW_COPY_ASSIGNMENT_AND_MOVE(QnnModelWrapper);

  ~QnnModelWrapper() = default;

  const ModelSettings& GetModelSettings() const { return model_settings_; }

  bool CreateQnnGraph(const Qnn_ContextHandle_t& context,
                      const std::string& graph_name,
                      const QnnGraph_Config_t** graph_configs = nullptr);

  // Make a QnnTensorWrapper from an onnx input or output.
  Status MakeTensorWrapper(const NodeUnitIODef& tensor, QnnTensorWrapper& tensor_wrapper) const;
  Status MakeTensorWrapper(const TensorInfo& tensor_info,
                           const std::string& tensor_name,
                           QnnTensorWrapper& tensor_wrapper) const;

  // Add to internal tensor wrapper table
  bool AddTensorWrapper(QnnTensorWrapper&& tensor_wrapper);

  // Add to internal param wrapper table
  bool AddParamWrapper(QnnParamWrapper&& param_wrapper);

  const QnnTensorWrapper& GetQnnTensorWrapper(const std::string& tensor_name);

  // Utility function to validate a QNN node. Does not modify this object's state.
  Status ValidateQnnNode(const std::string& node_name,
                         const std::string& package_name,
                         const std::string& qnn_op_type,
                         std::vector<Qnn_Tensor_t>&& input_tensors,
                         std::vector<Qnn_Tensor_t>&& output_tensors,
                         std::vector<Qnn_Param_t>&& params) const;

  bool CreateQnnNode(const std::string& name,
                     const std::string& package_name,
                     const std::string& type,
                     std::vector<std::string>&& input_names,
                     std::vector<std::string>&& output_names,
                     std::vector<std::string>&& param_tensor_names,
                     bool do_op_validation = false);

  bool ComposeQnnGraph(bool build_json_qnn_graph = false);

  Qnn_GraphHandle_t GetQnnGraph() const { return graph_; }

  std::string GetQnnGraphName() const { return graph_name_; }

  Qnn_ContextHandle_t GetQnnGraphContext() const { return graph_context_; }

  // Move input tensor wrappers to GraphInfo, QnnModelWrapper end of live
  std::vector<QnnTensorWrapper>&& GetGraphInputTensorWrappers() {
    GetGraphInputOutputTensorWrapper(model_input_names_, model_input_tensor_wrappers_);
    return std::move(model_input_tensor_wrappers_);
  }

  // Move output tensor wrappers to GraphInfo, QnnModelWrapper end of live
  std::vector<QnnTensorWrapper>&& GetGraphOutputTensorWrappers() {
    GetGraphInputOutputTensorWrapper(model_output_names_, model_output_tensor_wrappers_);
    return std::move(model_output_tensor_wrappers_);
  }

  const InitializedTensorSet& GetInitializerTensors() const { return graph_viewer_.GetAllInitializedTensors(); }

  const ONNX_NAMESPACE::TensorProto* GetConstantTensor(const std::string& tensor_name) const {
    return graph_viewer_.GetConstantInitializer(tensor_name);
  }

  bool IsConstantInput(std::string input_name) const {
    return graph_viewer_.IsConstantInitializer(input_name, true);
  }

  static bool GetOnnxShape(const NodeArg& node_arg, std::vector<uint32_t>& shape);

  bool IsQnnTensorWrapperExist(const std::string& tensor_name) const;

  bool IsGraphOutput(const std::string& tensor_name) const {
    return output_index_map_.find(tensor_name) != output_index_map_.end();
  }

  bool IsGraphInput(const std::string& tensor_name) const {
    return input_index_map_.find(tensor_name) != input_index_map_.end();
  }

  const nlohmann::json& GetQnnJSONGraph() {
    return json_qnn_graph_.Finalize();
  }

  Qnn_TensorType_t GetTensorType(const std::string& tensor_name) const {
    if (IsConstantInput(tensor_name)) {
      return QNN_TENSOR_TYPE_STATIC;
    } else if (IsGraphInput(tensor_name)) {
      return QNN_TENSOR_TYPE_APP_WRITE;
    } else if (IsGraphOutput(tensor_name)) {
      return QNN_TENSOR_TYPE_APP_READ;
    } else {
      return QNN_TENSOR_TYPE_NATIVE;
    }
  }

  Status GetTensorInfo(const NodeUnitIODef& input, TensorInfo& input_info) const;

  Status AddReshapeNode(const std::string& input_name,
                        const std::string& output_name,
                        const std::vector<uint32_t>& input_shape,
                        const std::vector<uint32_t>& output_shape,
                        const Qnn_DataType_t& tensor_data_type,
                        const QnnQuantParamsWrapper& input_quantize_param,
                        const QnnQuantParamsWrapper& output_quantize_param,
                        bool do_op_validation,
                        bool is_for_input = true,
                        bool is_for_output = false);

  Status AddReshapeNode(const std::string& input_name,
                        const std::string& output_name,
                        const std::vector<uint32_t>& input_shape,
                        const std::vector<uint32_t>& output_shape,
                        const Qnn_DataType_t& tensor_data_type,
                        const QnnQuantParamsWrapper& quantize_param,
                        bool do_op_validation,
                        bool is_for_input = true,
                        bool is_for_output = false);

  Status AddTransposeNode(NodeIndex node_index,
                          const std::string& input_name,
                          const std::string& output_name,
                          const std::vector<uint32_t>& input_shape,
                          const std::vector<uint32_t>& transpose_perm,
                          const std::vector<uint32_t>& output_shape,
                          const Qnn_DataType_t& tensor_data_type,
                          const QnnQuantParamsWrapper& quantize_param,
                          bool do_op_validation,
                          bool is_for_input = true,
                          bool is_for_output = false);

  // Tranpose NCHW->HWCN for QNN weight
  Status AddNchwToHwcnTranspose(NodeIndex node_index,
                                const std::string& input_name,
                                const std::string& output_name,
                                const std::vector<uint32_t>& input_shape,
                                const std::vector<uint32_t>& output_shape,
                                const Qnn_DataType_t& tensor_data_type,
                                const QnnQuantParamsWrapper& quantize_param,
                                bool do_op_validation,
                                bool is_for_input = true,
                                bool is_for_output = false,
                                bool is_3d = false) {
    LOGS(logger_, VERBOSE) << "Add NCHW->HWCN Transpose node after Conv weight input: " << input_name
                           << " -> " << output_name;
    auto perm = is_3d ? nchw2hwcn_perm_3d : nchw2hwcn_perm;
    std::vector<uint32_t> transpose_perm;
    transpose_perm.resize(perm.size());
    std::transform(perm.begin(), perm.end(),
                   transpose_perm.begin(), [](size_t item) -> uint32_t {
                     return narrow<uint32_t>(item);
                   });
    return AddTransposeNode(node_index, input_name, output_name, input_shape, transpose_perm, output_shape,
                            tensor_data_type, quantize_param, do_op_validation, is_for_input, is_for_output);
  }

  // Tranpose CNHW->HWCN for QNN weight
  Status AddCnhwToHwcnTranspose(NodeIndex node_index,
                                const std::string& input_name,
                                const std::string& output_name,
                                const std::vector<uint32_t>& input_shape,
                                const std::vector<uint32_t>& output_shape,
                                const Qnn_DataType_t& tensor_data_type,
                                const QnnQuantParamsWrapper& quantize_param,
                                bool do_op_validation,
                                bool is_for_input = true,
                                bool is_for_output = false,
                                bool is_3d = false) {
    LOGS(logger_, VERBOSE) << "Add CNHW->HWCN Transpose node after ConvTranspose weight input: " << input_name
                           << " -> " << output_name;
    auto perm = is_3d ? cnhw2hwcn_perm_3d : cnhw2hwcn_perm;
    std::vector<uint32_t> transpose_perm;
    transpose_perm.resize(perm.size());
    std::transform(perm.begin(), perm.end(),
                   transpose_perm.begin(), [](size_t item) -> uint32_t {
                     return narrow<uint32_t>(item);
                   });
    return AddTransposeNode(node_index, input_name, output_name, input_shape, transpose_perm, output_shape,
                            tensor_data_type, quantize_param, do_op_validation, is_for_input, is_for_output);
  }

  Status UnpackInitializerData(const ONNX_NAMESPACE::TensorProto& initializer,
                               std::vector<uint8_t>& unpacked_tensor) const;

  QnnBackendType GetQnnBackendType() const { return qnn_backend_type_; }

  const GraphViewer& GetGraphViewer() const { return graph_viewer_; }

  // Unpack scales from initializer (1 scale for per-tensor, > 1 for per-axis or per-block).
  // Template parameter T allows handling both float and uint8_t scale types.
  template <typename T = float>
  Status UnpackScales(const std::string& initializer_name, std::vector<T>& scales) const {
    const auto& graph_initializers = GetInitializerTensors();
    auto iter = graph_initializers.find(initializer_name);
    ORT_RETURN_IF(iter == graph_initializers.end(), "Unable to find initializer for scale(s): ",
                  initializer_name.c_str());
    gsl::not_null<const onnx::TensorProto*> scale_tensor_proto = iter->second;

    ORT_RETURN_IF_NOT(scale_tensor_proto->has_data_type(), "Expected scale initializer ", initializer_name.c_str(),
                      " to have a proto data type.");

    // Handle float scales
    if constexpr (std::is_same_v<T, float>) {
      // Verify data type for float scales
      ORT_RETURN_IF_NOT(scale_tensor_proto->data_type() == ONNX_NAMESPACE::TensorProto_DataType_FLOAT,
                        "Expected float scale initializer to be of type FLOAT");

      std::vector<uint8_t> initializer_bytes;
      ORT_RETURN_IF_ERROR(UnpackInitializerData(*scale_tensor_proto, initializer_bytes));
      gsl::span<const float> src = gsl::make_span(reinterpret_cast<const float*>(initializer_bytes.data()),
                                                  initializer_bytes.size() / sizeof(float));
      scales.insert(scales.end(), src.begin(), src.end());
    }
    // Handle uint8_t scales (for block quantization)
    else if constexpr (std::is_same_v<T, uint8_t>) {
      // Verify data type for uint8_t scales
      ORT_RETURN_IF_NOT(scale_tensor_proto->data_type() == ONNX_NAMESPACE::TensorProto_DataType_UINT8,
                        "Expected uint8_t scale initializer to be of type UINT8");

      ORT_RETURN_IF_ERROR(UnpackInitializerData(*scale_tensor_proto, scales));
    } else {
      return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Scale ONNX data type `", typeid(T).name(),
                             "` is not supported for unpacking.");
    }
    return Status::OK();
  }

  // Unpack zero-points from initializer and convert to int32_t (1 zero-point for per-tensor, > 1 for per-channel).
  Status UnpackZeroPoints(const std::string& initializer_name,
                          /*out*/ std::vector<int32_t>& zero_points,
                          /*out*/ int32_t& onnx_data_type) const;

  // Checks if a tensor in the ONNX graph is per-channel quantized.
  Status IsPerChannelQuantized(const onnxruntime::NodeUnitIODef& io_def,
                               /*out*/ bool& is_per_channel,
                               /*out*/ int64_t& axis) const;

 private:
  bool CreateQnnInputOutputTensors(const std::string& qnn_node_name,
                                   const std::vector<std::string>& names,
                                   std::vector<Qnn_Tensor_t>& tensor_wrappers,
                                   bool do_op_validation = false);

  bool QnnParamExists(const std::string& param_tensor_name) const;

  bool CreateQnnParamTensors(const std::string& qnn_node_name,
                             const std::vector<std::string>& param_tensor_names,
                             std::vector<Qnn_Param_t>& qnn_params,
                             bool do_op_validation = false);

  bool IsQDQNode(const Node& node) const {
    if (node.OpType() == "QuantizeLinear" || node.OpType() == "DequantizeLinear") {
      return true;
    }
    return false;
  }

  bool IsQnnTensorCreated(const std::string& tensor_name) {
    auto pos = tensor_created_map_.find(tensor_name);
    if (pos == tensor_created_map_.end()) {
      return false;
    }
    return pos->second;
  }

  void GetGraphInputOutputTensorWrapper(const std::vector<std::string>& names,
                                        std::vector<QnnTensorWrapper>& wrappers_list);

  const GraphViewer& graph_viewer_;
  const logging::Logger& logger_;
  const QNN_INTERFACE_VER_TYPE& qnn_interface_;
  const Qnn_BackendHandle_t& backend_handle_;
  Qnn_GraphHandle_t graph_ = nullptr;
  std::string graph_name_ = "";
  // QNN context that holds the QNN graph referenced by `graph_`
  Qnn_ContextHandle_t graph_context_ = nullptr;

  std::vector<std::string> model_input_names_;
  std::vector<std::string> model_output_names_;
  std::vector<QnnTensorWrapper> model_input_tensor_wrappers_;
  std::vector<QnnTensorWrapper> model_output_tensor_wrappers_;
  // All QnnTensorWrapper for the graph
  std::unordered_map<std::string, QnnTensorWrapper> model_tensors_map_;
  // All QnnParamWrapper for the graph
  std::unordered_map<std::string, QnnParamWrapper> model_params_map_;
  std::vector<QnnOpProperty> qnn_op_property_list_;
  // <tensor_name, qnn_tensor_created> -- true means qnn tensor created in qnn graph
  // it includs normal qnn_tensors and qnn_tensors inside param_tensors
  std::unordered_map<std::string, bool> tensor_created_map_;
  const std::unordered_map<std::string, size_t>& input_index_map_;
  const std::unordered_map<std::string, size_t>& output_index_map_;
  QnnBackendType qnn_backend_type_ = QnnBackendType::CPU;
  ModelSettings model_settings_ = {};
  utils::QnnJSONGraph json_qnn_graph_;
};  // QnnModelWrapper

template <typename T>
inline Status AddQnnScalar(QnnModelWrapper& qnn_model_wrapper,
                           const NodeIndex& node_index,
                           const std::string& node_name,
                           const T& scalar,
                           const std::string& qnn_scalar_param_name,
                           std::vector<std::string>& param_names) {
  Qnn_Scalar_t qnn_scalar = QNN_SCALAR_INIT;
  if (std::is_same<T, float>::value) {
    qnn_scalar.dataType = QNN_DATATYPE_FLOAT_32;
    qnn_scalar.floatValue = static_cast<float>(scalar);
  } else if (std::is_same<T, uint32_t>::value) {
    qnn_scalar.dataType = QNN_DATATYPE_UINT_32;
    qnn_scalar.uint32Value = static_cast<uint32_t>(scalar);
  } else if (std::is_same<T, int32_t>::value) {
    qnn_scalar.dataType = QNN_DATATYPE_INT_32;
    qnn_scalar.int32Value = static_cast<int32_t>(scalar);
  } else if (std::is_same<T, int64_t>::value) {
    qnn_scalar.dataType = QNN_DATATYPE_INT_64;
    qnn_scalar.int64Value = static_cast<int64_t>(scalar);
  } else if (std::is_same<T, bool>::value) {
    qnn_scalar.dataType = QNN_DATATYPE_BOOL_8;
    qnn_scalar.bool8Value = static_cast<uint8_t>(scalar);
  } else {
    ORT_RETURN_IF(true, "QNN EP: Unsupported scalar dtype");
  }
  QnnParamWrapper qnn_param_wrapper(node_index, node_name, qnn_scalar_param_name, qnn_scalar);
  param_names.push_back(qnn_param_wrapper.GetParamTensorName());
  qnn_model_wrapper.AddParamWrapper(std::move(qnn_param_wrapper));
  return Status::OK();
}

inline Status AddQnnScalar(QnnModelWrapper& qnn_model_wrapper,
                           const NodeIndex& node_index,
                           const std::string& node_name,
                           const std::string& scalar,
                           const std::string& qnn_scalar_param_name,
                           std::vector<std::string>& param_names) {
  Qnn_Scalar_t qnn_scalar = QNN_SCALAR_INIT;
  qnn_scalar.dataType = QNN_DATATYPE_STRING;
  qnn_scalar.stringValue = scalar.c_str();
  QnnParamWrapper qnn_param_wrapper(node_index, node_name, qnn_scalar_param_name, qnn_scalar);
  param_names.push_back(qnn_param_wrapper.GetParamTensorName());
  qnn_model_wrapper.AddParamWrapper(std::move(qnn_param_wrapper));
  return Status::OK();
}

}  // namespace qnn
}  // namespace onnxruntime
