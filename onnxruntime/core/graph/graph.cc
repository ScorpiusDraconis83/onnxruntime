// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/graph/graph.h"

#include <cassert>
#include <fstream>
#include <iostream>
#include <numeric>
#include <queue>
#include <stack>

#include <gsl/gsl>

#include "core/common/common.h"
#include "core/common/inlined_containers.h"
#include "core/common/logging/logging.h"
#include "core/common/narrow.h"
#include "core/flatbuffers/flatbuffers_utils.h"
#include "core/framework/tensor_type_and_shape.h"
#include "core/flatbuffers/schema/ort.fbs.h"
#include "core/framework/tensor_external_data_info.h"
#include "core/framework/tensor_shape.h"
#include "core/framework/tensor_type_and_shape.h"
#include "core/framework/tensorprotoutils.h"
#include "core/framework/utils.h"
#include "core/graph/function_utils.h"
#include "core/graph/graph_flatbuffers_utils.h"
#include "core/graph/graph_viewer.h"
#include "core/graph/indexed_sub_graph.h"
#include "core/graph/model.h"
#include "core/graph/graph_utils.h"
#include "core/graph/model_editor_api_types.h"
#include "core/graph/model_load_utils.h"
#include "core/graph/model_saving_options.h"
#include "core/graph/node_attr_utils.h"
#include "core/graph/op.h"
#include "core/graph/runtime_optimization_record_container.h"

#if !defined(ORT_MINIMAL_BUILD)
#include "core/graph/function.h"
#include "core/graph/function_impl.h"
#include "core/graph/schema_registry.h"
#include "onnx/checker.h"
#include "onnx/defs/parser.h"
using namespace ONNX_NAMESPACE::checker;
#endif

using namespace ONNX_NAMESPACE;
using namespace ONNX_NAMESPACE::Utils;
using namespace ::onnxruntime::common;

namespace onnxruntime {

#if !defined(ORT_MINIMAL_BUILD)
#define NO_CHANGE_ON_SYNC_FLAG(...)                  \
  do {                                               \
    const bool sync_needed = GraphProtoSyncNeeded(); \
    {                                                \
      __VA_ARGS__;                                   \
    }                                                \
    GraphProtoSyncNeeded(sync_needed);               \
  } while (0)

static Status MergeShapeInfo(const std::string& output_name,
                             const TypeProto& source, TypeProto& target,
                             bool strict, const logging::Logger& logger) {
  if (!(utils::HasTensorType(source) && utils::HasTensorType(target))
#if !defined(DISABLE_OPTIONAL_TYPE)
      && !(utils::HasOptionalTensorType(source) && utils::HasOptionalTensorType(target))
#endif

#if !defined(DISABLE_SPARSE_TENSORS)
      && !(utils::HasSparseTensorType(source) && utils::HasSparseTensorType(target))
#endif
  ) {
    std::ostringstream ss;
    ss << "Source and target must both be tensors";

#if !defined(DISABLE_OPTIONAL_TYPE)
    ss << " , or optional typed entities";
#endif

#if !defined(DISABLE_SPARSE_TENSORS)
    ss << " , or sparse tensors";
#endif

    return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, ss.str());
  }

  auto status = Status::OK();

  ORT_TRY {
    if (utils::HasTensorType(source)) {
      ONNX_NAMESPACE::mergeInShapeInfo(source.tensor_type(), *target.mutable_tensor_type());
    }
#if !defined(DISABLE_OPTIONAL_TYPE)
    else if (utils::HasOptionalTensorType(source)) {
      ONNX_NAMESPACE::mergeInShapeInfo(utils::GetOptionalTypeProto(source).tensor_type(),
                                       *utils::GetMutableOptionalTypeProto(target)->mutable_tensor_type());
    }
#endif

#if !defined(DISABLE_SPARSE_TENSORS)
    else {
      ONNX_NAMESPACE::mergeInShapeInfo(source.sparse_tensor_type(), *target.mutable_sparse_tensor_type());
    }
#endif
  }
  ORT_CATCH(const ONNX_NAMESPACE::InferenceError& ex) {
    // if this model was not created with the latest onnx version, allow the shape inferencing failure
    // (strict == false).
    // we do this to have strict testing of the latest inferencing to detect bugs, but lenient shape inferencing for
    // older models in case later changes to the ONNX shape inferencing or ORT break them.
    if (!strict) {
      // mergeInShapeInfo does nothing unless source.shape() is not null, and there would be no conflict if
      // target.shape() was empty. 'assert' just in case that ever changes.
      assert(utils::HasShape(source) && utils::HasShape(target));
      LOGS(logger, WARNING) << "Error merging shape info for output. '" << output_name
                            << "' source:" << utils::GetTensorShapeFromTensorShapeProto(utils::GetShape(source))
                            << " target:" << utils::GetTensorShapeFromTensorShapeProto(utils::GetShape(target))
                            << ". Falling back to lenient merge.";
      if (utils::HasTensorType(source)) {
        ONNX_NAMESPACE::UnionShapeInfo(utils::GetShape(source), *target.mutable_tensor_type());
      }
#if !defined(DISABLE_OPTIONAL_TYPE)
      else if (utils::HasOptionalTensorType(source)) {
        ONNX_NAMESPACE::UnionShapeInfo(utils::GetShape(source),
                                       *utils::GetMutableOptionalTypeProto(target)->mutable_tensor_type());
      }
#endif

#if !defined(DISABLE_SPARSE_TENSORS)
      else {
        ONNX_NAMESPACE::UnionShapeInfo(utils::GetShape(source), *target.mutable_sparse_tensor_type());
      }
#endif
    } else {
      ORT_UNUSED_PARAMETER(logger);
      ORT_UNUSED_PARAMETER(strict);
      ORT_UNUSED_PARAMETER(output_name);
      ORT_HANDLE_EXCEPTION([&]() {
        status = ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Output:", output_name, " ", ex.what());
      });
    }
  }

  return status;
}

static bool GraphLoadedFromModelFile(const GraphProto* graph_proto) {
  return graph_proto && (graph_proto->node_size() != 0 ||
                         graph_proto->output_size() != 0);
}

// there are some known invalid usages of dim_param and dim_value. remove them from the TypeProto so that
// they don't affect shape inferencing or the allocation planner
static void RemoveInvalidValues(ONNX_NAMESPACE::TypeProto& type) {
  if (utils::HasTensorType(type) && utils::HasShape(type.tensor_type())) {
    auto* shape = type.mutable_tensor_type()->mutable_shape();
    for (int i = 0, end = shape->dim_size(); i < end; ++i) {
      auto& dim = *shape->mutable_dim(i);
      if (utils::HasDimParam(dim)) {
        if (dim.dim_param().empty()) {
          dim.clear_dim_param();
        }
      } else if (utils::HasDimValue(dim)) {
        if (dim.dim_value() < 0) {
          dim.clear_dim_value();
        }
      }
    }
  }
}

static std::string GenerateSchemaKey(const IndexedSubGraph& subgraph_ptr) {
  return MakeString(subgraph_ptr.GetMetaDef()->domain, "_",
                    subgraph_ptr.GetMetaDef()->name, "_",
                    subgraph_ptr.GetMetaDef()->since_version);
}
#endif  // !defined(ORT_MINIMAL_BUILD)

#if !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD) || defined(ORT_MINIMAL_BUILD_CUSTOM_OPS)
NodeArg::NodeArg(const std::string& name, const TypeProto* p_node_arg_type) {
  node_arg_info_.set_name(name);
  // If the name is empty, it means the arg does not exist.
  exists_ = !(name.empty());
  if (nullptr != p_node_arg_type) {
    (*node_arg_info_.mutable_type()) = *p_node_arg_type;
#if !defined(ORT_MINIMAL_BUILD)
    // should not be possible to have invalid values in the ORT format model, so we don't need this
    // in a minimal build
    RemoveInvalidValues(*node_arg_info_.mutable_type());
#endif
    type_ = DataTypeUtils::ToType(node_arg_info_.type());
  } else {
    type_ = nullptr;
  }
}
#endif  // #if !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD) || defined(ORT_MINIMAL_BUILD_CUSTOM_OPS)

NodeArg::NodeArg(NodeArgInfo&& node_arg_info) {
  node_arg_info_ = std::move(node_arg_info);

  exists_ = !node_arg_info_.name().empty();
  if (node_arg_info_.has_type())
    type_ = DataTypeUtils::ToType(node_arg_info_.type());
  else {
    type_ = nullptr;
  }
}

const std::string& NodeArg::Name() const noexcept {
  return node_arg_info_.name();
}

DataType NodeArg::Type() const noexcept {
  return type_;
}

const TypeProto* NodeArg::TypeAsProto() const noexcept {
  if (utils::HasType(node_arg_info_))
    return &node_arg_info_.type();

  return nullptr;
}

const TensorShapeProto* NodeArg::Shape() const {
  const TypeProto* type = TypeAsProto();
  if (type == nullptr) return nullptr;
  const auto typeCase = type->value_case();
  switch (typeCase) {
    case TypeProto::kTensorType: {
      if (utils::HasShape(type->tensor_type())) {
        return &(type->tensor_type().shape());
      }
      return nullptr;
    }
#if !defined(DISABLE_SPARSE_TENSORS)
    case TypeProto::kSparseTensorType: {
      if (utils::HasShape(type->sparse_tensor_type())) {
        return &(type->sparse_tensor_type().shape());
      }
      return nullptr;
    }
#endif

#if !defined(DISABLE_OPTIONAL_TYPE)
    case TypeProto::kOptionalType: {
      // Shape is applicable only for optional tensor type
      if (utils::HasOptionalTensorType(*type) &&
          utils::HasShape(utils::GetOptionalTypeProto(*type).tensor_type())) {
        return &(utils::GetOptionalTypeProto(*type).tensor_type().shape());
      }
      return nullptr;
    }
#endif

    case TypeProto::kSequenceType:
    case TypeProto::kMapType:
    case TypeProto::kOpaqueType:
    case TypeProto::VALUE_NOT_SET:
    default:
      return nullptr;
  }
}

bool NodeArg::HasTensorOrScalarShape() const {
  const TypeProto* type = TypeAsProto();
  if (!type) return false;
  const auto type_case = type->value_case();
  switch (type_case) {
    case TypeProto::kTensorType:
#if !defined(DISABLE_SPARSE_TENSORS)
    case TypeProto::kSparseTensorType:
#endif
      // Standard tensor has a valid shape field while
      // scalar's shape is empty. Thus, we don't need to
      // check shape here.
      return true;
    case TypeProto::kSequenceType:
    case TypeProto::kOptionalType:
    case TypeProto::kMapType:
    case TypeProto::kOpaqueType:
    case TypeProto::VALUE_NOT_SET:
    default:
      return false;
  }
}

#if !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD)
void NodeArg::SetShape(const TensorShapeProto& shape) {
  const auto type_case = node_arg_info_.type().value_case();
  switch (type_case) {
    case TypeProto::kTensorType:
      *(node_arg_info_.mutable_type()->mutable_tensor_type()->mutable_shape()) = shape;
      break;
#if !defined(DISABLE_SPARSE_TENSORS)
    case TypeProto::kSparseTensorType:
      *(node_arg_info_.mutable_type()->mutable_sparse_tensor_type()->mutable_shape()) = shape;
      break;
#endif

#if !defined(DISABLE_OPTIONAL_TYPE)
    case TypeProto::kOptionalType:
      // Set shape only for optional tensors
      if (utils::HasOptionalTensorType(node_arg_info_.type())) {
        *(utils::GetMutableOptionalTypeProto(*(node_arg_info_.mutable_type()))
              ->mutable_tensor_type()
              ->mutable_shape()) = shape;
      }
      break;
#endif
    case TypeProto::kSequenceType:
    case TypeProto::kMapType:
    case TypeProto::kOpaqueType:
    case TypeProto::VALUE_NOT_SET:
    default:
      return;
  }
}

void NodeArg::ClearShape() {
  const auto type_case = node_arg_info_.type().value_case();
  switch (type_case) {
    case TypeProto::kTensorType:
      node_arg_info_.mutable_type()->mutable_tensor_type()->clear_shape();
      break;
#if !defined(DISABLE_SPARSE_TENSORS)
    case TypeProto::kSparseTensorType:
      node_arg_info_.mutable_type()->mutable_sparse_tensor_type()->clear_shape();
      break;
#endif

#if !defined(DISABLE_OPTIONAL_TYPE)
    case TypeProto::kOptionalType:
      // Clear shape only for optional tensors
      if (utils::HasOptionalTensorType(node_arg_info_.type())) {
        utils::GetMutableOptionalTypeProto(*(node_arg_info_.mutable_type()))
            ->mutable_tensor_type()
            ->clear_shape();
      }
      break;
#endif

    case TypeProto::kSequenceType:
    case TypeProto::kMapType:
    case TypeProto::kOpaqueType:
    case TypeProto::VALUE_NOT_SET:
    default:
      return;
  }
}

#endif  // !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD)

#if !defined(ORT_MINIMAL_BUILD)

common::Status NodeArg::OverrideTypesHelper(const ONNX_NAMESPACE::TypeProto& input_type,
                                            int32_t input_tensor_elem_type,
                                            int32_t current_tensor_elem_type,
                                            bool override_types) {
  if (input_tensor_elem_type != current_tensor_elem_type) {
    if (override_types) {
      DataType inferred_type = DataTypeUtils::ToType(input_type);
      // The "SetType" call will override the shape information to empty.
      // If the original tensor has shape information, need to set it back.
      if (Shape()) {
        auto old_shape = *Shape();
        SetType(inferred_type);
        SetShape(old_shape);
      } else {
        SetType(inferred_type);
      }
    } else {
      return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Tensor element type mismatch. ",
                             static_cast<TensorProto_DataType>(input_tensor_elem_type), " != ",
                             static_cast<TensorProto_DataType>(current_tensor_elem_type));
    }
  }

  return Status::OK();
}

common::Status NodeArg::UpdateTypeAndShape(const ONNX_NAMESPACE::TypeProto& input_type, bool strict,
                                           bool override_types, const logging::Logger& logger) {
  if (!utils::HasType(node_arg_info_)) {
    SetType(input_type);
    return Status::OK();
  }

  auto& current_type = *node_arg_info_.mutable_type();
  const auto current_type_case = current_type.value_case();
  const auto input_type_case = input_type.value_case();

  if (current_type_case != input_type_case)
    return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Type mismatch. Current=",
                           current_type_case, " Input=", input_type_case);

  switch (input_type_case) {
    case TypeProto::kTensorType: {
      const auto& input_tensor_type = input_type.tensor_type();
      const auto& input_tensor_elem_type = input_tensor_type.elem_type();
      const auto& current_tensor_elem_type = current_type.tensor_type().elem_type();

      ORT_RETURN_IF_ERROR(OverrideTypesHelper(input_type, input_tensor_elem_type, current_tensor_elem_type,
                                              override_types));

      if (utils::HasShape(input_tensor_type)) {
        if (utils::HasShape(current_type)) {
          ORT_RETURN_IF_ERROR(MergeShapeInfo(Name(), input_type, current_type, strict, logger));
        } else {
          *current_type.mutable_tensor_type() = input_tensor_type;
        }
      }

      break;
    }

#if !defined(DISABLE_SPARSE_TENSORS)
    case TypeProto::kSparseTensorType: {
      const auto& input_tensor_type = input_type.sparse_tensor_type();
      const auto input_tensor_elem_type = input_tensor_type.elem_type();
      const auto current_tensor_elem_type = current_type.sparse_tensor_type().elem_type();

      ORT_RETURN_IF_ERROR(OverrideTypesHelper(input_type, input_tensor_elem_type, current_tensor_elem_type,
                                              override_types));

      if (utils::HasShape(input_tensor_type)) {
        if (utils::HasShape(current_type)) {
          ORT_RETURN_IF_ERROR(MergeShapeInfo(Name(), input_type, current_type, strict, logger));
        } else {
          *current_type.mutable_sparse_tensor_type() = input_tensor_type;
        }
      }
      break;
    }
#endif

#if !defined(DISABLE_OPTIONAL_TYPE)
    case TypeProto::kOptionalType: {
      bool is_input_type_optional_tensor_type = utils::HasOptionalTensorType(input_type);
      bool is_current_type_optional_tensor_type = utils::HasOptionalTensorType(current_type);

      // Check for homogeneity within optional type
      if (is_input_type_optional_tensor_type != is_current_type_optional_tensor_type) {
        return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Optional Type mismatch. Expected: ",
                               ONNX_NAMESPACE::Utils::DataTypeUtils::ToType(current_type),
                               " . Got: ", ONNX_NAMESPACE::Utils::DataTypeUtils::ToType(input_type));
      }

      // Updating element type and shape is only applicable for optional tensors
      if (is_input_type_optional_tensor_type) {
        const auto& optional_input_type = utils::GetOptionalTypeProto(input_type);
        auto& optional_current_type = *utils::GetMutableOptionalTypeProto(current_type);

        const auto& input_tensor_type = optional_input_type.tensor_type();
        const auto& input_tensor_elem_type = input_tensor_type.elem_type();
        const auto& current_tensor_elem_type = optional_current_type.tensor_type().elem_type();

        ORT_RETURN_IF_ERROR(OverrideTypesHelper(input_type, input_tensor_elem_type, current_tensor_elem_type,
                                                override_types));

        if (utils::HasShape(optional_input_type.tensor_type())) {
          if (utils::HasShape(optional_current_type.tensor_type())) {
            ORT_RETURN_IF_ERROR(MergeShapeInfo(Name(), optional_input_type, optional_current_type, strict, logger));
          } else {
            *optional_current_type.mutable_tensor_type() = input_tensor_type;
          }
        }
      } else {
        // TODO: What is the plan for optional sequence tensors ?
        // Currently, we don't do anything for the generic sequence type
        // as seen below. This section needs an update if we choose to
        // change that in the future.
      }

      break;
    }
#endif

    case TypeProto::kSequenceType:
    case TypeProto::kMapType:
    case TypeProto::kOpaqueType:
    case TypeProto::VALUE_NOT_SET:
    default:
      break;
  }

  return Status::OK();
}

common::Status NodeArg::UpdateTypeAndShape(const NodeArg& node_arg, bool strict, bool override_types,
                                           const logging::Logger& logger) {
  auto status = Status::OK();

  if (utils::HasType(node_arg.node_arg_info_))
    status = UpdateTypeAndShape(node_arg.node_arg_info_.type(), strict, override_types, logger);

  return status;
}

void NodeArg::SetType(DataType p_type) {
  if (nullptr == p_type) {
    return;
  }

  type_ = p_type;
  *(node_arg_info_.mutable_type()) = DataTypeUtils::ToTypeProto(p_type);
}

#endif  // !defined(ORT_MINIMAL_BUILD)

#if !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD)

void NodeArg::SetType(const TypeProto& type_proto) {
  type_ = DataTypeUtils::ToType(type_proto);
  *(node_arg_info_.mutable_type()) = type_proto;
}

#endif  // !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD)

bool NodeArg::Exists() const noexcept {
  return exists_;
}

Node::EdgeEnd::EdgeEnd(const Node& node, int src_arg_index, int dst_arg_index) noexcept
    : node_(&node),
      src_arg_index_(src_arg_index),
      dst_arg_index_(dst_arg_index) {
}

Node::EdgeEnd::EdgeEnd(const Node& node) noexcept
    : EdgeEnd(node, INT_MAX, INT_MAX) {
}

Node::NodeConstIterator::NodeConstIterator(EdgeConstIterator p_iter) {
  m_iter = p_iter;
}

bool Node::NodeConstIterator::operator==(const NodeConstIterator& p_other) const {
  return m_iter == p_other.m_iter;
}

bool Node::NodeConstIterator::operator!=(const NodeConstIterator& p_other) const {
  return m_iter != p_other.m_iter;
}

void Node::NodeConstIterator::operator++() {
  ++m_iter;
}

void Node::NodeConstIterator::operator--() {
  --m_iter;
}

const Node& Node::NodeConstIterator::operator*() const {
  return (*m_iter).GetNode();
}

const Node* Node::NodeConstIterator::operator->() const {
  return &(operator*());
}

void Node::SetPriority(int priority) noexcept {
  priority_ = priority;
}

const std::filesystem::path& Node::ModelPath() const noexcept {
  return graph_->ModelPath();
}

#if !defined(ORT_MINIMAL_BUILD)

bool Node::CanBeInlined() const {
  if (func_body_ || func_template_)
    return true;
  if (!op_) return false;
  ONNX_NAMESPACE::FunctionProto function_proto;
  return TryGetFunctionProto(function_proto);
  // Note: We end up doing some redundant work, which can be eliminated if we cache
  // the constructed FunctionProto. Keeping the changes localized for now. A better
  // implementation would require some more invasive refactoring.
}

bool Node::TryGetFunctionProto(ONNX_NAMESPACE::FunctionProto& onnx_function_proto) const {
  if (func_template_) {
    onnx_function_proto = *func_template_->onnx_func_proto_;
    return true;
  } else if (op_) {
    auto get_opset_version = [op = op_](Graph* graph) -> std::optional<int> {
      if (op->domain() == kOnnxDomain) {
        const auto& domain_to_version = graph->DomainToVersionMap();
        const auto iter = domain_to_version.find(kOnnxDomain);
        if (iter != domain_to_version.cend()) {
          return iter->second;
        }
      }
      return {};
    };

    // Check if this node has a schema defined function proto.
    if (op_->HasContextDependentFunction()) {
      NodeProto node_proto;
      ToProto(node_proto, true);
      std::vector<TypeProto> input_types;
      for (size_t i = 0, n = InputDefs().size(); i < n; i++) {
        auto p_node_arg = InputDefs().at(i);
        if ((nullptr != p_node_arg) && p_node_arg->Exists()) {
          auto& type = *(p_node_arg->TypeAsProto());
          input_types.emplace_back(type);
        } else
          input_types.emplace_back();
      }

      auto requested_opset_version = get_opset_version(graph_);
      if (!requested_opset_version.has_value()) {
        requested_opset_version = SinceVersion();
      }
      ONNX_NAMESPACE::FunctionBodyBuildContextImpl function_body_ctx(node_proto, input_types);
      return op_->BuildContextDependentFunction(function_body_ctx, onnx_function_proto, *requested_opset_version);
    } else if (op_->HasFunction()) {
      const FunctionProto* function_ptr = nullptr;
      // We need to get a function-body suitable for the ONNX opset used by the model.
      // The first-parameter to GetFunction needs to be the ONNX opset used by the model.
      // Unfortunately, ONNX's function-registration code uses the function's since-version
      // as the default-version, which is incorrect in the case of functions belonging to
      // non-onnx domains, like MSDOMAIN.

      auto requested_opset_version = get_opset_version(graph_);
      if (requested_opset_version.has_value()) {
        function_ptr = op_->GetFunction(*requested_opset_version, true);
      } else {
        function_ptr = op_->GetFunction(SinceVersion(), false);
      }

      if (function_ptr != nullptr) {
        onnx_function_proto = *function_ptr;
        return true;
      }
    }
  }
  return false;
}

void Node::SetFunctionTemplate(const FunctionTemplate& func_template) {
  op_ = func_template.op_schema_.get();
  since_version_ = op_->since_version();
  func_template_ = &func_template;
}

void Node::ToProto(NodeProto& proto, bool update_subgraphs) const {
  proto.set_name(name_);
  proto.set_op_type(op_type_);

  if (!domain_.empty())
    proto.set_domain(domain_);

  if (!description_.empty())
    proto.set_doc_string(description_);

  // Checks an attribute was not removed.
  if (!can_be_saved_) {
    ORT_THROW("Removable attributes were removed before the conversion is started.");
  }

  // Set attributes.
  proto.clear_attribute();
  for (const auto& attribute : attributes_) {
    const gsl::not_null<AttributeProto*> attr{proto.add_attribute()};
    *attr = attribute.second;  // copy
    if (update_subgraphs && attr->has_g()) {
      attr->clear_g();
      *attr->mutable_g() = attr_to_subgraph_map_.find(attribute.first)->second->ToGraphProto();
    }
  }

  // Set inputs' definitions.
  proto.clear_input();
  for (auto& input_def : definitions_.input_defs) {
    proto.add_input(input_def->Name());
  }

  // Set outputs' definitions.
  proto.clear_output();
  for (auto& output_def : definitions_.output_defs) {
    proto.add_output(output_def->Name());
  }
}

Status Node::SaveToOrtFormat(flatbuffers::FlatBufferBuilder& builder,
                             flatbuffers::Offset<fbs::Node>& fbs_node) const {
  // if type is Primitive it's an ONNX function and currently we have kernel implementations for all those
  if (func_body_ != nullptr && node_type_ != Type::Primitive) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Serialization of fused function body is not currently supported, ",
                           "Node [", name_, "] op_type [", op_type_, "]");
  }

  auto GetNodeArgsOrtFormat = [&builder](const std::vector<NodeArg*>& src) {
    std::vector<flatbuffers::Offset<flatbuffers::String>> node_args(src.size());
    std::transform(src.cbegin(), src.cend(), node_args.begin(),
                   [&builder](const NodeArg* nodearg) {
                     // NodeArg's name will be used by multiple places, create shared string
                     return builder.CreateSharedString(nodearg->Name());
                   });
    return builder.CreateVector(node_args);
  };

  auto name = builder.CreateString(name_);
  auto doc_string = builder.CreateString(description_);
  auto domain = builder.CreateSharedString(domain_);
  auto op_type = builder.CreateSharedString(op_type_);
  auto ep = builder.CreateSharedString(execution_provider_type_);
  auto inputs = GetNodeArgsOrtFormat(definitions_.input_defs);
  auto outputs = GetNodeArgsOrtFormat(definitions_.output_defs);
  auto input_arg_counts = builder.CreateVector(definitions_.input_arg_count);
  auto implicit_inputs = GetNodeArgsOrtFormat(definitions_.implicit_input_defs);

  // Checks an attribute was not removed.
  if (!can_be_saved_) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Removable attributes were removed before the node is saved.");
  }

  // Node attributes
  std::vector<flatbuffers::Offset<fbs::Attribute>> attributes_vec;
  attributes_vec.reserve(attributes_.size());
  for (const auto& entry : attributes_) {
    const auto& attr_name = entry.first;
    const auto& attr_proto = entry.second;
    flatbuffers::Offset<fbs::Attribute> fbs_attr;
    Graph* subgraph = nullptr;
    if (attr_proto.has_g()) {
      const auto it = attr_to_subgraph_map_.find(attr_name);
      ORT_RETURN_IF_NOT(it != attr_to_subgraph_map_.cend(),
                        "Node [", name_, "] op_type [", op_type_, "] ", "does not have the graph for key ", attr_name);
      subgraph = it->second;
    }
    ORT_RETURN_IF_ERROR(
        fbs::utils::SaveAttributeOrtFormat(builder, attr_proto, fbs_attr, ModelPath(), subgraph));
    attributes_vec.push_back(fbs_attr);
  }
  auto attributes = builder.CreateVector(attributes_vec);

  fbs::NodeBuilder nb(builder);
  nb.add_name(name);
  nb.add_doc_string(doc_string);
  nb.add_domain(domain);
  nb.add_since_version(since_version_);
  nb.add_index(narrow<uint32_t>(index_));
  nb.add_op_type(op_type);
  nb.add_type(static_cast<fbs::NodeType>(node_type_));
  nb.add_execution_provider_type(ep);
  nb.add_inputs(inputs);
  nb.add_outputs(outputs);
  nb.add_attributes(attributes);
  nb.add_input_arg_counts(input_arg_counts);
  nb.add_implicit_inputs(implicit_inputs);
  fbs_node = nb.Finish();
  return Status::OK();
}

flatbuffers::Offset<fbs::NodeEdge> Node::SaveEdgesToOrtFormat(flatbuffers::FlatBufferBuilder& builder) const {
  auto get_edges = [](const EdgeSet& edge_set) {
    std::vector<fbs::EdgeEnd> edges;
    edges.reserve(edge_set.size());
    for (const auto& edge : edge_set)
      edges.push_back(fbs::EdgeEnd(narrow<uint32_t>(edge.GetNode().Index()),
                                   edge.GetSrcArgIndex(), edge.GetDstArgIndex()));

    return edges;
  };

  const auto input_edges = get_edges(relationships_.input_edges);
  const auto output_edges = get_edges(relationships_.output_edges);
  return fbs::CreateNodeEdgeDirect(builder, narrow<uint32_t>(index_), &input_edges, &output_edges);
}

#endif  // !defined(ORT_MINIMAL_BUILD)

Status Node::LoadFromOrtFormat(const onnxruntime::fbs::Node& fbs_node, Graph& graph,
                               const OrtFormatLoadOptions& load_options,
                               const logging::Logger& logger, std::unique_ptr<Node>& node) {
  node = std::make_unique<Node>(fbs_node.index(), graph);
  return node->LoadFromOrtFormat(fbs_node, load_options, logger);
}

Status Node::LoadFromOrtFormat(const onnxruntime::fbs::Node& fbs_node,
                               const OrtFormatLoadOptions& load_options,
                               const logging::Logger& logger) {
  auto LoadNodeArgsFromOrtFormat =
      [&](const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>* fbs_node_arg_names,
          std::vector<NodeArg*>& node_args,
          bool check_parent_graph = false) -> Status {
    ORT_RETURN_IF(nullptr == fbs_node_arg_names, "fbs_node_arg_names cannot be null");
    node_args.reserve(fbs_node_arg_names->size());
    for (const auto* node_arg_name : *fbs_node_arg_names) {
      ORT_RETURN_IF(nullptr == node_arg_name, "node_arg_name cannot be null");
      auto* node_arg = check_parent_graph ? graph_->GetNodeArgIncludingParentGraphs(node_arg_name->str())
                                          : graph_->GetNodeArg(node_arg_name->str());
      ORT_RETURN_IF(nullptr == node_arg, "LoadNodeArgsFromOrtFormat: Node [", name_, "] op_type [", op_type_,
                    "], could not find NodeArg ", node_arg_name->str());
      node_args.push_back(node_arg);
    }
    return Status::OK();
  };

  // index_ was set in the ctor of this Node instance
  fbs::utils::LoadStringFromOrtFormat(name_, fbs_node.name());
  fbs::utils::LoadStringFromOrtFormat(description_, fbs_node.doc_string());
  fbs::utils::LoadStringFromOrtFormat(domain_, fbs_node.domain());
  since_version_ = fbs_node.since_version();
  fbs::utils::LoadStringFromOrtFormat(op_type_, fbs_node.op_type());
  node_type_ = static_cast<Node::Type>(fbs_node.type());
  // we skip populating the saved EP here
  // the node will be assigned to an EP by the ORT format model-specific graph partitioning
  // fbs::utils::LoadStringFromOrtFormat(execution_provider_type_, fbs_node.execution_provider_type());
  ORT_RETURN_IF_ERROR(LoadNodeArgsFromOrtFormat(fbs_node.inputs(), definitions_.input_defs));

  // attributes
  auto fbs_attributes = fbs_node.attributes();
  if (fbs_attributes) {
    for (const auto* fbs_attr : *fbs_attributes) {
      ORT_RETURN_IF(nullptr == fbs_attr, "fbs_attr cannot be null");
      AttributeProto attr_proto;
      std::unique_ptr<Graph> subgraph;
      ORT_RETURN_IF_ERROR(
          fbs::utils::LoadAttributeOrtFormat(*fbs_attr, attr_proto, subgraph, *graph_, *this, load_options, logger));

      // If we have a sub graph in this attributes, it will be loaded into subgraph ptr
      // while the attribute proto contains the sub graph will have the empty g() field
      if (attr_proto.type() == AttributeProto_AttributeType_GRAPH) {
        ORT_RETURN_IF_NOT(subgraph, "Serialization error. Graph attribute was serialized without Graph instance");
        attr_to_subgraph_map_.emplace(attr_proto.name(), gsl::not_null<Graph*>(subgraph.get()));
        subgraphs_.push_back(std::move(subgraph));
      }

      AddAttributeProto(std::move(attr_proto));
    }
  }

  ORT_RETURN_IF_ERROR(LoadNodeArgsFromOrtFormat(fbs_node.implicit_inputs(), definitions_.implicit_input_defs,
                                                /* check parent graphs */ true));

  {  // input_arg_counts
    auto fbs_input_arg_counts = fbs_node.input_arg_counts();
    ORT_RETURN_IF(nullptr == fbs_input_arg_counts, "Node::LoadFromOrtFormat, input_arg_counts is missing");
    auto& input_arg_count = definitions_.input_arg_count;
    input_arg_count.reserve(fbs_input_arg_counts->size());
    input_arg_count.insert(input_arg_count.begin(), fbs_input_arg_counts->cbegin(), fbs_input_arg_counts->cend());
  }

  ORT_RETURN_IF_ERROR(LoadNodeArgsFromOrtFormat(fbs_node.outputs(), definitions_.output_defs));

  return Status::OK();
}

Status Node::LoadEdgesFromOrtFormat(const onnxruntime::fbs::NodeEdge& fbs_node_edges,
                                    const Graph& graph) {
  ORT_RETURN_IF(fbs_node_edges.node_index() != index_,
                "input index: ", fbs_node_edges.node_index(), " is not the same as this node's index:", index_);

  auto add_edges = [&graph](const flatbuffers::Vector<const onnxruntime::fbs::EdgeEnd*>* fbs_edges,
                            EdgeSet& edge_set, const std::string& dst_name) -> Status {
    if (fbs_edges) {
      for (const auto* fbs_edge : *fbs_edges) {
        ORT_RETURN_IF(nullptr == fbs_edge, "Node::LoadEdgesFromOrtFormat, edge is missing for ", dst_name);
        edge_set.emplace(*graph.GetNode(fbs_edge->node_index()), fbs_edge->src_arg_index(), fbs_edge->dst_arg_index());
      }
    }
    return Status::OK();
  };

  ORT_RETURN_IF_ERROR(add_edges(fbs_node_edges.input_edges(), relationships_.input_edges, "input edges"));
  ORT_RETURN_IF_ERROR(add_edges(fbs_node_edges.output_edges(), relationships_.output_edges, "output edges"));

  return Status::OK();
}

#if !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD) || defined(ORT_MINIMAL_BUILD_CUSTOM_OPS)
void Node::Init(std::string_view name,
                std::string_view op_type,
                std::string_view description,
                gsl::span<NodeArg* const> input_args,
                gsl::span<NodeArg* const> output_args,
                const NodeAttributes* attributes,
                std::string_view domain) {
  name_ = name;
  op_type_ = op_type;
  description_ = description;
  definitions_.input_defs.assign(input_args.begin(), input_args.end());
  definitions_.output_defs.assign(output_args.begin(), output_args.end());
  domain_ = domain;
  can_be_saved_ = true;
  priority_ = 0;
  if (kOnnxDomainAlias == domain_) {
    domain_ = kOnnxDomain;
  }

  // Set each arg count as 1 by default.
  // It could be adjusted when resolving the node with its operator
  // information.
  definitions_.input_arg_count.assign(input_args.size(), 1);

  if (attributes) {
    attributes_ = *attributes;

    for (auto& name_to_attr : attributes_) {
      if (utils::HasGraph(name_to_attr.second)) {
#if !defined(ORT_MINIMAL_BUILD)
        CreateSubgraph(name_to_attr.first);
#else
        ORT_THROW("Creating node with a subgraph via AddNode is not supported in this build.");
#endif
      }
    }
  }
}
void Node::Init(std::string_view name,
                std::string_view op_type,
                std::string_view description,
                gsl::span<NodeArg* const> input_args,
                gsl::span<NodeArg* const> output_args,
                NodeAttributes&& attributes,
                std::string_view domain) {
  name_ = name;
  op_type_ = op_type;
  description_ = description;
  definitions_.input_defs.assign(input_args.begin(), input_args.end());
  definitions_.output_defs.assign(output_args.begin(), output_args.end());
  domain_ = domain;
  can_be_saved_ = true;
  priority_ = 0;
  if (kOnnxDomainAlias == domain_) {
    domain_ = kOnnxDomain;
  }

  // Set each arg count as 1 by default.
  // It could be adjusted when resolving the node with its operator
  // information.
  definitions_.input_arg_count.assign(input_args.size(), 1);

  attributes_ = std::move(attributes);

  for (auto& name_to_attr : attributes_) {
    if (utils::HasGraph(name_to_attr.second)) {
#if !defined(ORT_MINIMAL_BUILD)
      CreateSubgraph(name_to_attr.first);
#else
      ORT_THROW("Creating node with a subgraph via AddNode is not supported in this build.");
#endif
    }
  }
}
#endif  // !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD) || defined(ORT_MINIMAL_BUILD_CUSTOM_OPS)

#if !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD)
Node::Definitions& Node::MutableDefinitions() noexcept {
  // someone fetching these is going to change something
  graph_->SetGraphResolveNeeded();
  graph_->SetGraphProtoSyncNeeded();
  return definitions_;
}

Node::Relationships& Node::MutableRelationships() noexcept {
  // someone fetching these is going to change something
  graph_->SetGraphResolveNeeded();
  graph_->SetGraphProtoSyncNeeded();
  return relationships_;
}
#endif  // !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD)

#if !defined(ORT_MINIMAL_BUILD)
void Node::CreateSubgraph(const std::string& attr_name) {
  auto attr = attributes_.find(attr_name);

  if (attr != attributes_.cend() && utils::HasGraph(attr->second)) {
    GraphProto& mutable_graph = *attr->second.mutable_g();
    std::unique_ptr<Graph> subgraph = std::make_unique<Graph>(*graph_, *this, mutable_graph);
    attr_to_subgraph_map_.insert({std::string(attr_name), gsl::not_null<Graph*>{subgraph.get()}});
    subgraphs_.emplace_back(std::move(subgraph));
  }
}

#endif  // !defined(ORT_MINIMAL_BUILD)

void Node::AddAttributeProto(AttributeProto value) {
  utils::SetNodeAttribute(std::move(value), attributes_);
  if (graph_) {
    graph_->SetGraphResolveNeeded();
    graph_->SetGraphProtoSyncNeeded();
  }
}

#define ADD_ATTR_SINGLE_IMPL(Type)                                                   \
  void Node::AddAttribute(std::string attr_name, Type value) {                       \
    AttributeProto a = utils::MakeAttribute(std::move(attr_name), std::move(value)); \
    AddAttributeProto(std::move(a));                                                 \
  }

#define ADD_ATTR_LIST_IMPL(Type)                                                 \
  void Node::AddAttribute(std::string attr_name, gsl::span<const Type> values) { \
    AttributeProto a = utils::MakeAttribute(std::move(attr_name), values);       \
    AddAttributeProto(std::move(a));                                             \
  }

#define ADD_ATTR_IMPLS(Type) \
  ADD_ATTR_SINGLE_IMPL(Type) \
  ADD_ATTR_LIST_IMPL(Type)

ADD_ATTR_IMPLS(int64_t)
ADD_ATTR_IMPLS(float)
ADD_ATTR_IMPLS(std::string)
ADD_ATTR_IMPLS(TensorProto)
#if !defined(DISABLE_SPARSE_TENSORS)
ADD_ATTR_IMPLS(SparseTensorProto)
#endif
ADD_ATTR_IMPLS(TypeProto)

#undef ADD_ATTR_SINGLE_IMPL
#undef ADD_ATTR_LIST_IMPL
#undef ADD_ATTR_IMPLS

void Node::AddAttribute(std::string attr_name, GraphProto value) {
  // Do not move attr_name as it is needed below
  AttributeProto a = utils::MakeAttribute(attr_name, std::move(value));
  AddAttributeProto(std::move(a));

#if !defined(ORT_MINIMAL_BUILD)
  // subgraph is created via deserialization and not here in a minimal build
  CreateSubgraph(attr_name);
#endif
};

#if !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD)
bool Node::ClearAttribute(const std::string& attr_name) {
  graph_->SetGraphResolveNeeded();
  graph_->SetGraphProtoSyncNeeded();
  return attributes_.erase(attr_name) > 0;
}

#endif  // !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD)

int Node::PruneRemovableAttributes(gsl::span<const std::string> removable_attributes) {
  graph_->SetGraphResolveNeeded();
  graph_->SetGraphProtoSyncNeeded();
  int n_removed = 0;
  for (const auto& name : removable_attributes) {
    n_removed += static_cast<int>(attributes_.erase(name));
  }
  can_be_saved_ = can_be_saved_ && n_removed == 0;
  return n_removed;
}

#if !defined(ORT_MINIMAL_BUILD)
Status Node::UpdateInputArgCount() {
  // The node refers to a primitive operator.
  // Infer and verify node input arg type information.
  int total_arg_count = std::accumulate(definitions_.input_arg_count.cbegin(),
                                        definitions_.input_arg_count.cend(), 0);

  if (total_arg_count < 0 || static_cast<size_t>(total_arg_count) != definitions_.input_defs.size()) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL,
                           "This is an invalid model. "
                           "The sum of input arg count is not equal to size of input defs in node (",
                           name_, ")");
  }

  // op_ is always valid when this is called
  const ONNX_NAMESPACE::OpSchema& op = *Op();

  // Verify size of node arg count is same as input number in
  // operator definition.
  if (op.inputs().size() != definitions_.input_arg_count.size()) {
    // Adjust input arg count array with op definition
    // The adjustment will work as below,
    // In total, there're <total_arg_count> inputs, which
    // will be split as <1, 1, 1, 1, ... 1, x> or
    // <1, 1, 1, 1, ...1, 0, 0, ...0>. The final input
    // arg count array's element number will be the same
    // as op definition, and the sum of all elements will
    // be equal to <total_arg_count>.
    auto& input_arg_count = definitions_.input_arg_count;
    input_arg_count.clear();
    size_t m = 0;
    auto arg_count_left = total_arg_count;

    if (!op.inputs().empty()) {
      for (; m < op.inputs().size() - 1; ++m) {
        if (arg_count_left > 0) {
          input_arg_count.push_back(1);
          arg_count_left--;
        } else {
          input_arg_count.push_back(0);
        }
      }
    }

    // Set the arg count for the last input formal parameter.
    // NOTE: in the case that there's no .input(...) defined
    // in op schema, all input args will be fed as one input
    // of the operator.
    input_arg_count.push_back(arg_count_left);

    graph_->SetGraphResolveNeeded();
    graph_->SetGraphProtoSyncNeeded();
  }

  return Status::OK();
}

Graph* Node::GetMutableGraphAttribute(const std::string& attr_name) {
  Graph* subgraph = nullptr;

  const auto& entry = attr_to_subgraph_map_.find(attr_name);
  if (entry != attr_to_subgraph_map_.cend()) {
    subgraph = entry->second;
  }

  return subgraph;
}

const Graph* Node::GetGraphAttribute(const std::string& attr_name) const {
  return const_cast<Node*>(this)->GetMutableGraphAttribute(attr_name);
}

void Node::ReplaceDefs(const std::map<const onnxruntime::NodeArg*, onnxruntime::NodeArg*>& replacements) {
  std::vector<std::vector<NodeArg*>*> all_defs = {&definitions_.input_defs, &definitions_.output_defs};

  for (auto pair : replacements)
    for (auto* defs : all_defs)
      for (auto& def : *defs)
        if (def == pair.first)
          def = pair.second;
}

#endif  // !defined(ORT_MINIMAL_BUILD)

std::vector<gsl::not_null<const Graph*>> Node::GetSubgraphs() const {
  std::vector<gsl::not_null<const Graph*>> subgraphs;
  subgraphs.reserve(attr_to_subgraph_map_.size());
  using value_type = std::unordered_map<std::string, gsl::not_null<Graph*>>::value_type;
  std::transform(attr_to_subgraph_map_.cbegin(), attr_to_subgraph_map_.cend(), std::back_inserter(subgraphs),
                 [](const value_type& entry) { return entry.second; });

  return subgraphs;
}

std::unordered_map<std::string, gsl::not_null<const Graph*>> Node::GetAttributeNameToSubgraphMap() const {
  std::unordered_map<std::string, gsl::not_null<const Graph*>> attr_to_subgraphs;
  for (auto& entry : attr_to_subgraph_map_) {
    attr_to_subgraphs.insert({entry.first, entry.second});
  }
  return attr_to_subgraphs;
}

void Node::ForEachDef(std::function<void(const onnxruntime::NodeArg&, bool is_input)> func,
                      bool include_missing_optional_defs) const {
  for (const auto* arg : InputDefs()) {
    if (include_missing_optional_defs || arg->Exists())
      func(*arg, true);
  }

  for (const auto* arg : ImplicitInputDefs()) {
    if (include_missing_optional_defs || arg->Exists())
      func(*arg, true);
  }

  for (const auto* arg : OutputDefs()) {
    if (include_missing_optional_defs || arg->Exists())
      func(*arg, false);
  }
};

// Constructor: Given a <GraphProto> loaded from model file, construct
// a <Graph> object and Resolve() it.
// Status Graph::LoadGraph(const GraphProto& graph_proto,
//                        const std::unordered_map<std::string, int>& domain_to_version,
//                        Version ir_version,
//                        std::unique_ptr<Graph>& new_graph) {
//  // create instance. need to call private ctor so can't use make_unique
//  GSL_SUPPRESS(r.11)
//  new_graph.reset(new Graph(nullptr, &graph_proto, domain_to_version, ir_version));
//
//  // as we just loaded from file we want to fully initialize/Resolve, but not let that change
//  // the proto sync flag
//  ResolveOptions options;
//  options.no_proto_sync_required = true;
//  auto status = new_graph->Resolve(options);
//  return status;
//}
using google::protobuf::RepeatedPtrField;

#if !defined(ORT_MINIMAL_BUILD)

Graph::Graph(const Model& owning_model,
             GraphProto* graph_proto,
             const std::unordered_map<std::string, int>& domain_to_version,
             Version ir_version,
             IOnnxRuntimeOpSchemaCollectionPtr schema_registry,
             const logging::Logger& logger,
             bool strict_shape_type_inference)
    : Graph(owning_model, graph_proto, domain_to_version, ir_version,
            schema_registry, nullptr, nullptr, logger, strict_shape_type_inference) {}

Graph::Graph(const Model& owning_model,
             GraphProto* graph_proto, const std::unordered_map<std::string, int>& domain_to_version, Version ir_version,
             IOnnxRuntimeOpSchemaCollectionPtr schema_registry, Graph* parent_graph, const Node* parent_node,
             const logging::Logger& logger,
             bool strict_shape_type_inference)
    : owning_model_(owning_model),
      graph_proto_(graph_proto),
#if !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD)
      runtime_optimizations_ptr_(std::make_unique<RuntimeOptimizationRecordContainer>()),
      runtime_optimizations_(*runtime_optimizations_ptr_),
#endif
      schema_registry_(schema_registry),
      graph_resolve_needed_(true),
      domain_to_version_(domain_to_version),
      ir_version_(ir_version),
      parent_graph_(parent_graph),
      parent_node_(parent_node),
      logger_(logger),
      strict_shape_type_inference_(strict_shape_type_inference),
      is_loaded_from_model_file_(GraphLoadedFromModelFile(graph_proto_)) {
  ORT_ENFORCE(graph_proto != nullptr, "graph_proto cannot be null");
  ArgNameToTypeMap name_to_type_map;
  const auto& model_path = ModelPath();

  // Process 'Constant' nodes
  // Put the 'TensorProto' stored in the 'Constant' nodes attribute into the graphs initializer list
  for (auto& node : graph_proto_->node()) {
    if (node.op_type() != kConstant) {
      continue;
    }

    const gsl::not_null<TensorProto*> tensor{graph_proto_->add_initializer()};
    ORT_THROW_IF_ERROR(utils::ConstantNodeProtoToTensorProto(node, model_path, *tensor));
    if constexpr (endian::native != endian::little) {
      const AttributeProto& attrib = node.attribute(0);
      if (attrib.type() == AttributeProto_AttributeType_SPARSE_TENSOR) {
        const TensorProto& sparse_values = node.attribute(0).sparse_tensor().values();
        if ((!(sparse_values.has_raw_data())) && utils::HasRawData(*tensor)) {
          onnxruntime::utils::ConvertRawDataInTensorProto(*tensor);
        }
      }
    }

    // Ensure initializers are also graph inputs.
    if (ir_version_ < 4) {
      TypeProto t{utils::TypeProtoFromTensorProto(*tensor)};
      const NodeArg& node_arg = GetOrCreateNodeArg(tensor->name(), &t);
      *(graph_proto_->add_input()) = node_arg.ToProto();
    }
#if !defined(DISABLE_SPARSE_TENSORS)
    if (node.attribute(0).type() == AttributeProto_AttributeType_SPARSE_TENSOR) {
      auto p = sparse_tensor_names_.emplace(tensor->name());
      ORT_ENFORCE(p.second, "Duplicate constant node sparse initializer name: '", tensor->name(),
                  "' Model is invalid.");
    }
#endif
  }

  if (owning_model_.IsLoadCancellationFlagSet()) {
    ORT_THROW_WITH_CATEGORY_AND_CODE(ONNXRUNTIME, MODEL_LOAD_CANCELED, "Graph loading canceled due to user request.");
  }

  // Remove constant nodes as they're replaced with initializers above.
  const gsl::not_null<RepeatedPtrField<NodeProto>*> graph_mutable_nodes{graph_proto_->mutable_node()};
  graph_mutable_nodes->erase(
      std::remove_if(graph_mutable_nodes->begin(), graph_mutable_nodes->end(),
                     [](NodeProto& p) {
                       return (p.op_type() == kConstant);
                     }),
      graph_mutable_nodes->end());

#if !defined(DISABLE_SPARSE_TENSORS)
  // For now we convert sparse_intializer to dense tensors
  // since there are currently no supported ops that consume sparse
  // initializers directly. We remove them from graph_proto. We will reconstitute them
  // when saving to ORT format to save space on disk.
  if (graph_proto_->sparse_initializer_size() > 0) {
    for (const auto& sparse_tensor : graph_proto_->sparse_initializer()) {
      ORT_ENFORCE(utils::HasName(sparse_tensor), "Sparse initializer must have a name. This model is invalid");
      const gsl::not_null<TensorProto*> tensor{graph_proto_->add_initializer()};
      auto status = utils::SparseTensorProtoToDenseTensorProto(sparse_tensor, model_path, *tensor);
      ORT_ENFORCE(status.IsOK(), status.ToString());
      auto p = sparse_tensor_names_.emplace(tensor->name());
      ORT_ENFORCE(p.second, "Duplicate sparse_tensor_initializer: '", tensor->name(), "' Model is invalid.");
    }

    // Remove sparse_initializers from protobuf to save memory as they are converted to dense now
    graph_proto_->mutable_sparse_initializer()->Clear();
#if GOOGLE_PROTOBUF_VERSION < 5026000
    const int sparse_num_cleared = graph_proto_->sparse_initializer().ClearedCount();
    for (int i = 0; i < sparse_num_cleared; ++i) {
      delete graph_proto_->mutable_sparse_initializer()->ReleaseCleared();
    }
#endif
  }
#endif

  // Collect all node arg name, type, shape information in the graph.
  // type/shape information will be assigned to each node arg when going
  // thru all nodes later.

  // process graph inputs first as we want the type/shape from them to be preferred if a graph input
  // has a matching initializer
  for (auto& graph_input : graph_proto_->input()) {
    if (utils::HasName(graph_input)) {
      if (utils::HasType(graph_input)) {
        name_to_type_map[graph_input.name()] = graph_input.type();
        GetOrCreateNodeArg(graph_input.name(), &graph_input.type());
      } else {
        // subgraph inputs can have type inferred later. need to create a NodeArg in case this input is only used in
        // a nested subgraph (a NodeArg won't be added by AddNode for the nodes in this subgraph)
        if (IsSubgraph()) {
          GetOrCreateNodeArg(graph_input.name(), nullptr);
        }
      }
    }
  }

  // Copy initial tensors to a map.
  for (auto& tensor : graph_proto_->initializer()) {
    auto p = name_to_initial_tensor_.emplace(tensor.name(), &tensor);
    if (!p.second) {
      LOGS(logger_, WARNING) << "Duplicate initializer (dense, sparse or ConstantNode): '" << tensor.name()
                             << "' the model will use the latest encountered initializer"
                             << ". Please, fix your model.";
      p.first->second = &tensor;
    }

    NodeArg* matching_graph_input = GetNodeArg(tensor.name());
    TypeProto t{utils::TypeProtoFromTensorProto(tensor)};

    if (!utils::HasElemType(t.tensor_type())) {
      ORT_THROW("This is an invalid model. Tensor does not have type information.");
    }

    if (utils::HasDataType(tensor) && (tensor.data_type() < TensorProto_DataType_DataType_ARRAYSIZE)) {
      weight_data_type_freq_[tensor.data_type()]++;
    }

    if (ir_version_ < 4) {
      // initializers can have matching graph inputs but are treated as constant,
      // so we prefer the shape from the initializer
      name_to_type_map[tensor.name()] = t;
      if (matching_graph_input != nullptr) {
        ORT_THROW_IF_ERROR(matching_graph_input->UpdateTypeAndShape(t, true, false, logger));
      }
    } else {
      // v4 and later allows a constant initializer with no matching graph input. create a NodeArg for these.
      // otherwise we prefer the shape from the graph input so leave matching_graph_input as is.
      if (matching_graph_input == nullptr) {
        name_to_type_map[tensor.name()] = t;
        ORT_IGNORE_RETURN_VALUE(GetOrCreateNodeArg(tensor.name(), &t));
      } else {
        LOGS(logger_, WARNING) << "Initializer " << tensor.name()
                               << " appears in graph inputs and will not be treated as constant value/weight. "
                               << "This may prevent some of the graph optimizations, like const folding. "
                               << "Move it out of graph inputs if there is no need to override it, "
                               << "by either re-generating the model with latest exporter/converter "
                               << "or with the tool onnxruntime/tools/python/remove_initializer_from_input.py.";
      }
    }
  }

  if (owning_model_.IsLoadCancellationFlagSet()) {
    ORT_THROW_WITH_CATEGORY_AND_CODE(ONNXRUNTIME, MODEL_LOAD_CANCELED, "Graph loading canceled due to user request.");
  }

  for (auto& graph_output : graph_proto_->output()) {
    if (utils::HasName(graph_output) && utils::HasType(graph_output)) {
      auto& name = graph_output.name();
      name_to_type_map[name] = graph_output.type();
      // always create NodeArg for graph output, in case it's from initializer
      GetOrCreateNodeArg(name, &graph_output.type());
    }
  }

  for (auto& node_arg : graph_proto_->value_info()) {
    if (utils::HasName(node_arg) && utils::HasType(node_arg)) {
      if (node_arg.name().size() > 0) {
        name_to_type_map[node_arg.name()] = node_arg.type();
      }
    }
  }

  for (const auto& node_proto : graph_proto_->node()) {
    AddNode(node_proto, name_to_type_map);
  }

  if (is_loaded_from_model_file_) {
    InitializeStateFromModelFileGraphProto();
  }
}

Graph::Graph(Graph& parent_graph, const Node& parent_node, ONNX_NAMESPACE::GraphProto& subgraph_proto)
    : Graph(parent_graph.owning_model_,
            &subgraph_proto,
            parent_graph.DomainToVersionMap(), parent_graph.IrVersion(), parent_graph.schema_registry_,
            &parent_graph,
            &parent_node,
            parent_graph.logger_,
            parent_graph.strict_shape_type_inference_) {
}

Graph::Graph(const Model& owning_model,
             IOnnxRuntimeOpSchemaCollectionPtr schema_registry,
             ONNX_NAMESPACE::GraphProto& subgraph_proto,
             const std::unordered_map<std::string, int>& domain_version_map,
             const logging::Logger& logger,
             bool strict_shape_type_inference)
    : Graph(owning_model,
            &subgraph_proto,
            domain_version_map,
            owning_model.IrVersion(),
            schema_registry,
            nullptr,
            nullptr,
            logger,
            strict_shape_type_inference) {
}

void Graph::InitializeStateFromModelFileGraphProto() {
  ORT_ENFORCE(
      graph_inputs_excluding_initializers_.empty() && graph_inputs_including_initializers_.empty() &&
          value_info_.empty() && graph_outputs_.empty(),
      "Graph state to be loaded into must be empty.");

  // Name to NodeArg mapping of all graph initializers.
  InlinedHashMap<std::string, const NodeArg*> graph_initializers;
  graph_initializers.reserve(graph_proto_->initializer_size());
  for (auto& initializer : graph_proto_->initializer()) {
    auto& initializer_name = initializer.name();
    auto initializer_arg = GetNodeArg(initializer_name);
    graph_initializers.insert({initializer_name, initializer_arg});
  }

  // Name to NodeArg mapping of all graph inputs.
  InlinedHashMap<std::string, const NodeArg*> graph_inputs;
  graph_inputs.reserve(graph_proto_->input_size());

  // Set graph inputs.
  // <graph_inputs_including_initializers_> contains inputs exactly specified in proto.
  // <graph_inputs_excluding_initializers_> contains inputs without default value (specified as initializer).
  for (auto& graph_input : graph_proto_->input()) {
    auto& name = graph_input.name();
    const auto* node_arg = GetNodeArg(name);
    ORT_ENFORCE(node_arg, "Graph ctor should have created NodeArg for initializer. Missing:", name);
    graph_inputs.insert({name, node_arg});
    graph_inputs_including_initializers_.push_back(node_arg);
    if (graph_initializers.end() == graph_initializers.find(name)) {
      graph_inputs_excluding_initializers_.push_back(node_arg);
    }
  }

  // Name to NodeArg mapping of all graph node outputs.
  InlinedHashMap<std::string, const NodeArg*> nodes_outputs;
  nodes_outputs.reserve(graph_proto_->node_size() * 2);  // rough estimate
  for (const auto& node : Nodes()) {
    for (const auto* output_def : node.OutputDefs()) {
      nodes_outputs.insert({output_def->Name(), output_def});
    }
  }

  // Set graph outputs.
  // Graph outputs specified in the model must be nodes' outputs, initializers or graph inputs.
  for (auto& graph_output : graph_proto_->output()) {
    auto& graph_output_name = graph_output.name();
    auto iter = nodes_outputs.find(graph_output_name);
    if (nodes_outputs.end() == iter) {
      // Graph output is not found as any node's output.
      auto iter2 = graph_initializers.find(graph_output_name);
      if (graph_initializers.end() == iter2) {
        // Graph output is not found as any initializer.
        auto iter3 = graph_inputs.find(graph_output_name);
        if (graph_inputs.end() == iter3) {
          if (parent_graph_ == nullptr ||
              parent_graph_->GetNodeArgIncludingParentGraphs(graph_output_name) == nullptr) {
            // Graph output is not found as any graph input.
            ORT_THROW("This is an invalid model. Graph output (", graph_output_name, ") does not exist in the graph.");
          } else {
            // Special case of a subgraph directly returning an outer scope value. This is not explicitly allowed
            // by the ONNX spec, and supporting it would potentially be complicated.
            ORT_THROW("This is an invalid model. Subgraph output (", graph_output_name,
                      ") is an outer scope value being returned directly. Please update the model to add an "
                      "Identity node between the outer scope value and the subgraph output.");
          }
        }
        graph_outputs_.push_back(iter3->second);
        continue;
      }
      graph_outputs_.push_back(iter2->second);
      continue;
    }
    graph_outputs_.push_back(iter->second);
  }

  // Set graph value_info_.
  for (const auto& graph_value_info : graph_proto_->value_info()) {
    const auto& name = graph_value_info.name();
    const auto* node_arg = GetNodeArg(name);
    if (node_arg != nullptr) {
      value_info_.insert(node_arg);
    }
  }

  ComputeOverridableInitializers();
}

Status Graph::VerifyNoDuplicateName() {
  auto& inputs_and_initializers = resolve_context_.inputs_and_initializers;
  auto& output_args = resolve_context_.output_args;
  auto& node_name_to_index = resolve_context_.node_name_to_index;

  output_args.clear();
  node_name_to_index.clear();
  // inputs_and_initializers: this is passed in as a parameter, since functions don't have initializers
  // but graphs have them.

  for (auto& node : Nodes()) {
    // Verify node name should be unique.
    auto& node_name = node.Name();

    if (!node_name.empty() && node_name_to_index.end() != node_name_to_index.find(node_name)) {
      // The node has name and its name was used by another node.
      Status status(ONNXRUNTIME, onnxruntime::common::StatusCode::FAIL,
                    "This is an invalid model. Error: two nodes with same node name (" + node_name + ").");
      return status;
    }

    node_name_to_index[node_name] = node.Index();

    // Verify node outputs' name should be unique.
    int output_index = -1;
    for (const auto* output_def : node.OutputDefs()) {
      ++output_index;
      if (output_def->Exists()) {
        auto& output_arg_name = output_def->Name();
        if (inputs_and_initializers.count(output_arg_name)) {
          Status status(ONNXRUNTIME, onnxruntime::common::StatusCode::FAIL,
                        "This is an invalid model. Error: Duplicate definition of name (" + output_arg_name + ").");
          return status;
        }
        auto result = output_args.insert({output_arg_name, {&node, output_index}});
        if (!result.second) {
          // Two outputs with same name, so that insertion fails.
          Status status(ONNXRUNTIME, onnxruntime::common::StatusCode::FAIL,
                        "This is an invalid model. Error: Duplicate definition of name (" + output_arg_name + ").");
          return status;
        }
      }
    }
  }
  return Status::OK();
}

#endif  // !defined(ORT_MINIMAL_BUILD)

void Graph::ConstructPrepackedSharedContainerAndSetMode(bool saving_mode_on) {
  if (parent_graph_ == nullptr) {
    prepacked_key_to_blobs_.emplace();
    prepacked_weights_for_graph_.emplace(*prepacked_key_to_blobs_, saving_mode_on);
  } else {
    // Subgraph
    prepacked_weights_for_graph_.emplace(parent_graph_->prepacked_weights_for_graph_->GetKeyToBlob(),
                                         saving_mode_on);
  }
}

#if !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD)
void Graph::AddEdge(NodeIndex src_node_index, NodeIndex dst_node_index, int src_arg_slot, int dst_arg_slot) {
  if (nodes_.size() <= src_node_index || src_arg_slot < 0 || nodes_.size() <= dst_node_index || dst_arg_slot < 0 ||
      nullptr == nodes_[src_node_index] || nullptr == nodes_[dst_node_index]) {
    // Invalid node indexes specified.
    ORT_THROW("Invalid node indexes specified when adding edge.");
  }

  NodeArg* src_arg = nullptr;
  NodeArg* dst_arg = nullptr;
  if (nodes_[src_node_index]->MutableDefinitions().output_defs.size() > static_cast<size_t>(src_arg_slot)) {
    src_arg = nodes_[src_node_index]->MutableDefinitions().output_defs[src_arg_slot];
  }

  if (nullptr == src_arg) {
    ORT_THROW("Invalid source node arg slot specified when adding edge.");
  }

  auto& dst_node_defs = nodes_[dst_node_index]->MutableDefinitions();
  NodeArg** dst_arg_pointer = nullptr;
  if (dst_node_defs.input_defs.size() > static_cast<size_t>(dst_arg_slot)) {
    dst_arg_pointer = &dst_node_defs.input_defs[dst_arg_slot];
    dst_arg = *dst_arg_pointer;
  } else {
    auto num_of_explicit_inputs = dst_node_defs.input_defs.size();
    if (num_of_explicit_inputs + dst_node_defs.implicit_input_defs.size() > static_cast<size_t>(dst_arg_slot)) {
      dst_arg_pointer = &dst_node_defs.implicit_input_defs[dst_arg_slot - num_of_explicit_inputs];
      dst_arg = *dst_arg_pointer;
    }
  }
  if (nullptr == dst_arg) {
    ORT_THROW("Invalid destination node arg slot specified when adding edge.");
  }

  if (src_arg != dst_arg) {
    if (src_arg->Type() != dst_arg->Type()) {
      // The output type of source node arg does not match the input type of destination node arg.
      ORT_THROW("Argument type mismatch when adding edge.");
    }
    *dst_arg_pointer = src_arg;
  }

  nodes_[src_node_index]->MutableRelationships().output_edges.insert(Node::EdgeEnd(*nodes_[dst_node_index],
                                                                                   src_arg_slot, dst_arg_slot));
  nodes_[dst_node_index]->MutableRelationships().input_edges.insert(Node::EdgeEnd(*nodes_[src_node_index],
                                                                                  src_arg_slot, dst_arg_slot));
}

void Graph::RemoveEdge(NodeIndex src_node_index, NodeIndex dst_node_index, int src_arg_slot, int dst_arg_slot) {
  if (nodes_.size() <= src_node_index || src_arg_slot < 0 || nodes_.size() <= dst_node_index || dst_arg_slot < 0 ||
      nullptr == nodes_[src_node_index] || nullptr == nodes_[dst_node_index]) {
    // Invalid node indexes specified.
    ORT_THROW("Invalid node indexes specified when removing edge.");
  }

  const NodeArg* src_arg = nullptr;
  const NodeArg* dst_arg = nullptr;
  if (nodes_[src_node_index]->GetDefinitions().output_defs.size() > static_cast<size_t>(src_arg_slot)) {
    src_arg = nodes_[src_node_index]->GetDefinitions().output_defs[src_arg_slot];
  }

  if (nullptr == src_arg) {
    ORT_THROW("Invalid source node arg slot specified when removing edge.");
  }

  auto& dst_node_defs = nodes_[dst_node_index]->GetDefinitions();
  if (dst_node_defs.input_defs.size() > static_cast<size_t>(dst_arg_slot)) {
    dst_arg = dst_node_defs.input_defs[dst_arg_slot];
  } else {
    auto num_of_explicit_inputs = dst_node_defs.input_defs.size();
    if (num_of_explicit_inputs + dst_node_defs.implicit_input_defs.size() > static_cast<size_t>(dst_arg_slot)) {
      dst_arg = dst_node_defs.implicit_input_defs[dst_arg_slot - num_of_explicit_inputs];
    }
  }
  if (nullptr == dst_arg) {
    ORT_THROW("Invalid destination node arg slot specified when removing edge.");
  }

  if (src_arg != dst_arg) {
    // The edge ends specified by source and destination arg slot are not referring to same node arg.
    // It means there was no edge between these two slots before.
    ORT_THROW("Argument mismatch when removing edge.");
  }

  nodes_[dst_node_index]->MutableRelationships().input_edges.erase(Node::EdgeEnd(*nodes_[src_node_index],
                                                                                 src_arg_slot, dst_arg_slot));
  nodes_[src_node_index]->MutableRelationships().output_edges.erase(Node::EdgeEnd(*nodes_[dst_node_index],
                                                                                  src_arg_slot, dst_arg_slot));
}
#endif  // !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD)

#if !defined(ORT_MINIMAL_BUILD)
GSL_SUPPRESS(es .84)  // ignoring return value from unordered_map::insert causes noisy complaint
Status Graph::BuildConnections(std::unordered_set<std::string>& outer_scope_node_args_consumed) {
  // recurse into subgraphs first so we can update any nodes in this graph that are used by those subgraphs
  if (!resolve_context_.nodes_with_subgraphs.empty()) {
    for (auto* node : resolve_context_.nodes_with_subgraphs) {
      for (auto& subgraph : node->MutableSubgraphs()) {
        std::unordered_set<std::string> node_args_consumed;
        ORT_RETURN_IF_ERROR(subgraph->BuildConnections(node_args_consumed));

        for (auto& node_arg_name : node_args_consumed) {
          auto node_arg = GetNodeArg(node_arg_name);

          if (node_arg == nullptr) {
            // it's a node arg from outside this graph's scope, so add that to the list we return
            // so that we can add the dependency at the next level up. this happens if you have multiple
            // levels of subgraphs between the graph with the original NodeArg and the subgraph with implicit usage.
            ORT_IGNORE_RETURN_VALUE(outer_scope_node_args_consumed.insert(node_arg_name));

            if (!parent_graph_) {
              return ORT_MAKE_STATUS(
                  ONNXRUNTIME, INVALID_GRAPH,
                  "This is an invalid model. At top level graph without matching NodeArg that subgraph consumes. Name=",
                  node_arg_name,
                  " Graph may not conform to the ONNX spec and contain initializers that are not graph inputs.");
            }

            node_arg = parent_graph_->GetNodeArgIncludingParentGraphs(node_arg_name);

            // make sure the node arg is found in the parent graph/s
            if (!node_arg) {
              return ORT_MAKE_STATUS(
                  ONNXRUNTIME, INVALID_GRAPH,
                  "This is an invalid model. Failed to find NodeArg in all parent graphs. Name=", node_arg_name,
                  " Graph may not conform to the ONNX spec and contain initializers that are not graph inputs.");
            }
          } else {
            // as we create a NodeArg instance for all Node inputs the value could be produced by this graph,
            // or could be coming from outer scope. check the valid values for just this Graph using resolve_context_.
            // if none are found, it's an outer scope value.
            if (resolve_context_.IsLocalValue(node_arg_name) == false) {
              ORT_IGNORE_RETURN_VALUE(outer_scope_node_args_consumed.insert(node_arg_name));
            }
          }

          // add it to the Node's list of implicit inputs
          auto& implicit_inputs = node->MutableDefinitions().implicit_input_defs;
          int input_slot_index = static_cast<int>(node->GetDefinitions().input_defs.size());
          auto iter = std::find(implicit_inputs.cbegin(), implicit_inputs.cend(), node_arg);
          if (implicit_inputs.cend() == iter) {
            implicit_inputs.push_back(node_arg);
            input_slot_index += static_cast<int>(implicit_inputs.size() - 1);
          } else {
            input_slot_index += static_cast<int>(iter - implicit_inputs.cbegin());
          }

          auto entry = resolve_context_.output_args.find(node_arg_name);
          if (entry != resolve_context_.output_args.end()) {
            // Create relationship between this node (node), and the node providing the output (output_node).
            Node& output_node = *entry->second.first;
            AddEdge(output_node.Index(), node->Index(), entry->second.second, input_slot_index);

            // If this Graph was built manually and the outputs were not manually set, remove the implicit input from
            // the graph outputs if it is present there.
            //
            // Otherwise, if the Graph was loaded from a GraphProto or the outputs were manually set, honor the
            // explicit graph outputs and leave as is.
            if (!is_loaded_from_model_file_ && !graph_outputs_manually_set_) {
              graph_outputs_.erase(std::remove(graph_outputs_.begin(), graph_outputs_.end(), node_arg),
                                   graph_outputs_.end());
            }
          }
        }
      }
    }
  }

  // now build connections within this Graph instance
  for (auto& node : Nodes()) {
    const auto input_args = node.InputDefs();

    if (!input_args.empty()) {
      // This node needs inputs.

      int input_slot_index = -1;
      for (const auto* input_arg : input_args) {
        ++input_slot_index;
        if (!input_arg->Exists()) {
          // This input could be optional and it does not exist in this case.
          continue;
        }

        const auto& input_arg_name = input_arg->Name();
        auto output_arg_iter = resolve_context_.output_args.find(input_arg_name);
        if (resolve_context_.output_args.end() != output_arg_iter) {
          // The input to this node is an output from a previous node in this graph.
          // Create relationship between this node (node), and the node providing the output (output_node).
          Node& output_node = *output_arg_iter->second.first;
          AddEdge(output_node.Index(), node.Index(), output_arg_iter->second.second, input_slot_index);
        } else {
          // the value is either an input, an initializer, or coming from outer scope. we only need to take action
          // if coming from outer scope, so first check if this is a subgraph (otherwise there is no outer scope).
          if (parent_graph_ != nullptr) {
            // make sure it's not an input or initializer first as those override any outer scope values
            if (resolve_context_.inputs_and_initializers.find(input_arg_name) ==
                resolve_context_.inputs_and_initializers.cend()) {
              // If it is present in the outer scope it will be 'fed' by the execution frame
              // providing access to the OrtValue from the outer scope. Pass the name back up so nodes can
              // be linked correctly at that level.
              if (resolve_context_.IsOuterScopeValue(input_arg_name)) {
                ORT_IGNORE_RETURN_VALUE(outer_scope_node_args_consumed.insert(input_arg_name));
              }
            }
          } else {
            // Check all the inputs are found.
            //
            // Ignore a Fused node as it could have moved things like initializers to a different device
            // (they're internally available to the fused node but removed from the Graph instance).
            // Fusion happens after the model was loaded in full so we know the inputs were valid originally.
            bool check = node.NodeType() != Node::Type::Fused;
#if defined(ENABLE_TRAINING_CORE)
            // Only check initial model load for training as graph modifications there also render inputs 'invalid'.
            check = check && num_resolves_ == 0;
#endif
            if (check &&
                resolve_context_.inputs_and_initializers.find(input_arg_name) ==
                    resolve_context_.inputs_and_initializers.cend() &&
                // if we're manually creating a Graph for use as a subgraph the outer scope names are manually set
                outer_scope_node_arg_names_.find(input_arg_name) == outer_scope_node_arg_names_.cend()) {
              return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Invalid model. Node input '", input_arg_name,
                                     "' is not a graph input, initializer, or output of a previous node.");
            }
          }
        }
      }
    } else if (node.OutputDefs().empty()) {
      // This is a useless node.
      // It has no input/output.
      RemoveNode(node.Index());
    }
  }

  ORT_RETURN_IF_ERROR(PopulateNodeArgToProducerConsumerLookupsFromNodes());

  return Status::OK();
}

#endif  // !defined(ORT_MINIMAL_BUILD)

NodeArg* Graph::GetNodeArgIncludingParentGraphs(const std::string& node_arg_name) {
  NodeArg* node_arg = GetNodeArg(node_arg_name);

  if (!node_arg && parent_graph_) {
    node_arg = parent_graph_->GetNodeArgIncludingParentGraphs(node_arg_name);
  }

  return node_arg;
}

const NodeArg* Graph::GetNodeArgIncludingParentGraphs(const std::string& node_arg_name) const {
  return const_cast<Graph*>(this)->GetNodeArgIncludingParentGraphs(node_arg_name);
}

void Graph::ReverseDFSFrom(gsl::span<NodeIndex const> from,
                           const std::function<void(const Node*)>& enter,
                           const std::function<void(const Node*)>& leave,
                           const std::function<bool(const Node*, const Node*)>& comp) const {
  InlinedVector<const Node*> node_vec;
  node_vec.reserve(from.size());
  for (auto i : from) {
    node_vec.push_back(GetNode(i));
  }

  ReverseDFSFrom(node_vec, enter, leave, comp, {});
}

void Graph::ReverseDFSFrom(gsl::span<const Node* const> from,
                           const std::function<void(const Node*)>& enter,
                           const std::function<void(const Node*)>& leave,
                           const std::function<bool(const Node*, const Node*)>& comp) const {
  ReverseDFSFrom(from, enter, leave, comp, {});
}

void Graph::ReverseDFSFrom(gsl::span<const Node* const> from,
                           const std::function<void(const Node*)>& enter,
                           const std::function<void(const Node*)>& leave,
                           const std::function<bool(const Node*, const Node*)>& comp,
                           const std::function<bool(const Node* from, const Node* to)>& stop) const {
  using WorkEntry = std::pair<const Node*, bool>;  // bool represents leave or not
  InlinedVector<WorkEntry> stack;
  stack.reserve(from.size());
  for (auto node : from) {
    stack.emplace_back(node, false);
  }

  InlinedVector<bool> visited(MaxNodeIndex(), false);
  while (!stack.empty()) {
    const WorkEntry last_entry = stack.back();
    stack.pop_back();

    if (last_entry.first == nullptr) {
      continue;
    }
    const Node& n = *last_entry.first;

    if (last_entry.second) {
      // leave node
      leave(&n);
      continue;
    }

    if (visited[n.Index()]) continue;

    visited[n.Index()] = true;

    if (enter) enter(&n);

    if (leave) stack.emplace_back(&n, true);

    if (comp) {
      InlinedVector<const Node*> sorted_nodes;
      for (auto iter = n.InputNodesBegin(); iter != n.InputNodesEnd(); ++iter) {
        if (stop && stop(&n, &(*iter))) continue;
        sorted_nodes.push_back(&(*iter));
      }
      std::sort(sorted_nodes.begin(), sorted_nodes.end(), comp);
      for (const auto* in : sorted_nodes) {
        const NodeIndex idx = in->Index();
        if (!visited[idx]) {
          stack.emplace_back(in, false);
        }
      }
    } else {
      for (auto iter = n.InputNodesBegin(); iter != n.InputNodesEnd(); ++iter) {
        if (stop && stop(&n, &(*iter))) continue;
        const NodeIndex idx = (*iter).Index();
        if (!visited[idx]) {
          stack.emplace_back(GetNode(idx), false);
        }
      }
    }
  }
}

template <typename T>
struct VisitorPriorityQueue {
  using ComparatorType = std::function<bool(T, T)>;
  std::list<T> list_;
  const ComparatorType comparator_ = nullptr;
  VisitorPriorityQueue(const ComparatorType& comp) : comparator_(comp) {}

  void push(T node) {
    list_.insert(
        std::upper_bound(list_.begin(), list_.end(), node, comparator_),
        node);
  }
  bool empty() { return list_.empty(); }
  T top() { return list_.back(); }
  void pop() { list_.pop_back(); }
};

#if !defined(ORT_MINIMAL_BUILD)
void Graph::KahnsTopologicalSort(const std::function<void(const Node*)>& enter,
                                 const std::function<bool(const Node*, const Node*)>& comp) const {
  InlinedVector<size_t> in_degree(MaxNodeIndex(), 0);
  InlinedVector<NodeIndex> topo_order;
  VisitorPriorityQueue<const Node*> to_visit(comp);

  auto number_of_nodes = NumberOfNodes();
  topo_order.reserve(number_of_nodes);

  for (auto& node : Nodes()) {
    size_t input_edge_count = node.GetInputEdgesCount();
    in_degree[node.Index()] = input_edge_count;
    if (input_edge_count == 0) {
      to_visit.push(&node);
    }
  }

  while (!to_visit.empty()) {
    const Node* current = to_visit.top();
    to_visit.pop();

    if (!current) continue;

    if (enter) {
      enter(current);
    }

    for (auto node_it = current->OutputNodesBegin(); node_it != current->OutputNodesEnd(); ++node_it) {
      auto& node_in_degree = in_degree[node_it->Index()];
      node_in_degree--;

      if (node_in_degree == 0) {
        to_visit.push(&*node_it);
      }
    }
    topo_order.push_back(current->Index());
  }

  if (number_of_nodes != static_cast<int>(topo_order.size())) {
    ORT_THROW("Some nodes are not included in the topological sort, graph have a cycle.");
  }
}

#ifdef ENABLE_TRAINING

namespace {

/**
 * @brief The data struct to store the group of nodes.
 *
 * The group contains a set of nodes, its execution depends on either of backward input leaf nodes,
 * graph inputs/initializers or other group node's output.
 */
struct GroupNode {
  GroupNode(const InlinedVector<const Node*>& node_list) {
    nodes = node_list;

    InlinedHashSet<const NodeArg*> intermediate_args;
    for (const Node* node : nodes) {
      for (const NodeArg* arg : node->InputDefs()) {
        if (intermediate_args.find(arg) == intermediate_args.end()) {
          input_args.push_back(arg);
        }
      }

      for (const NodeArg* arg : node->OutputDefs()) {
        intermediate_args.insert(arg);
      }

      if (node->GetOutputEdgesCount() == 0) {
        for (const NodeArg* arg : node->OutputDefs()) {
          output_args.push_back(arg);
        }

        continue;
      }

      for (auto output_edge_it = node->OutputEdgesBegin(); output_edge_it != node->OutputEdgesEnd();
           ++output_edge_it) {
        const Node* output_node = &output_edge_it->GetNode();
        // Only if the output arg is used by nodes outside the group, then it is an output arg.
        if (std::find(nodes.begin(), nodes.end(), output_node) == nodes.end()) {
          output_args.push_back(node->OutputDefs()[output_edge_it->GetSrcArgIndex()]);
        }
      }
    }
  }

  bool is_outputted{false};

  InlinedVector<const NodeArg*> input_args;
  InlinedVector<const NodeArg*> output_args;

  InlinedVector<const Node*> nodes;
};

struct NodeCompareByMaxDistance {
  explicit NodeCompareByMaxDistance(const InlinedHashMap<NodeIndex, int>& max_distance)
      : max_distance_(max_distance) {}
  bool operator()(const Node* n1, const Node* n2) const {
    if (max_distance_.at(n1->Index()) != max_distance_.at(n2->Index())) {
      // The longer distance node should be executed first.
      return max_distance_.at(n1->Index()) < max_distance_.at(n2->Index());
    }

    return n1->Index() < n2->Index();
  }

 private:
  const InlinedHashMap<NodeIndex, int>& max_distance_;
};

void SortForwardNodesByReverseDFS(const Graph* graph,
                                  const InlinedVector<const Node*>& forward_output_nodes,
                                  const InlinedHashMap<NodeIndex, InlinedVector<NodeIndex>>& shape_size_parents,
                                  InlinedHashSet<const Node*>& nodes_to_execute_before_yieldop,
                                  std::vector<NodeIndex>& node_orders) {
  // Note 1: YieldOp is the separator of forward and backward nodes.
  // Note 2: While it is also possible some nodes not contributing to the forward output nodes will be
  //   executed before YieldOp, for example, if one forward node's output is used by Shape/Size, then
  //   the Shape/Size node should be executed before YieldOp to release the memory as soon as possible.
  InlinedVector<size_t> nodes_in_degree;
  std::queue<const Node*> to_visit;
  nodes_in_degree.resize(graph->MaxNodeIndex(), 0);
  for (auto& node : graph->Nodes()) {
    size_t input_edge_count = node.GetInputEdgesCount();
    nodes_in_degree[node.Index()] = input_edge_count;
    if (input_edge_count == 0) {
      to_visit.push(&node);
    }
  }

  InlinedHashMap<NodeIndex, int> max_distance;
  max_distance.reserve(graph->NumberOfNodes());
  while (!to_visit.empty()) {
    const Node* current = to_visit.front();
    to_visit.pop();

    if (!current) continue;

    for (auto output_edge_it = current->OutputEdgesBegin();
         output_edge_it != current->OutputEdgesEnd();
         ++output_edge_it) {
      const Node* out_node = &output_edge_it->GetNode();
      max_distance[out_node->Index()] = std::max(max_distance[out_node->Index()],
                                                 max_distance[current->Index()] + 1);
      auto& node_in_degree = nodes_in_degree[out_node->Index()];
      node_in_degree--;
      if (node_in_degree == 0) {
        to_visit.push(out_node);
      }
    }
  }

  // Reverse DFS from forward output nodes to find all "forward" nodes.
  // The forward nodes are ordered by Reverse DFS tranverse.
  graph->ReverseDFSFrom(
      forward_output_nodes,
      nullptr,
      [&nodes_to_execute_before_yieldop, &node_orders](const Node* n) {
        nodes_to_execute_before_yieldop.insert(n);
        node_orders.push_back(n->Index());
      },
      NodeCompareByMaxDistance(max_distance));

  for (const auto& parent_to_children_pair : shape_size_parents) {
    const NodeIndex& parent_index = parent_to_children_pair.first;
    if (nodes_to_execute_before_yieldop.find(graph->GetNode(parent_index)) == nodes_to_execute_before_yieldop.end()) {
      continue;
    }

    for (const NodeIndex& shape_size_node_index : parent_to_children_pair.second) {
      const Node* shape_size_node = graph->GetNode(shape_size_node_index);
      // If the Shape/Size is already in the node_orders, then skip it.
      if (nodes_to_execute_before_yieldop.find(shape_size_node) != nodes_to_execute_before_yieldop.end()) {
        continue;
      }

      auto it = std::find(node_orders.begin(), node_orders.end(), parent_index);
      ORT_ENFORCE(it != node_orders.end(), "Cannot find the parent node in the node orders.");

      node_orders.insert(it + 1, shape_size_node_index);
      nodes_to_execute_before_yieldop.insert(shape_size_node);
    }
  }
}

void PrepareToFindBranchGraph(const Graph* graph,
                              std::function<bool(const Node*)> is_forward_node,
                              InlinedVector<const Node*>& branch_graph_input_nodes,
                              InlinedVector<size_t>& backward_node_in_degree,
                              std::queue<const Node*>& to_visit) {
  for (auto& node : graph->Nodes()) {
    // Ignore forward.
    if (is_forward_node(&node)) {
      continue;
    }

    if (node.OpType() == "YieldOp") {
      backward_node_in_degree[node.Index()] = 0;
      to_visit.push(&node);
      continue;
    }

    size_t input_edge_count = node.GetInputEdgesCount();
    backward_node_in_degree[node.Index()] = input_edge_count;

    // A shortcut: input_edge_count could be 0 if it takes graph input directly.
    if (input_edge_count == 0) {
      branch_graph_input_nodes.push_back(&node);
      continue;
    }

    for (auto input_edge_it = node.InputEdgesBegin(); input_edge_it != node.InputEdgesEnd(); ++input_edge_it) {
      const Node* input_node = &input_edge_it->GetNode();
      // If the input edge connect to forward nodes, then we remove the in_degree of the node.
      if (is_forward_node(input_node)) {
        input_edge_count--;
      }
    }

    backward_node_in_degree[node.Index()] = input_edge_count;
    if (input_edge_count == 0) {
      branch_graph_input_nodes.push_back(&node);
    }
  }
}

void FindBranchGraph(
    const InlinedVector<const Node*>& branch_graph_input_nodes,
    const InlinedVector<size_t>& backward_node_in_degree,
    InlinedVector<const Node*>& branch_graph,
    InlinedVector<std::pair<const Node*, size_t>>& branch_subgraph_consumers,
    InlinedVector<const NodeArg*>& branch_subgraph_outputs) {
  // Loop through the branch_graph_input_nodes to find the branch subgraphs by its output edges in BFS,
  // and find the maximum self_contained subgraph taking the branch_graph_input_nodes as input nodes.
  std::queue<const Node*> to_visit_queue;
  InlinedVector<size_t> in_degree_copy = backward_node_in_degree;

  // Add all nodes in branch_graph_input_nodes to the queue
  for (auto branch_input_node : branch_graph_input_nodes) {
    to_visit_queue.push(branch_input_node);
    branch_graph.push_back(branch_input_node);
  }

  while (!to_visit_queue.empty()) {
    const Node* current = to_visit_queue.front();
    to_visit_queue.pop();

    if (!current) continue;

    for (auto node_it = current->OutputNodesBegin(); node_it != current->OutputNodesEnd(); ++node_it) {
      auto& node_in_degree = in_degree_copy[node_it->Index()];
      node_in_degree--;

      if (node_in_degree == 0) {
        to_visit_queue.push(&*node_it);
        branch_graph.push_back(&*node_it);
      }
    }
  }

  // At this point, branch_graph is a big subgraph that contains all the nodes that are purely
  // triggered by the branch_graph_input_nodes, other graph input/initializers and leaf nodes (for example Constant).
  for (const Node* n : branch_graph) {
    if (n->GetOutputEdgesCount() == 0) {
      // In case the node connect to graph outputs or nothings, append all outputs as the branch subgraph outputs.
      for (auto output_def : n->OutputDefs()) {
        branch_subgraph_outputs.push_back(output_def);
      }
      continue;
    }

    for (auto output_it = n->OutputEdgesBegin(); output_it != n->OutputEdgesEnd(); ++output_it) {
      const Node* output_node = &output_it->GetNode();
      const size_t dest_in_port = output_it->GetDstArgIndex();
      if (std::find(branch_graph.begin(), branch_graph.end(), output_node) == branch_graph.end()) {
        branch_subgraph_consumers.push_back({output_node, dest_in_port});
        branch_subgraph_outputs.push_back(n->OutputDefs()[output_it->GetSrcArgIndex()]);
      }
    }
  }
}

void TagNodeToAssociatedOutputs(const Graph* graph,
                                const InlinedHashSet<const Node*>& nodes_to_execute_before_yieldop,
                                const InlinedVector<const NodeArg*>& branch_subgraph_outputs,
                                const InlinedVector<const Node*>& branch_graph,
                                InlinedVector<GroupNode>& group_node_collection,
                                InlinedHashMap<const NodeArg*, GroupNode*>& output_arg_to_grouped_node) {
  // Reverse DFS from branch graph outputs (e.g. branch_subgraph_outputs) to tag each nodes:
  // If one node N contributes to a graph output A, then we will tag A to N.
  // If the node N contributes to multiple graph outputs A, B, C, then we will tag the A, B, C to N.
  InlinedHashMap<const Node*, std::set<const NodeArg*>> node_to_its_associated_outputs;
  node_to_its_associated_outputs.reserve(branch_graph.size());
  InlinedHashSet<const Node*> handled_branch_subgraph_end_nodes;
  for (const auto& output_arg : branch_subgraph_outputs) {
    const Node* end_node = graph->GetProducerNode(output_arg->Name());
    handled_branch_subgraph_end_nodes.insert(end_node);

    InlinedVector<const Node*> end_nodes{end_node};
    graph->ReverseDFSFrom(
        end_nodes,
        nullptr,
        [&node_to_its_associated_outputs, &output_arg](const Node* n) {
          node_to_its_associated_outputs[n].insert(output_arg);
        },
        nullptr,
        [&nodes_to_execute_before_yieldop](const Node*, const Node* to) -> bool {
          if (nodes_to_execute_before_yieldop.find(to) != nodes_to_execute_before_yieldop.end()) {
            return true;  // Skip forward nodes.
          }

          return false;
        });
  }

  // Cluster the nodes in the branch_graph based on the associated outputs.
  InlinedHashMap<std::set<const NodeArg*>, InlinedVector<const Node*>> associated_outputs_to_nodes;
  associated_outputs_to_nodes.reserve(node_to_its_associated_outputs.size());
  for (const auto& node : branch_graph) {
    const std::set<const NodeArg*>& associated_outputs = node_to_its_associated_outputs[node];
    associated_outputs_to_nodes[associated_outputs].push_back(node);
  }

  // Finalize the subgraph inputs/output information.
  group_node_collection.reserve(associated_outputs_to_nodes.size());
  for (auto& [associated_outputs, nodes] : associated_outputs_to_nodes) {
    group_node_collection.push_back(nodes);

    // Flatten the key into NodeArg* for better search.
    GroupNode& grouped_node = group_node_collection.back();
    for (const auto& output_arg : grouped_node.output_args) {
      output_arg_to_grouped_node.insert({output_arg, &grouped_node});
    }
  }
}

void UpdateBackwardInDegree(InlinedVector<size_t>& backward_node_in_degree,
                            InlinedVector<std::pair<const Node*, size_t>>& branch_subgraph_consumers) {
  // For each GroupNode, its execution is non-blocking main critical path rooting from YieldOp.
  // The only dependencies of a GroupNode is either graph input/initializer/forward nodes, or
  // the output nodes of another GroupNode.
  // So we treat those GroupNode(s) as a single unit that can be executed anytime when it is
  // firstly needed by the main critipath path.
  for (auto& [output_node, dest_in_port] : branch_subgraph_consumers) {
    ORT_ENFORCE(backward_node_in_degree[output_node->Index()] > 0);
    backward_node_in_degree[output_node->Index()]--;
  }
}

void OutputGroupedNodes(const Graph* graph,
                        const NodeArg* output_arg,
                        const InlinedHashMap<const NodeArg*, GroupNode*>& output_arg_to_grouped_node,
                        std::vector<NodeIndex>& node_orders,
                        InlinedVector<NodeIndex>& topo_order) {
  ORT_ENFORCE(output_arg_to_grouped_node.find(output_arg) != output_arg_to_grouped_node.end(),
              "output_arg_to_grouped_node does not contain output_arg named ", output_arg->Name());

  GroupNode* grouped_node = output_arg_to_grouped_node.at(output_arg);

  if (grouped_node->is_outputted) {
    return;
  }

  for (const NodeArg* input_arg : grouped_node->input_args) {
    if (!input_arg->Exists()) {
      continue;
    }

    auto it = output_arg_to_grouped_node.find(input_arg);
    if (it != output_arg_to_grouped_node.end() && !it->second->is_outputted) {
      OutputGroupedNodes(graph, input_arg, output_arg_to_grouped_node, node_orders, topo_order);
    }
  }

  for (const Node* n : grouped_node->nodes) {
    node_orders.push_back(n->Index());
    topo_order.push_back(n->Index());
  }

  grouped_node->is_outputted = true;
}

}  // namespace

void Graph::MemoryEfficientTopologicalSort(const Node* yield_op,
                                           const InlinedHashMap<NodeIndex, InlinedVector<NodeIndex>>& shape_size_parents,
                                           std::vector<NodeIndex>& node_orders) const {
  /// Firstly, sort the forward nodes with customized ReverseDFS.

  const size_t num_nodes = NumberOfNodes();
  InlinedVector<const Node*> forward_output_nodes;
  forward_output_nodes.reserve(yield_op->GetInputEdgesCount());
  for (auto input_it = yield_op->InputNodesBegin(); input_it != yield_op->InputNodesEnd(); ++input_it) {
    forward_output_nodes.push_back(&*input_it);
  }

  // Create a hash map (paired with node_orders) for cheaper search.
  InlinedHashSet<const Node*> nodes_to_execute_before_yieldop;
  nodes_to_execute_before_yieldop.reserve(num_nodes);

  SortForwardNodesByReverseDFS(this, forward_output_nodes,
                               shape_size_parents,
                               nodes_to_execute_before_yieldop,
                               node_orders);

  /// Secondly, sort the backward nodes with customized Kahn's algorithm.

  size_t num_of_backward_nodes = num_nodes - node_orders.size();
  InlinedVector<size_t> backward_node_in_degree(MaxNodeIndex(), 0);
  InlinedVector<NodeIndex> topo_order;
  topo_order.reserve(num_of_backward_nodes);
  std::queue<const Node*> to_visit;

  auto is_forward_op = [&nodes_to_execute_before_yieldop](const Node* n) -> bool {
    return nodes_to_execute_before_yieldop.find(n) != nodes_to_execute_before_yieldop.end();
  };

  InlinedVector<const Node*> branch_graph_input_nodes;
  branch_graph_input_nodes.reserve(num_of_backward_nodes);

  PrepareToFindBranchGraph(this,
                           is_forward_op,
                           branch_graph_input_nodes,
                           backward_node_in_degree,
                           to_visit);

  InlinedVector<const Node*> branch_graph;
  branch_graph.reserve(num_of_backward_nodes);
  InlinedVector<std::pair<const Node*, size_t>> branch_subgraph_consumers;
  InlinedVector<const NodeArg*> branch_subgraph_outputs;
  FindBranchGraph(branch_graph_input_nodes,
                  backward_node_in_degree,
                  branch_graph,
                  branch_subgraph_consumers,
                  branch_subgraph_outputs);

  // Cluster the nodes in the branch_graph based on the associated outputs.
  InlinedVector<GroupNode> group_node_collection;
  InlinedHashMap<const NodeArg*, GroupNode*> output_arg_to_grouped_node;
  TagNodeToAssociatedOutputs(this,
                             nodes_to_execute_before_yieldop,
                             branch_subgraph_outputs,
                             branch_graph,
                             group_node_collection,
                             output_arg_to_grouped_node);

  UpdateBackwardInDegree(backward_node_in_degree, branch_subgraph_consumers);

  while (!to_visit.empty()) {
    const Node* current = to_visit.front();
    to_visit.pop();

    if (!current) continue;

    for (auto input_edge_it = current->InputEdgesBegin(); input_edge_it != current->InputEdgesEnd();
         ++input_edge_it) {
      const NodeArg* input_arg = current->InputDefs()[input_edge_it->GetDstArgIndex()];
      if (!input_arg->Exists()) {
        continue;
      }

      auto it = output_arg_to_grouped_node.find(input_arg);
      if (it != output_arg_to_grouped_node.end() && !it->second->is_outputted) {
        OutputGroupedNodes(this, input_arg, output_arg_to_grouped_node, node_orders, topo_order);
      }
    }

    node_orders.push_back(current->Index());

    for (auto output_edge_it = current->OutputEdgesBegin(); output_edge_it != current->OutputEdgesEnd();
         ++output_edge_it) {
      const Node* out_node = &output_edge_it->GetNode();
      auto& node_in_degree = backward_node_in_degree[out_node->Index()];
      node_in_degree--;
      if (node_in_degree == 0) {
        to_visit.push(out_node);
      }
    }

    topo_order.push_back(current->Index());
  }

  // For the group nodes that are not outputted, we need to output them.
  // Hitting this code path means some nodes are consuming outputs of forward nodes, and their outputs
  // are not used by main branch backward nodes.
  InlinedVector<std::pair<const NodeArg*, GroupNode*>>
      left_output_arg_to_grouped_node_vector;  // To ensure deterministic order.
  left_output_arg_to_grouped_node_vector.reserve(output_arg_to_grouped_node.size());
  for (auto& [output_arg, grouped_node] : output_arg_to_grouped_node) {
    if (!grouped_node->is_outputted) {
      left_output_arg_to_grouped_node_vector.push_back({output_arg, grouped_node});
    }
  }

  if (!left_output_arg_to_grouped_node_vector.empty()) {
    // Sort to ensure deterministic order.
    std::sort(left_output_arg_to_grouped_node_vector.begin(), left_output_arg_to_grouped_node_vector.end(),
              [](const std::pair<const NodeArg*, GroupNode*>& a, const std::pair<const NodeArg*, GroupNode*>& b) {
                return a.first->Name() < b.first->Name();
              });
    for (const auto& pair : left_output_arg_to_grouped_node_vector) {
      const NodeArg* output_arg = pair.first;
      GroupNode* grouped_node = pair.second;
      if (!grouped_node->is_outputted) {
        OutputGroupedNodes(this, output_arg, output_arg_to_grouped_node, node_orders, topo_order);
      }
    }
  }

  if (num_of_backward_nodes != topo_order.size()) {
    ORT_THROW("Some nodes for backward are not included in the topological sort: " +
              std::to_string(num_of_backward_nodes) + " vs " +
              std::to_string(topo_order.size()));
  }
}

#endif  // ENABLE_TRAINING

GSL_SUPPRESS(es .84)  // noisy warning about ignoring return value from insert(...)
Status Graph::PerformTopologicalSortAndCheckIsAcyclic() {
  nodes_in_topological_order_.clear();
  std::unordered_set<NodeIndex> downstream_nodes;  // nodes downstream of the node we're currently checking
  std::unordered_set<NodeIndex> nodes_seen;        // nodes we have seen but may not have been added to nodes_added yet
  std::unordered_set<NodeIndex> nodes_added;       // nodes added to topo order
  std::stack<NodeIndex> stack;

  // push the root nodes into nodes_in_topological_order in the order they were defined in the model
  // to ensure that is consistent.
  auto& nodes_in_original_order = Nodes();
  std::for_each(nodes_in_original_order.cbegin(), nodes_in_original_order.cend(),
                [&](const Node& node) {
                  auto index = node.Index();

                  // find the top level nodes in the graph.
                  // need to also consider nodes that only have Constants as inputs as top level nodes,
                  // as the constant will get replaced by an initializer.
                  auto input_edges = node.GetRelationships().input_edges;
                  auto has_inputs = std::any_of(input_edges.cbegin(), input_edges.cend(),
                                                [](const Node::EdgeEnd& edge) {
                                                  return edge.GetNode().OpType() != kConstant;
                                                });

                  if (!has_inputs) {
                    // add to the topological list, and ensure we skip these nodes when walking the graph
                    nodes_in_topological_order_.push_back(index);
                    nodes_added.insert(index);
                    nodes_seen.insert(index);
                  }
                });

  // find all the leaf nodes (nodes with no output edges as there's no edge to a graph output)
  for (auto iter = Nodes().begin(); iter != Nodes().end(); ++iter) {
    if (iter->relationships_.output_edges.empty()) {
      stack.push(iter->Index());
    }
  }

  // work our way up from the leaf nodes
  while (!stack.empty()) {
    const NodeIndex current = stack.top();
    stack.pop();

    if (nodes_added.find(current) != nodes_added.end()) {
      continue;
    }

    if (nodes_seen.find(current) != nodes_seen.end()) {
      // we popped the stack and are back to a node that was seen previously,
      // so we know all the upstream nodes from it have been added.
      nodes_in_topological_order_.push_back(current);
      nodes_added.insert(current);
      downstream_nodes.erase(current);
      continue;
    }

    const Node* node = GetNode(current);
    if (!node) {
      continue;
    }

    // node hasn't been seen before, so mark it as seen and re-add it along with its inputs
    // also mark it as downstream of anything new that is added to the stack to detect acyclic graphs
    nodes_seen.insert(current);
    downstream_nodes.insert(current);

    stack.push(current);

    for (auto iter = node->InputNodesBegin(), end = node->InputNodesEnd(); iter != end; ++iter) {
      const NodeIndex idx = iter->Index();
      // the input to this node is also downstream of this node
      if (downstream_nodes.find(idx) != downstream_nodes.end()) {
        Status status(ONNXRUNTIME, onnxruntime::common::StatusCode::FAIL,
                      "This is an invalid model. Error: the graph is not acyclic.");
        return status;
      }

      // avoid re-processing nodes
      if (nodes_seen.find(idx) == nodes_seen.end()) {
        stack.push(idx);
      }
    }
  }

  if (num_of_nodes_ >= 0 && static_cast<size_t>(num_of_nodes_) == nodes_in_topological_order_.size()) {
    return Status::OK();
  }

  return Status(ONNXRUNTIME, onnxruntime::common::StatusCode::FAIL,
                "This is an invalid model. Error: the graph is not acyclic.");
}

bool FullyDefinedType(const TypeProto& type_proto) {
  switch (type_proto.value_case()) {
    case TypeProto::kTensorType: {
      auto& tensor_type = type_proto.tensor_type();
      return utils::HasElemType(tensor_type);
    }
#if !defined(DISABLE_SPARSE_TENSORS)
    case TypeProto::kSparseTensorType: {
      auto& tensor_type = type_proto.sparse_tensor_type();
      return utils::HasElemType(tensor_type);
    }
#endif
    case TypeProto::kSequenceType: {
      auto& seq_type = type_proto.sequence_type();
      return utils::HasElemType(seq_type) && FullyDefinedType(seq_type.elem_type());
    }
#if !defined(DISABLE_OPTIONAL_TYPE)
    case TypeProto::kOptionalType: {
      auto& optional_type = type_proto.optional_type();
      return utils::HasElemType(optional_type) && FullyDefinedType(optional_type.elem_type());
    }
#endif
    case TypeProto::kMapType: {
      auto& map_type = type_proto.map_type();
      return utils::HasKeyType(map_type) &&
             utils::HasValueType(map_type) &&
             FullyDefinedType(map_type.value_type());
    }
    case TypeProto::kOpaqueType:
      return true;
    case TypeProto::VALUE_NOT_SET:
    default:
      return false;
  }
}

// function to handle type/shape inferencing of a subgraph.
// parameters are the Graph instance for the subgraph, the input types from the control flow node that contains
// the subgraph, and the vector to write the output from the inferencing.
using SubgraphInferencingFunc =
    std::function<Status(const Node&, Graph&, const std::vector<const TypeProto*>&, std::vector<const TypeProto*>&,
                         const Graph::ResolveOptions&)>;

class GraphInferencerImpl : public ONNX_NAMESPACE::GraphInferencer {
 public:
  GraphInferencerImpl(const Node& node, Graph& graph, SubgraphInferencingFunc& inferencing_func,
                      const Graph::ResolveOptions& options)
      : node_(node), graph_(graph), inferencing_func_(inferencing_func), options_(options) {
  }

  // Perform inferencing on the graph contained in GraphInferencer.
  // Returns the graph output types post-inferencing.
  // We ignore input_data currently as the inferencing happens prior to receiving user input.
  std::vector<const TypeProto*> doInferencing(const std::vector<const TypeProto*>& input_types,
                                              const std::vector<const TensorProto*>& /*input_data*/) override {
    std::vector<const TypeProto*> output_types;

    auto status = inferencing_func_(node_, graph_, input_types, output_types, options_);

    if (status != Status::OK()) {
      fail_type_inference("Graph attribute inferencing failed: ", status.ErrorMessage());
    }

    return output_types;
  }

 private:
  const Node& node_;
  Graph& graph_;
  SubgraphInferencingFunc& inferencing_func_;
  const Graph::ResolveOptions& options_;
};

// An implementation of the InferenceContext interface required by operator-specific
// shape inference for onnxruntime graphs.
class InferenceContextImpl : public ONNX_NAMESPACE::InferenceContext {
  using AttributeGraphMap = std::unordered_map<std::string, Graph*>;

 public:
  InferenceContextImpl(Node& node,
                       SubgraphInferencingFunc subgraph_inferencing_func,
                       const Graph& graph,
                       const Graph::ResolveOptions& options) noexcept
      : node_(node),
        subgraph_inferencing_func_(subgraph_inferencing_func),
        graph_(graph),
        options_(options) {
    node_output_types_.resize(node.OutputDefs().size());
  }

  void RunInferencing() {
    auto schema = node_.Op();
    if (nullptr != schema) {
      schema->GetTypeAndShapeInferenceFunction()(*this);
    }
  }

  std::vector<TypeProto> InferredOutputTypes() const { return node_output_types_; }

  const AttributeProto* getAttribute(const std::string& name) const override {
    auto& attribute_value_map = node_.GetAttributes();
    auto iter = attribute_value_map.find(name);
    if (iter == attribute_value_map.end()) {
      return nullptr;
    }
    return &iter->second;
  }

  size_t getNumInputs() const noexcept override {
    return node_.InputDefs().size();
  }

  const TypeProto* getInputType(size_t index) const override {
    const TypeProto* type = nullptr;
    auto p_node_arg = node_.InputDefs().at(index);
    if ((nullptr != p_node_arg) && p_node_arg->Exists()) {
      type = p_node_arg->TypeAsProto();
    }

    return type;
  }

  size_t getNumOutputs() const noexcept override {
    return node_output_types_.size();
  }

  TypeProto* getOutputType(size_t index) override {
    return &node_output_types_[index];
  }

  const TensorProto* getInputData(size_t index) const override {
    auto def = node_.InputDefs()[index];
    if (!def)
      return nullptr;

    // only return data if it's for a constant initializer. checks for outer scope initializers
    // if this is a subgraph and the name isn't found locally.
    const TensorProto* initializer = graph_.GetConstantInitializer(def->Name(), true);
    return initializer;
  }

  // ORT does not implement partial data propagation yet so just return nullptr.
  const TensorShapeProto* getSymbolicInput(size_t) const override {
    return nullptr;
  }

  GraphInferencer* getGraphAttributeInferencer(const std::string& attribute_name) override {
    GraphInferencer* graph_inferencer = nullptr;

    auto* subgraph = node_.GetMutableGraphAttribute(attribute_name);

    if (subgraph) {
      auto inferencer = std::make_unique<GraphInferencerImpl>(node_, *subgraph, subgraph_inferencing_func_, options_);
      graph_inferencer = inferencer.get();
      graph_inferencers_.push_back(std::move(inferencer));
    } else {
      fail_type_inference("No Graph instance was found for attribute ",
                          attribute_name, " in node ", node_.Name());
    }

    return graph_inferencer;
  }

  // XXX: When we changed and kept sparse constant initializers in sparse form,
  // we would adjust this method
  const SparseTensorProto* getInputSparseData(size_t) const override {
    return nullptr;
  }

 private:
  Node& node_;
  // node_output_types_ will be populated by the operator-specific shape inference.
  std::vector<TypeProto> node_output_types_;
  SubgraphInferencingFunc subgraph_inferencing_func_;
  std::vector<std::unique_ptr<GraphInferencerImpl>> graph_inferencers_;
  const Graph& graph_;
  const Graph::ResolveOptions& options_;
};

Status Graph::InferAndVerifySubgraphTypes(const Node& node, Graph& subgraph,
                                          const std::vector<const TypeProto*>& input_types,
                                          std::vector<const TypeProto*>& output_types,
                                          const Graph::ResolveOptions& options) {
  auto status = Status::OK();

  output_types.clear();

  // the spec says all inputs should be provided for the subgraph so default to that first
  auto* subgraph_inputs = &subgraph.GetInputsIncludingInitializers();
  auto num_subgraph_inputs = subgraph_inputs->size();

  if (num_subgraph_inputs != input_types.size()) {
    // we also allow for just the required inputs to be provided to be user friendly due to ONNX requiring
    // initializers to have matching inputs (making them optional inputs that most likely the user doesn't want to
    // override).
    auto& required_subgraph_inputs = subgraph.GetInputs();
    auto num_required_subgraph_inputs = required_subgraph_inputs.size();

    if (num_required_subgraph_inputs != input_types.size()) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Size mismatch validating subgraph inputs. Got ", input_types.size(),
                             " inputs but subgraph has ", num_subgraph_inputs,
                             " inputs and requires ", num_required_subgraph_inputs,
                             " inputs. Either provide all subgraph inputs, or just the required inputs.");
    }

    subgraph_inputs = &required_subgraph_inputs;
    num_subgraph_inputs = num_required_subgraph_inputs;
  }

  // apply type/shape info to the subgraph's inputs
  for (size_t i = 0; i < num_subgraph_inputs; ++i) {
    const auto* input_type = input_types[i];
    if (input_type == nullptr) {
      // optional input
      continue;
    }

    const auto& subgraph_input = *subgraph_inputs->at(i);

    NodeArg* mutable_nodearg = subgraph.GetNodeArg(subgraph_input.Name());
    status = mutable_nodearg->UpdateTypeAndShape(*input_type, true, options.override_types, subgraph.logger_);
    if (!status.IsOK()) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Node:", node.Name(), " ", status.ErrorMessage());
    }
  }

  // Apply any current input type/shape information to the Nodes in the subgraph that are implicitly
  // consuming NodeArg's from this scope or higher.
  // The NodeArg's that implicit_input_defs point to would have any type/shape inferencing applied to them
  // by now. As the subgraph is referring to the outer scope NodeArg, we simply replace any information in
  // the subgraph with the details from the outer scope NodeArg.
  const auto& implicit_input_defs = node.GetDefinitions().implicit_input_defs;
  for (const auto* implicit_node_arg : implicit_input_defs) {
    auto subgraph_nodearg = subgraph.GetNodeArg(implicit_node_arg->Name());

    // the implicit input defs may be for a nested subgraph, so it won't necessarily match here.
    // if that is the case, we will update the type/shape information when we descend into the
    // nested subgraph later.
    if (!subgraph_nodearg)
      continue;

    status = subgraph_nodearg->UpdateTypeAndShape(*implicit_node_arg, true, options.override_types, subgraph.logger_);
    if (!status.IsOK()) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Node:", node.Name(), " ", status.ErrorMessage());
    }

    // all values above us should have a type by now due to ONNX requirements.
    if (subgraph_nodearg->Type() == nullptr)
      return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Subgraph input missing type.");
  }

  // now that we have handled the input types, do the type/shape inferencing for the subgraph
  // to flow the type/shape info through it
  status = subgraph.PerformTypeAndShapeInferencing(options);
  ORT_RETURN_IF_ERROR(status);

  auto& subgraph_outputs = subgraph.GetOutputs();
  for (const auto* output : subgraph_outputs) {
    output_types.push_back(output->TypeAsProto());
  }

  return Status::OK();
}

Status Graph::UpdateShapeInference(Node& node) {
  // We only use this during constant folding, and we don't constant fold control flow nodes.
  ORT_ENFORCE(node.GetAttributeNameToMutableSubgraphMap().empty(),
              "UpdateTypeShapeInference is not intended to be used with control flow nodes containing subgraphs");

  // Whilst the type inferencing will run again we don't allow type overrides due to using the default
  // ResolveOptions settings, so essentially this can only change the shape information.
  return InferAndVerifyTypeMatch(node, *node.Op(), {});
}

// Implementation of type-inference and type-checking for a single node
GSL_SUPPRESS(f .23)  // spurious warning about inferred_type never being checked for null
Status Graph::InferAndVerifyTypeMatch(Node& node, const OpSchema& op, const ResolveOptions& options) {
  auto& node_name = node.Name();

  // if we're building a graph we permit outer scope node args to have no type
  // as the 'real' Resolve at runtime will have type inferencing
  auto is_outer_scope_nodearg = [this](const std::string& name) {
    return outer_scope_node_arg_names_.find(name) != outer_scope_node_arg_names_.cend();
  };

  // <k> index used to navigate node->InputDefs().
  int k = 0;
  std::unordered_map<std::string, DataType> type_parameter_to_type_map;

  for (size_t i = 0; i < node.InputArgCount().size(); ++i) {
    // Number of inputs corresponding to the i-th argument.
    const int arg_count = node.InputArgCount()[i];
    // The i-th formal parameter definition.
    auto op_formal_parameter = op.inputs()[i];

    // Check all <arg_count> actual parameters (corresponding to the k-th input)
    // match the formal parameter definition (i-th argument).
    for (int j = 0; j < arg_count; ++j, ++k) {
      const auto* input_def = node.GetDefinitions().input_defs[k];
      if (!input_def->Exists())
        continue;

      if (input_def->Type() == nullptr) {
        // if we are building a subgraph that uses outer scope values,
        // allow an empty type as it will be copied from the outer scope graph at runtime
        if (is_outer_scope_nodearg(input_def->Name()))
          continue;

        // Logic error: This should not happen if we properly checked that every use has
        // a corresponding def, for which type-inference already produced a valid type
        Status status(ONNXRUNTIME, onnxruntime::common::StatusCode::FAIL,
                      "This is an invalid model. "
                      "Node (" +
                          node_name + ") input arg (" +
                          input_def->Name() + ") does not have type information set by parent node.");
        return status;
      }

      // Verify that the actual parameter's type is one of permitted types of the formal parameter
      DataType input_type = input_def->Type();
      auto& permitted_types = op_formal_parameter.GetTypes();
      if (0 == permitted_types.count(input_type)) {
        std::string null_pointer("(null)");
        if (input_type == nullptr) input_type = &null_pointer;
        // Type error in input model/graph.

        Status status(ONNXRUNTIME, INVALID_GRAPH,
                      "This is an invalid model. "
                      "Type Error: Type '" +
                          *input_type + "' of input parameter (" + input_def->Name() +
                          ") of operator (" + op.Name() + ") in node (" + node_name + ") is invalid.");
        return status;
      }

      // When multiple parameters have the same type-variable, they are all required
      // to have the same type. E.g., when adding tensors A and B, it is an error if
      // input A is of type "tensor(int32)" and B is of type "tensor(float)".
      // For variadic arguments, this verification rule is normally applicable:
      // e.g., Concat/Max/Mean/Min/Sum all require all input tensors to be of same type.
      // However, some ops, like the control-flow constructs (Scan, If, Loop) have variadic
      // inputs and outputs of different types. The check is not applicable to such ops.
      if (op_formal_parameter.GetIsHomogeneous()) {
        auto param_to_type_iter = type_parameter_to_type_map.find(op_formal_parameter.GetTypeStr());
        if (type_parameter_to_type_map.end() == param_to_type_iter) {
          // Bind the corresponding type-parameter's value to the actual type:
          type_parameter_to_type_map[op_formal_parameter.GetTypeStr()] = input_type;
        } else if (param_to_type_iter->second != input_type) {
          // Type error in input model/graph:
          // The type-parameter T is bound to different values for different inputs.
          Status status(ONNXRUNTIME, onnxruntime::common::StatusCode::FAIL,
                        "Type Error: Type parameter (" + op_formal_parameter.GetTypeStr() +
                            ") of Optype (" + op.Name() + ") bound to different types (" +
                            *(param_to_type_iter->second) +
                            " and " + *(input_def->Type()) +
                            " in node (" + node_name + ").");
          return status;
        }
      }
    }
  }

  // Apply ONNX's type/shape inference to this node.
  // This will call InferAndVerifySubgraphTypes if the ONNX level type/shape inferencing for the Node attempts
  // to do subgraph type/shape inferencing (Scan/If/Loop nodes).
  // InferAndVerifySubgraphTypes will call PerformTypeAndShapeInferencing for the subgraph, which will recursively
  // handle type/shape inferencing for it.
  // Once that completes, the outputs from the node containing the subgraph will be updated, and the final values
  // returned here.
  SubgraphInferencingFunc func(Graph::InferAndVerifySubgraphTypes);
  InferenceContextImpl context(node, func, *this, options);

  {
    auto status = Status::OK();
    ORT_TRY {
      context.RunInferencing();
    }
    ORT_CATCH(const std::exception& ex) {
      ORT_HANDLE_EXCEPTION([&]() {
        status = ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Node (", node.Name(), ") Op (", node.OpType(), ") ", ex.what());
      });
    }
    ORT_RETURN_IF_ERROR(status);
  }

  const auto& onnx_inferred_types(context.InferredOutputTypes());

  // Infer and verify node output arg type information.
  int i = -1;
  for (auto& output_def : node.MutableDefinitions().output_defs) {
    ++i;
    if (!output_def->Exists()) continue;

    // if the number of actual parameters exceeds the number of formal parameters,
    // then the op has variadic outputs and the trailing extra actual parameters
    // correspond to the last formal parameter. (The ONNX schema verification check
    // would have checked that the corresponding formal parameter is variadic.)

    const int num_formal_params = gsl::narrow_cast<int>(op.outputs().size());
    auto operand_index = std::min(i, num_formal_params - 1);
    auto op_formal_parameter = op.outputs().at(operand_index);

    const TypeProto& onnx_inferred_type = onnx_inferred_types[i];
    DataType existing_type = output_def->Type();
    DataType inferred_type = nullptr;

    // Infer output arg type if it is constrained to be of the same type as some input:
    // For example, the output of "Abs" is of the same type as its input.
    bool homogeneous = op_formal_parameter.GetIsHomogeneous();
    auto input_types_iter = type_parameter_to_type_map.find(op_formal_parameter.GetTypeStr());
    if (homogeneous && (type_parameter_to_type_map.end() != input_types_iter)) {
      inferred_type = input_types_iter->second;
    } else if (1 == op_formal_parameter.GetTypes().size()) {
      // Infer output arg type if operator definition specifies unique output type:
      inferred_type = *(op_formal_parameter.GetTypes().begin());
    } else if (FullyDefinedType(onnx_inferred_type)) {
      // Use output type inferred by ONNX inference
      inferred_type = DataTypeUtils::ToType(onnx_inferred_type);
    } else if (existing_type != nullptr) {
      inferred_type = existing_type;
    } else {
      // This should not happen: indicates incompleteness in ONNX inference.
      std::stringstream ss;
      ss << "index=" << operand_index;
      for (auto it = op_formal_parameter.GetTypes().begin(); it != op_formal_parameter.GetTypes().end(); ++it) {
        ss << "," << *(*it);
      }
      Status status(ONNXRUNTIME, onnxruntime::common::StatusCode::FAIL,
                    "Node (" + node_name + ") Op (" + node.OpType() + ") output arg (" +
                        output_def->Name() + ") type inference failed, inferred types: " + ss.str());
      return status;
    }

    if ((existing_type != inferred_type) && (existing_type != nullptr)) {
      // A type exists for this output but does not match the inferred type.

      if (options.override_types) {
        // Replace existing type by inferred type: for use after graph-transformations
        // that change types of variables such as mixed-precision transformation.
        // Note: This reuses the original shape, with inferred type. Transformations
        // that can affect the shape are not yet supported.

        // The "SetType" call will override the shape information to empty.
        // If the original tensor has shape information, need to set it back.
        if (output_def->Shape()) {
          auto old_shape = *output_def->Shape();
          output_def->SetType(inferred_type);
          output_def->SetShape(old_shape);
        } else {
          output_def->SetType(inferred_type);
        }
      } else
        return Status(ONNXRUNTIME, onnxruntime::common::StatusCode::FAIL,
                      "Type Error: Type (" + *existing_type + ") of output arg (" +
                          output_def->Name() + ") of node (" + node_name +
                          ") does not match expected type (" + *inferred_type + ").");
    }

    if (existing_type == nullptr)
      output_def->SetType(inferred_type);

    // Update output-shape if it was inferred:
    // HasShape()/GetShape() work for tensor types
    // if the behavior changes the below may need adjustment
    if (utils::HasShape(onnx_inferred_type)) {
      if (output_def->Shape() == nullptr) {
        output_def->SetShape(utils::GetShape(onnx_inferred_type));
      } else {
        // we need to merge the shapes as a subgraph may have placeholder dimensions to represent the rank
        // that have no values.
        TypeProto merge_target;
        if (utils::HasTensorType(onnx_inferred_type)) {
          *merge_target.mutable_tensor_type()->mutable_shape() = *output_def->Shape();
        }
#if !defined(DISABLE_OPTIONAL_TYPE)
        else if (utils::HasOptionalTensorType(onnx_inferred_type)) {
          *utils::GetMutableOptionalTypeProto(merge_target)
               ->mutable_tensor_type()
               ->mutable_shape() = *output_def->Shape();
        }
#endif

#if !defined(DISABLE_SPARSE_TENSORS)
        else if (utils::HasSparseTensorType(onnx_inferred_type)) {
          *merge_target.mutable_sparse_tensor_type()->mutable_shape() = *output_def->Shape();
        }
#endif
        auto status = MergeShapeInfo(output_def->Name(), onnx_inferred_type, merge_target,
                                     strict_shape_type_inference_, logger_);
        if (!status.IsOK()) {
          return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Node:", node_name, " ", status.ErrorMessage());
        }
        // we may have cleared the shape if there was a mismatch so handle that
        if (utils::HasShape(merge_target))
          output_def->SetShape(utils::GetShape(merge_target));
        else
          output_def->ClearShape();
      }
    }
  }

  return Status::OK();
}

// Apply type-inference and type-checking to all inputs and initializers:
common::Status Graph::TypeCheckInputsAndInitializers() {
  // Check that the type of every input is specified:
  for (auto* graph_input : GetInputs()) {
    if (nullptr == graph_input->Type()) {
      Status status(ONNXRUNTIME, onnxruntime::common::StatusCode::FAIL,
                    "This is an invalid model. "
                    "Model input (" +
                        graph_input->Name() + ") does not have type information.");
      return status;
    }
  }

  // Infer/check type and shape for all initializers from their values
  for (auto& initializer_pair : name_to_initial_tensor_) {
    const std::string& name = initializer_pair.first;
    auto* node_arg = GetNodeArg(name);
    // If node_arg is null, we ignore this as a potentially unused initializer here
    if (nullptr != node_arg) {
      const TensorProto* tensor_proto = initializer_pair.second;
      TypeProto tensor_type;
      tensor_type.mutable_tensor_type()->set_elem_type(tensor_proto->data_type());
      auto initializer_type = DataTypeUtils::ToType(tensor_type);
      auto nodearg_type = node_arg->Type();
      if (nullptr == nodearg_type)
        node_arg->SetType(initializer_type);
      else if (initializer_type != nodearg_type) {
        return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL,
                               "Type Error: Data in initializer '", name, "' has element type ", *initializer_type,
                               " but usage of initializer in graph expects ", *nodearg_type);
      }

      // Set shape accordingly.
      TensorShapeProto inferred_shape;
      for (auto dim : tensor_proto->dims()) {
        inferred_shape.add_dim()->set_dim_value(dim);
      }

      const TensorShapeProto* p_existing_shape = node_arg->Shape();
      if (nullptr == p_existing_shape) {
        // use the inferred shape if this is a constant initializer (cannot be overridden).
        // if not it has a matching graph input, and we prefer the shape info (or lack of info) from the graph input
        if (GetConstantInitializer(name, false) != nullptr) {
          node_arg->SetShape(inferred_shape);
        }
      } else {
        bool invalid = false;

        if (p_existing_shape->dim_size() != tensor_proto->dims_size()) {
          invalid = true;
        } else {
          for (int i = 0; i < p_existing_shape->dim_size(); ++i) {
            auto& d = p_existing_shape->dim(i);
            if (utils::HasDimValue(d) && (d.dim_value() != tensor_proto->dims(i))) {
              invalid = true;
              break;
            }
          }
        }

        if (invalid) {
          return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL,
                                 "Type Error: Shape of initializer ", name, " does not match. ",
                                 utils::GetTensorShapeFromTensorShapeProto(*p_existing_shape), " != ",
                                 utils::GetTensorShapeFromTensorProto(*tensor_proto));
        }
      }
    }
  }

  return Status::OK();
}

Status Graph::VerifyNodeAndOpMatch(const ResolveOptions& options) {
  CheckerContext ctx;
  ctx.set_ir_version(gsl::narrow_cast<int>(IrVersion()));
  ctx.set_opset_imports(DomainToVersionMap());
  ctx.set_schema_registry(schema_registry_.get());
  // Set the parent directory of model path to load external tensors if exist
  // ONNX expects a UTF-8 string here.
  ctx.set_model_dir(ToUTF8String(ModelPath().parent_path().native()));

  LexicalScopeContext parent;
  if (parent_node_) {
    // add outer scope values. these are set as implicit inputs to the node containing the subgraph
    // in BuildConnections, which happens prior to this being called during Graph::Resolve
    const auto& outer_scope_values = parent_node_->ImplicitInputDefs();
    parent.output_names.reserve(outer_scope_values.size());

    for (const auto* implicit_inputs : outer_scope_values) {
      parent.output_names.insert(implicit_inputs->Name());
    }
  } else {
    // we may have some locally defined outer scope args if we're in the middle of constructing a subgraph
    // and need to call Resolve. parent_node_ would be null in this case
    parent.output_names.insert(outer_scope_node_arg_names_.cbegin(), outer_scope_node_arg_names_.cend());
  }

  LexicalScopeContext lsc{parent};
  lsc.output_names.reserve(resolve_context_.inputs_and_initializers.size() + resolve_context_.output_args.size());

  for (const std::string_view& input : resolve_context_.inputs_and_initializers) {
    lsc.output_names.insert(std::string(input));
  }

  for (auto node_index : nodes_in_topological_order_) {
    // Node verification.
    auto& node = *GetNode(node_index);

    const auto& node_name = node.Name();

    if (!node.Op()) {
      {
        auto status = Status::OK();
        ORT_TRY {
          // if this is first Graph::Resolve call, we may have a NodeProto that was set on the Node so we can skip
          // the ToProto call.
          if (const NodeProto* orig_node_proto = node.GetOriginalNodeProto(); orig_node_proto) {
            checker::check_node(*orig_node_proto, ctx, lsc);
            // clear original as we don't know if the node will be modified once the Graph::Resolve completes.
            node.SetOriginalNodeProto(nullptr);
          } else {
            NodeProto node_proto;
            node.ToProto(node_proto);
            checker::check_node(node_proto, ctx, lsc);
          }
        }
        ORT_CATCH(const std::exception& ex) {
          ORT_HANDLE_EXCEPTION([&]() {
            status = ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_GRAPH,
                                     "This is an invalid model. In Node, ", node, ", Error ", ex.what());
          });
        }
        ORT_RETURN_IF_ERROR(status);
      }

      SetOpSchemaFromRegistryForNode(node);

      if (!node.op_) {
        // check whether it refer to a function.
        std::string func_identifier = function_utils::GetFunctionIdentifier(node.Domain(), node.OpType());
        const auto& model_local_func_templates = owning_model_.GetModelLocalFunctionTemplates();
        auto iter = model_local_func_templates.find(func_identifier);
        if (iter != model_local_func_templates.end()) {
          // This node has a model local function proto.
          node.SetFunctionTemplate(*(iter->second));
        }
      }

      if (!node.op_) {
        return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL,
                               "Fatal error: ", (node.Domain() == kOnnxDomain ? kOnnxDomainAlias : node.Domain()), ":",
                               node.OpType(), "(", node.SinceVersion(), ") is not a registered function/op");
      }

      // For ops without schema (like model local functions set the since version after constructing the schema.
      // schema construction will happen during function body initialization.
      if (node.since_version_ == -1) {
        node.since_version_ = node.op_->since_version();
      }
    }

    ORT_RETURN_IF_ERROR(node.UpdateInputArgCount());

    // currently an Op is required by ValidateVersion, so we use gsl::not_null to validate that.
    // This may change in the future to allow a null Op
    const gsl::not_null<const OpSchema*> p_op{node.Op()};

    // Attribute verification and fill node attribute with
    // default value defined in operator definition if needed.
    // Fill node attribute with default value specified in operator definition if any.
    const auto& node_attributes = node.GetAttributes();
    for (const auto& attr_def : p_op->attributes()) {
      auto node_attr_iter = node_attributes.find(attr_def.first);
      if (node_attributes.end() == node_attr_iter) {
        // The attribute was not specified in the node.
        if (!attr_def.second.required) {
          if (utils::HasName(attr_def.second.default_value)) {
            assert(attr_def.first == attr_def.second.default_value.name());
            // Set default value to the node attributes.
            node.AddAttributeProto(attr_def.second.default_value);
          }
          // TODO: Handle optional attribute but no default value specified in op definition.
        } else {
          Status status(ONNXRUNTIME, onnxruntime::common::StatusCode::FAIL,
                        "This is an invalid model. "
                        "Node (" +
                            node_name + ") attribute (" + attr_def.first +
                            ") is required but not specified.");
          return status;
        }
      }
    }

    NO_CHANGE_ON_SYNC_FLAG(ORT_RETURN_IF_ERROR(InferAndVerifyTypeMatch(node, *p_op, options)));

    // Accumulate output names of the iterated Node
    for (const auto& output : node.OutputDefs()) {
      lsc.output_names.insert(output->Name());
    }
  }

  // verify subgraphs
  for (auto node_index : nodes_in_topological_order_) {
    auto& node = *GetNode(node_index);
    for (auto& entry : node.GetAttributeNameToMutableSubgraphMap()) {
      Graph* subgraph = entry.second;
      ORT_RETURN_IF_ERROR(subgraph->VerifyNodeAndOpMatch(options));
    }
  }

  return Status::OK();
}

Status Graph::VerifyInputAndInitializerNames() {
  std::unordered_set<std::string_view>& inputs_and_initializers = resolve_context_.inputs_and_initializers;

  const auto& graph_inputs = GetInputs();
  inputs_and_initializers.reserve(graph_inputs.size() + name_to_initial_tensor_.size());

  for (auto* input : graph_inputs) {
    auto result = inputs_and_initializers.insert(input->Name());
    if (!result.second) {
      Status status(ONNXRUNTIME, onnxruntime::common::StatusCode::FAIL,
                    "Error: Duplicate definition-site for (" + input->Name() + ").");
      return status;
    }
  }

  for (auto& initializer_pair : name_to_initial_tensor_) {
    GSL_SUPPRESS(es .84)
    inputs_and_initializers.insert(initializer_pair.first);
    // Initializers are expected to be included in inputs (according to ONNX spec).
    // onnxruntime relaxes this constraint. No duplicate-name check here.
  }

  return Status::OK();
}

Status Graph::InitInputsInitializersOutputs() {
  // clear the previous relationships, as we re-create them when resolving.
  // same applies to the implicit input defs as they are built from any subgraphs within this graph.
  for (auto& node : Nodes()) {
    node.MutableRelationships().Clear();
    node.MutableDefinitions().implicit_input_defs.clear();
  }

  // add the subgraph pointers to the resolve context.
  for (auto& node : Nodes()) {
    auto& subgraphs = node.MutableSubgraphs();
    if (!subgraphs.empty()) {
      resolve_context_.nodes_with_subgraphs.insert(&node);
    }
  }

  ORT_RETURN_IF_ERROR(SetGraphInputsOutputs());
  ORT_RETURN_IF_ERROR(VerifyInputAndInitializerNames());
  ORT_RETURN_IF_ERROR(VerifyNoDuplicateName());

  return Status::OK();
}

Status Graph::PerformTypeAndShapeInferencing(const ResolveOptions& options) {
  ORT_RETURN_IF_ERROR(TypeCheckInputsAndInitializers());

  // type/shape inferencing on the nodes is done recursively as we need subgraph outputs
  // to be applied to Node outputs for the node containing the subgraph.
  // Call path is
  // VerifyNodeAndOpMatch
  //   Iterates Nodes
  //     Runs ONNX type/shape inferencing for each Node
  //      - If it hits a node with a subgraph, InferenceContext::getGraphAttributeInferencer is called
  //        by the ONNX level type/shape inferencing, which updates the subgraph inputs using GraphInferencerImpl
  //      - GraphInferencerImpl::doInferencing calls PerformTypeShapeInferencing to execute type/shape inferencing
  //        for all nodes in the subgraph. This leads to recursively handling all subgraphs contained in the node.
  //      - once we finish processing the subgraph/s we apply resultant type/shape information to the outputs
  //        of the node that contains the subgraph.
  ORT_RETURN_IF_ERROR(VerifyNodeAndOpMatch(options));

  return Status::OK();
}

Status Graph::Resolve(const ResolveOptions& options) {
  if (parent_graph_) {
    // Resolve must start at the top level graph in-order to handle outer scope
    // connections correctly, so recurse up to that level to start
    return parent_graph_->Resolve(options);
  }

  // find all subgraphs including nested ones.
  std::vector<Graph*> all_subgraphs;
  FindAllSubgraphs(all_subgraphs);

  bool subgraphs_need_resolve = std::any_of(all_subgraphs.cbegin(), all_subgraphs.cend(),
                                            [](const Graph* graph) {
                                              return graph->GraphResolveNeeded();
                                            });

  if (!GraphResolveNeeded() && !subgraphs_need_resolve) {
    return Status::OK();
  }

  // init all graph/subgraphs. non-recursive so call via ForThisAndAllSubgraphs.
  auto init_func = [](Graph& graph) { return graph.InitInputsInitializersOutputs(); };
  ORT_RETURN_IF_ERROR(ForThisAndAllSubgraphs(all_subgraphs, init_func));

  std::unordered_set<std::string> outer_scope_node_args_consumed;

  // recursively build connections between nodes in this graph and all subgraphs
  ORT_RETURN_IF_ERROR(BuildConnections(outer_scope_node_args_consumed));
  ORT_ENFORCE(outer_scope_node_args_consumed.empty(),
              "Shouldn't be possible to have NodeArgs that haven't been handled already.");

  // topological sort of this and any subgraphs is non-recursive
  auto topo_sort_func = [](Graph& graph) { return graph.PerformTopologicalSortAndCheckIsAcyclic(); };
  ORT_RETURN_IF_ERROR(ForThisAndAllSubgraphs(all_subgraphs, topo_sort_func));

  // type/shape validation and inferencing on this and any subgraphs
  // recurses into subgraphs via the ONNX checker, which descends into the GraphProto in node attributes
  // which define a subgraph.
  ORT_RETURN_IF_ERROR(PerformTypeAndShapeInferencing(options));

  // perform the final steps for this graph and all subgraphs
  auto finalize_func = [&options](Graph& graph) {
            // we don't need the resolve context any more. call Clear first to workaround bug in
            // MSVC std::unordered_set<std::string_view>.clear() when the underlying string is invalidated.
            // this can happen to ResolveContext.inputs_and_initializers during CleanUnusedInitializersAndNodeArgs.
            graph.resolve_context_.Clear();

            graph.CleanUnusedInitializersAndNodeArgs(options.initializer_names_to_preserve);
            graph.GraphResolveNeeded(false);

            // if we are resolving immediately after loading from a GraphProto, we don't need to
            // do a proto sync
            if (options.no_proto_sync_required) {
                graph.GraphProtoSyncNeeded(false);
            }

            // set num_resolves_ here so the graph and any subgraphs all have the same value
            ++graph.num_resolves_;

            return Status::OK(); };

  ORT_RETURN_IF_ERROR(ForThisAndAllSubgraphs(all_subgraphs, finalize_func));

  auto put_weights_maybe_in_memory_func = [&](Graph& graph) -> Status {
    // if we have any initializers that are not in memory, put them there.
    const auto& model_path = graph.ModelPath();
    auto& graph_proto = *graph.graph_proto_;
    for (int i = 0, lim = graph_proto.initializer_size(); i < lim; ++i) {
      auto& tensor_proto = *graph_proto.mutable_initializer(i);
      if (utils::HasExternalData(tensor_proto)) {
        continue;  // ignore data on disk, that will be loaded either by EP or at session_state finalize
      }

      size_t size_in_bytes = 0;
      ORT_RETURN_IF_ERROR(utils::GetSizeInBytesFromTensorProto<0>(tensor_proto, &size_in_bytes));
      if (size_in_bytes > utils::kSmallTensorExternalDataThreshold) {
        OrtValue ort_value;
        ORT_RETURN_IF_ERROR(utils::TensorProtoToOrtValue(Env::Default(), model_path, tensor_proto,
                                                         CPUAllocator::DefaultInstance(), ort_value));
        constexpr const bool use_tensor_buffer_true = true;
        auto tensor_proto_to_add = utils::TensorToTensorProto(ort_value.Get<Tensor>(), tensor_proto.name(),
                                                              use_tensor_buffer_true);
        ORT_RETURN_IF_ERROR(graph.ReplaceInitializedTensor(tensor_proto_to_add, ort_value));
      }
    }
    return Status::OK();
  };

  return ForThisAndAllSubgraphs(all_subgraphs, put_weights_maybe_in_memory_func);
}

void Graph::SetName(const std::string& name) {
  graph_proto_->set_name(name);
}

void Graph::SetDescription(const std::string& description) {
  graph_proto_->set_doc_string(description);
}

bool Graph::ResolveContext::IsLocalValue(const std::string& name) const {
  return output_args.find(name) != output_args.cend() ||
         inputs_and_initializers.find(name) != inputs_and_initializers.cend();
}

bool Graph::ResolveContext::IsInputInitializerOrOutput(const std::string& name, bool check_ancestors) const {
  return IsLocalValue(name) ||
         (check_ancestors && graph.parent_graph_ &&
          graph.parent_graph_->resolve_context_.IsInputInitializerOrOutput(name, check_ancestors));
}

bool Graph::ResolveContext::IsOuterScopeValue(const std::string& name) const {
  return graph.parent_graph_ && graph.parent_graph_->resolve_context_.IsInputInitializerOrOutput(name, true);
}
#endif  // !defined(ORT_MINIMAL_BUILD)

#if !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD)

void Graph::AddInitializedTensor(const TensorProto& tensor) {
  auto existing = name_to_initial_tensor_.find(tensor.name());
  const bool exists = existing != name_to_initial_tensor_.cend();
  if (exists) {
    ORT_ENFORCE(existing->second == &tensor,
                "AddInitializedTensor already has tensor with name ", tensor.name(), " but different TensorProto.");
    return;
  }

  // This overload is used when the tensor does not point to an OrtValue which
  // would need to be updated, but it is okay if it is pointing to flatbuffers or some other place at the moment.
  // However, if an ort_value present for the name, it must be replaced.
  if (utils::HasExternalDataInMemory(tensor)) {
    if (ortvalue_initializers_.count(tensor.name()) > 0) {
      ORT_THROW("OrtValue needs to be inserted. Use the overload that takes both TensorProto and OrtValue with data");
    }
  }
  const gsl::not_null<TensorProto*> tensor_added{graph_proto_->add_initializer()};
  *(tensor_added) = tensor;
  name_to_initial_tensor_.emplace(tensor.name(), tensor_added);

  if (!is_loaded_from_model_file_ && GetNodeArg(tensor.name()) == nullptr) {
    // make sure there is a NodeArg for the initializer as SetGraphInputsOutputs may add it to the graph inputs.
    // the shape will be set to the correct value in TypeCheckInputsAndInitializers as we don't yet know whether there
    // will be a matching graph input for this initializer (we prefer shape info from the graph input).
    TypeProto t;
    t.mutable_tensor_type()->set_elem_type(tensor.data_type());
    ORT_IGNORE_RETURN_VALUE(GetOrCreateNodeArg(tensor.name(), &t));
  }

  SetGraphResolveNeeded();
}

Status Graph::AddInitializedOrtValue(const ONNX_NAMESPACE::TensorProto& tensor_proto,
                                     const OrtValue& ortvalue_initializer) {
  ORT_RETURN_IF(name_to_initial_tensor_.count(tensor_proto.name()) > 0, "Attempt to replace the existing tensor");

  const gsl::not_null<TensorProto*> tensor_added{graph_proto_->add_initializer()};
  *(tensor_added) = tensor_proto;
  name_to_initial_tensor_.emplace(tensor_proto.name(), tensor_added);

  if (ortvalue_initializer.IsAllocated()) {
    const bool has_data_in_memory = utils::HasExternalDataInMemory(tensor_proto);
    ORT_RETURN_IF_NOT(has_data_in_memory,
                      "TensorProto is expected to refer to the ortvalue_initializer");
    const auto element_type = static_cast<int32_t>(utils::GetTensorElementType(tensor_proto));
    const auto& tensor = ortvalue_initializer.Get<Tensor>();
    ORT_RETURN_IF_NOT(tensor.GetElementType() == element_type,
                      "Element type mismatch between tensor proto and ortvalue_initializer");
    const auto proto_shape = utils::GetTensorShapeFromTensorProto(tensor_proto);
    ORT_RETURN_IF_NOT(proto_shape == tensor.Shape(), "Shape mismatch with ortvalue_initializer");

    ortvalue_initializers_.insert_or_assign(tensor_proto.name(), ortvalue_initializer);
  } else {
    ORT_ENFORCE(ortvalue_initializers_.count(tensor_proto.name()) == 0,
                "Stray leftover ort_value for a small initializer being inserted.");
  }

  SetGraphResolveNeeded();
  if (!is_loaded_from_model_file_ && GetNodeArg(tensor_proto.name()) == nullptr) {
    // make sure there is a NodeArg for the initializer as SetGraphInputsOutputs may add it to the graph inputs.
    // the shape will be set to the correct value in TypeCheckInputsAndInitializers as we don't yet know whether there
    // will be a matching graph input for this initializer (we prefer shape info from the graph input).
    TypeProto t;
    t.mutable_tensor_type()->set_elem_type(tensor_proto.data_type());
    ORT_IGNORE_RETURN_VALUE(GetOrCreateNodeArg(tensor_proto.name(), &t));
  }

  return Status::OK();
}

void Graph::FindAllSubgraphs(std::vector<Graph*>& subgraphs) {
  for (auto& node : Nodes()) {
    for (auto& subgraph : node.MutableSubgraphs()) {
      subgraphs.push_back(subgraph.get());
      subgraph->FindAllSubgraphs(subgraphs);
    }
  }
}

Status Graph::ForThisAndAllSubgraphs(const std::vector<Graph*>& subgraphs, std::function<Status(Graph&)> func) {
  auto status = func(*this);
  ORT_RETURN_IF_ERROR(status);

  for (auto& subgraph : subgraphs) {
    status = func(*subgraph);
    ORT_RETURN_IF_ERROR(status);
  }

  return status;
}

Status Graph::RemovedUnusedInitializersOrtFormat() {
  std::vector<Graph*> all_subgraphs;
  FindAllSubgraphs(all_subgraphs);
  auto cleanup_func = [](Graph& graph) {
    graph.CleanUnusedInitializersAndNodeArgs(nullptr);
    return Status::OK();
  };

  auto result = ForThisAndAllSubgraphs(all_subgraphs, cleanup_func);
  return result;
}
#endif  // !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD)

const std::string& Graph::Name() const noexcept {
  return graph_proto_->name();
}

const std::string& Graph::Description() const noexcept {
  return graph_proto_->doc_string();
}

const std::filesystem::path& Graph::ModelPath() const {
  return owning_model_.ModelPath();
}

template <typename T, typename TIter>
static void RemoveRepeatedFieldEntry(T& repeated_field, const TIter& entry_to_remove) {
  auto num_entries = repeated_field.size();
  if (num_entries > 1) {
    // swap the entry being deleted with the last one, and delete it.
    // we do this so we don't have to move all the entries past the one being deleted down one.
    auto slot = entry_to_remove - repeated_field.begin();
    auto last_entry = repeated_field.end() - 1;
    repeated_field.SwapElements(narrow<int>(slot), narrow<int>(num_entries - 1));
    repeated_field.erase(last_entry);
  } else {
    repeated_field.erase(entry_to_remove);
  }
}

bool Graph::IsInitializedTensor(const std::string& name) const {
  return name_to_initial_tensor_.count(name) > 0;
}

#if !defined(DISABLE_SPARSE_TENSORS)
bool Graph::IsSparseInitializer(const std::string& name) const {
  return sparse_tensor_names_.count(name) > 0;
}
#endif

void Graph::RemoveInitializedTensor(const std::string& tensor_name) {
  bool found = false;
  auto iter = name_to_initial_tensor_.find(tensor_name);
  found = iter != name_to_initial_tensor_.end();
  if (found) {
    name_to_initial_tensor_.erase(iter);
#if !defined(DISABLE_SPARSE_TENSORS)
    sparse_tensor_names_.erase(tensor_name);
#endif

    // doesn't matter if it existed or not
    ORT_IGNORE_RETURN_VALUE(ortvalue_initializers_.erase(tensor_name));

    SetGraphResolveNeeded();
  } else {
#if !defined(DISABLE_SPARSE_TENSORS)
    ORT_ENFORCE(sparse_tensor_names_.count(tensor_name) == 0,
                "sparse_tensor_names_ not in sync with name_to_initial_tensor_");
#endif
  }

  auto& mutable_initializers = *(graph_proto_->mutable_initializer());
  auto proto_entry = std::find_if(mutable_initializers.begin(), mutable_initializers.end(),
                                  [&tensor_name](const TensorProto& entry) { return entry.name() == tensor_name; });

  if (proto_entry != mutable_initializers.end()) {
    RemoveRepeatedFieldEntry(mutable_initializers, proto_entry);
  } else {
    // these should always be in sync as the pointer in name_to_initial_tensor_ is to memory owned by graph_proto_
    ORT_ENFORCE(!found, "graph_proto_ is not in sync with name_to_initial_tensor_.");
  }
}

#if !defined(ORT_MINIMAL_BUILD)
Status Graph::ReplaceInitializedTensorImpl(ONNX_NAMESPACE::TensorProto new_initializer,
                                           OrtValue ort_value, bool must_replace_external) {
  // name_to_initial_tensor_ maps from name to const TensorProto*, so we first
  // look up the const pointer by name, then find and modify the mutable
  // pointed-to TensorProto in graph_proto_.
  const auto& initializer_name = new_initializer.name();
  const auto name_to_initializer_it = name_to_initial_tensor_.find(initializer_name);
  ORT_RETURN_IF_NOT(name_to_initializer_it != name_to_initial_tensor_.end(),
                    "Failed to find existing initializer with name ", initializer_name, ".");

  const auto& old_initializer = *(name_to_initializer_it->second);

  auto dims_eq = [&old_initializer, &new_initializer]() {
    if (old_initializer.dims_size() != new_initializer.dims_size()) return false;
    for (int i = 0; i < old_initializer.dims_size(); ++i) {
      if (old_initializer.dims(i) != new_initializer.dims(i)) return false;
    }
    return true;
  };

  // This check ensures that we are replacing the right initializer than the users wants to
  // replace data that is on disk with a reference to data in memory.
  ORT_RETURN_IF_NOT(!must_replace_external || utils::HasExternalData(old_initializer),
                    "Trying to replace non-external initializer with external data");

  // New initializers data generally are within OrtValues
  // Small initializers are still stored inside TensorProto
  ORT_RETURN_IF_NOT(utils::HasExternalDataInMemory(new_initializer) || !ort_value.IsAllocated(),
                    "All TensorProtos are expected to point to an OrtValue");

  ORT_RETURN_IF_NOT(dims_eq(), "Replacement tensor's dimensions do not match.");
  ORT_RETURN_IF_NOT(old_initializer.data_type() == new_initializer.data_type(),
                    "Replacement tensor's data type does not match.");

  bool is_sparse = false;
  {
    auto sparse_tensor_it = sparse_tensor_names_.find(initializer_name);
    if (sparse_tensor_it != sparse_tensor_names_.end()) {
      sparse_tensor_names_.erase(sparse_tensor_it);
      is_sparse = true;
    }
  }

  auto& mutable_initializers = *(graph_proto_->mutable_initializer());
  // use cheaper pointer comparison to find old entry
  auto existing_entry = std::find(mutable_initializers.pointer_begin(), mutable_initializers.pointer_end(),
                                  &old_initializer);

  // these should always be in sync as the pointer in name_to_initial_tensor_ is to memory owned by graph_proto_
  ORT_ENFORCE(existing_entry != mutable_initializers.pointer_end(),
              "graph_proto_ is not in sync with name_to_initial_tensor_");

  if (ort_value.IsAllocated()) {
    ORT_IGNORE_RETURN_VALUE(ortvalue_initializers_.insert_or_assign(initializer_name, std::move(ort_value)));
  } else {
    ORT_IGNORE_RETURN_VALUE(ortvalue_initializers_.erase(initializer_name));
  }

  **existing_entry = std::move(new_initializer);
  if (is_sparse) {
    sparse_tensor_names_.insert((**existing_entry).name());
  }

  return Status::OK();
}

common::Status Graph::ReplaceInitializedTensor(const ONNX_NAMESPACE::TensorProto& new_initializer,
                                               const OrtValue& ort_value) {
  return ReplaceInitializedTensorImpl(new_initializer, ort_value, false);
}

#if !defined(DISABLE_EXTERNAL_INITIALIZERS)
Status Graph::InjectExternalInitializedTensors(const InlinedHashMap<std::string, OrtValue>& external_initializers) {
  for (const auto& [name, value] : external_initializers) {
    const auto& user_tensor = value.Get<Tensor>();

    OrtValue ort_value;
    TensorProto tensor_proto;
    constexpr const bool use_tensor_buffer_true = true;
    if (user_tensor.SizeInBytes() > utils::kSmallTensorExternalDataThreshold) {
      if (user_tensor.OwnsBuffer()) {
        // If the user tensor has its own memory, we avoid copying
        tensor_proto = utils::TensorToTensorProto(user_tensor, name, use_tensor_buffer_true);
        ORT_ENFORCE(utils::HasExternalDataInMemory(tensor_proto), "Expecting this tensor_proto to have a pointer");
        ort_value = value;
      } else {
        Tensor initializer{user_tensor.DataType(), user_tensor.Shape(), CPUAllocator::DefaultInstance()};
        utils::MakeCpuTensorCopy(user_tensor, initializer);

        tensor_proto = utils::TensorToTensorProto(initializer, name, use_tensor_buffer_true);
        ORT_ENFORCE(utils::HasExternalDataInMemory(tensor_proto), "Expecting this tensor_proto to have a pointer");
        Tensor::InitOrtValue(std::move(initializer), ort_value);
      }
    } else {
      constexpr const bool use_tensor_buffer_false = false;
      tensor_proto = utils::TensorToTensorProto(user_tensor, name, use_tensor_buffer_false);
    }

    ORT_RETURN_IF_ERROR(ReplaceInitializedTensorImpl(std::move(tensor_proto), std::move(ort_value), true));
    LOGS(logger_, INFO) << "Replaced external initializer: " << name;
  }
  return Status::OK();
}

Status Graph::InjectExternalInitializersFromFilesInMemory(
    const InlinedHashMap<PathString, std::pair<char*, size_t>>& external_initializer_files) {
  for (const auto& [tensor_name, tensor_proto] : name_to_initial_tensor_) {
    if (utils::HasExternalDataInFile(*tensor_proto)) {
      std::unique_ptr<ExternalDataInfo> external_data_info;
      ORT_RETURN_IF_ERROR(ExternalDataInfo::Create(tensor_proto->external_data(), external_data_info));

      const auto& external_file = external_data_info->GetRelPath();
      onnxruntime::FileOffsetType file_offset = external_data_info->GetOffset();
      const size_t external_data_length = external_data_info->GetLength();
      SafeInt<size_t> tensor_byte_size;
      ORT_RETURN_IF_ERROR(utils::GetSizeInBytesFromTensorProto<0>(*tensor_proto, &tensor_byte_size));

      ORT_RETURN_IF_NOT(external_data_length == 0 || external_data_length == tensor_byte_size,
                        "TensorProto: ", tensor_name, " external data size mismatch. Computed size: ",
                        *&tensor_byte_size, ", external_data.length: ", external_data_length);

      SafeInt<FileOffsetType> end_of_read(file_offset);
      end_of_read += tensor_byte_size;

      auto user_provided_entry = external_initializer_files.find(external_file);
      ORT_RETURN_IF(user_provided_entry == external_initializer_files.end(),
                    "External file: ", ORT_TSTR_CONVERT_TO_PRINTABLE_STRING(external_file),
                    " not found from the table user provided.");

      auto user_provided_length = user_provided_entry->second.second;

      ORT_RETURN_IF(file_offset < 0 || end_of_read > narrow<FileOffsetType>(user_provided_length),
                    "External initializer: ", tensor_name,
                    " offset: ", file_offset, " size to read: ", external_data_length,
                    " given file_length: ", user_provided_length, " are out of bounds or can not be read in full.");
      char* user_provided_file_buffer = static_cast<char*>(user_provided_entry->second.first);
      char* user_provided_tensor_buffer = user_provided_file_buffer + file_offset;

      const auto& old_initializer = *(tensor_proto);
      auto& mutable_initializers = *(graph_proto_->mutable_initializer());
      // use cheaper pointer comparison to find old entry
      auto existing_entry = std::find(mutable_initializers.pointer_begin(), mutable_initializers.pointer_end(),
                                      &old_initializer);

      // these should always be in sync as the pointer in name_to_initial_tensor_ is to memory owned by graph_proto_
      ORT_ENFORCE(existing_entry != mutable_initializers.pointer_end(),
                  "graph_proto_ is not in sync with name_to_initial_tensor_");
      (**existing_entry).clear_data_location();
      const DataTypeImpl* const type =
          DataTypeImpl::TensorTypeFromONNXEnum(old_initializer.data_type())->GetElementType();
      TensorShape tensor_shape = utils::GetTensorShapeFromTensorProto(old_initializer);
      auto tensor = Tensor(type, tensor_shape, user_provided_tensor_buffer,
                           OrtMemoryInfo(CPU, OrtAllocatorType::OrtDeviceAllocator));

      constexpr const bool use_tensor_buffer_false = false;
      auto new_tensor_proto = utils::TensorToTensorProto(tensor, tensor_name, use_tensor_buffer_false);
      **existing_entry = std::move(new_tensor_proto);
    }
  }

  return Status::OK();
}

#endif  // DISABLE_EXTERNAL_INITIALIZERS
#endif  // !defined(ORT_MINIMAL_BUILD)

bool Graph::GetInitializedTensor(const std::string& tensor_name, const TensorProto*& value) const {
  auto iter = name_to_initial_tensor_.find(tensor_name);
  if (name_to_initial_tensor_.end() == iter) {
    value = nullptr;
    return false;
  }
  value = iter->second;
  return true;
}

bool Graph::GetOrtValueInitializer(const std::string& name, OrtValue& value, bool check_outer_scope) const {
  // We want to make sure that the ort_value is found on the same level as its tensor_proto
  const ONNX_NAMESPACE::TensorProto* initializer = nullptr;
  if (GetInitializedTensor(name, initializer)) {
    auto it = ortvalue_initializers_.find(name);
    if (it != ortvalue_initializers_.end()) {
      value = it->second;
      return true;
    }
  }

  if (check_outer_scope && IsSubgraph()) {
    if (IsOuterScopeValue(name)) {
      // make sure there's not a local value with the same name. if there is it shadows any initializer in outer scope.
      return parent_graph_->GetOrtValueInitializer(name, value, check_outer_scope);
    }
  }
  return false;
}

Status Graph::LoadExternalInitializerAsOrtValue(const std::string& name, OrtValue& value) const {
  auto tensor_proto_iter = name_to_initial_tensor_.find(name);
  ORT_RETURN_IF(tensor_proto_iter == name_to_initial_tensor_.end(), "Cannot load unknown initializer named '",
                name, "'.");
  const ONNX_NAMESPACE::TensorProto& tensor_proto = *tensor_proto_iter->second;

  // This only supports TensorProtos that currently have their external data in an actual file.
  // This doesn't cache the new OrtValue because it would overwrite TensorProto.external_data and plugin EPs require
  // every call to Graph::GetExternalInitializerInfo to return the actual external file info (path, offset, length).
  ORT_RETURN_IF(!utils::HasExternalDataInFile(tensor_proto), "Initializer '", name,
                "' does not have external data in a file.");

  // Create OrtValue that memory maps external initializer from file.
  ORT_RETURN_IF_ERROR(utils::GetExtDataFromTensorProto(Env::Default(), ModelPath(), tensor_proto, value));
  assert(value.IsAllocated());

  return Status::OK();
}

bool Graph::GetExternalInitializerInfo(const std::string& name, std::unique_ptr<ExternalDataInfo>& ext_info,
                                       bool check_outer_scope) const {
  // We want to make sure that the external data info is found on the same level as its tensor_proto
  const ONNX_NAMESPACE::TensorProto* initializer = nullptr;
  if (GetInitializedTensor(name, initializer)) {
    if (utils::HasExternalDataInFile(*initializer)) {
      std::unique_ptr<ExternalDataInfo> external_data_info;
      ORT_THROW_IF_ERROR(ExternalDataInfo::Create(initializer->external_data(), external_data_info));

      ext_info = std::move(external_data_info);
      return true;
    }
  }

  if (check_outer_scope && IsSubgraph()) {
    if (IsOuterScopeValue(name)) {
      // make sure there's not a local value with the same name. if there is it shadows any initializer in outer scope.
      return parent_graph_->GetExternalInitializerInfo(name, ext_info, check_outer_scope);
    }
  }
  return false;
}

void Graph::CleanAllInitializedTensors() noexcept {
  name_to_initial_tensor_.clear();
#if !defined(DISABLE_SPARSE_TENSORS)
  sparse_tensor_names_.clear();
#endif
  // Clearing RepeatedPtrFields does not free objects' memory. The memory is retained
  // and can be reused. Need to explicitly release the cleared objects and free the
  // memory.
  graph_proto_->mutable_initializer()->Clear();
#if GOOGLE_PROTOBUF_VERSION < 5026000
  const int num_cleared = graph_proto_->initializer().ClearedCount();
  for (int i = 0; i < num_cleared; i++) {
    delete graph_proto_->mutable_initializer()->ReleaseCleared();
  }
#endif

  ortvalue_initializers_.clear();
}

const ONNX_NAMESPACE::TensorProto* Graph::GetConstantInitializer(const std::string& initializer_name,
                                                                 bool check_outer_scope) const {
  const ONNX_NAMESPACE::TensorProto* initializer = nullptr;
  if (GetInitializedTensor(initializer_name, initializer)) {
    if (CanOverrideInitializer()) {
      const auto& graph_inputs = GetInputsIncludingInitializers();
      bool is_constant = std::none_of(graph_inputs.cbegin(), graph_inputs.cend(),
                                      [&initializer_name](const NodeArg* input) {
                                        return input->Name() == initializer_name;
                                      });

      if (!is_constant) {
        initializer = nullptr;
      }
    }
  } else if (check_outer_scope && IsSubgraph()) {
    // make sure there's not a local value with the same name. if there is it shadows any initializer in outer scope.
    if (IsOuterScopeValue(initializer_name)) {
      initializer = parent_graph_->GetConstantInitializer(initializer_name, check_outer_scope);
    }
  }

  return initializer;
}

const ONNX_NAMESPACE::TensorProto* Graph::GetInitializer(const std::string& initializer_name,
                                                         bool check_outer_scope) const {
  const ONNX_NAMESPACE::TensorProto* initializer = nullptr;
  if (GetInitializedTensor(initializer_name, initializer)) {
    return initializer;
  } else if (check_outer_scope && IsSubgraph()) {
    // make sure there's not a local value with the same name. if there is it shadows any initializer in outer scope.
    if (IsOuterScopeValue(initializer_name)) {
      initializer = parent_graph_->GetInitializer(initializer_name, check_outer_scope);
    }
  }

  return initializer;
}

const ONNX_NAMESPACE::TensorProto* Graph::GetInitializer(const std::string& initializer_name, OrtValue& value,
                                                         bool& is_constant, bool check_outer_scope) const {
  const ONNX_NAMESPACE::TensorProto* initializer = nullptr;
  if (GetInitializedTensor(initializer_name, initializer)) {
    if (CanOverrideInitializer()) {
      const auto& graph_inputs = GetInputsIncludingInitializers();
      is_constant = std::none_of(graph_inputs.cbegin(), graph_inputs.cend(),
                                 [&initializer_name](const NodeArg* input) {
                                   return input->Name() == initializer_name;
                                 });
    } else {
      is_constant = true;
    }

    auto it = ortvalue_initializers_.find(initializer_name);
    value = (it != ortvalue_initializers_.end()) ? it->second : OrtValue();
  } else if (check_outer_scope && IsSubgraph()) {
    // make sure there's not a local value with the same name. if there is it shadows any initializer in outer scope.
    if (IsOuterScopeValue(initializer_name)) {
      initializer = parent_graph_->GetInitializer(initializer_name, value, is_constant, check_outer_scope);
    }
  }

  return initializer;
}

#if !defined(ORT_MINIMAL_BUILD)
void Graph::AddValueInfo(const NodeArg* new_value_info) {
  NodeArg* node_arg = GetNodeArg(new_value_info->Name());
  ORT_ENFORCE(node_arg && node_arg == new_value_info, "Error: trying to add an value info that are no in graph.");
  value_info_.insert(new_value_info);
}

template <typename StringRange>
std::vector<NodeArg*> Graph::CreateNodeArgs(const StringRange& names,
                                            const ArgNameToTypeMap& name_to_type_map) {
  const auto name_to_type_map_end = name_to_type_map.end();
  std::vector<NodeArg*> results;
  results.reserve(names.size());

  for (const std::string& name : names) {
    const TypeProto* type = nullptr;

    auto name_to_type_iter = name_to_type_map.find(name);
    if (name_to_type_iter != name_to_type_map_end) {
      // This node input arg type/shape does exist in graph proto.
      // Assign type/shape information to node input arg.
      type = &(name_to_type_iter->second);
    }

    auto node_arg = &GetOrCreateNodeArg(name, type);
    results.push_back(node_arg);
  }

  return results;
}

Node& Graph::AddNode(const Node& other) {
  const auto& definitions = other.GetDefinitions();

  auto& new_node = AddNode(other.Name(), other.OpType(), other.Description(),
                           definitions.input_defs,
                           definitions.output_defs,
                           &other.GetAttributes(),
                           other.Domain());

  return new_node;
}

Node& Graph::AddNode(const NodeProto& node_proto,
                     const ArgNameToTypeMap& name_to_type_map) {
  auto input_defs = CreateNodeArgs(node_proto.input(), name_to_type_map);
  auto output_defs = CreateNodeArgs(node_proto.output(), name_to_type_map);

  const int num_attributes = node_proto.attribute_size();
  NodeAttributes attributes;
  attributes.reserve(num_attributes);

  for (int i = 0; i < num_attributes; ++i) {
    auto& attr = node_proto.attribute(i);
    attributes[attr.name()] = attr;
  }

  Node& new_node = AddNode(node_proto.name(),
                           node_proto.op_type(),
                           node_proto.doc_string(),
                           input_defs,
                           output_defs,
                           &attributes,
                           node_proto.domain());

  // Perf optimization: temporarily set NodeProto in Node so we don't need to call Node::ToProto prior to
  // calling onnx::check_node
  // NOTE: We don't handle a node with kOnnxDomainAlias. The entry in schema_registry_ uses kOnnxDomain,
  // and that's what onnx::check_node uses during validation.
  // The Node ctor automatically converts kOnnxDomainAlias to kOnnxDomain to handle this.
  // node_proto is const so we can't do the same here.
  if (node_proto.domain() != kOnnxDomainAlias) {
    new_node.SetOriginalNodeProto(&node_proto);
  }

  return new_node;
}

static flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>>
SaveInputsOutputsToOrtFormat(flatbuffers::FlatBufferBuilder& builder, const std::vector<const NodeArg*>& src) {
  std::vector<flatbuffers::Offset<flatbuffers::String>> vec(src.size());
  std::transform(src.cbegin(), src.cend(), vec.begin(),
                 [&builder](const NodeArg* entry) {
                   return builder.CreateSharedString(entry->Name());
                 });
  return builder.CreateVector(vec);
}

common::Status Graph::SaveToOrtFormat(flatbuffers::FlatBufferBuilder& builder,
                                      flatbuffers::Offset<fbs::Graph>& fbs_graph) const {
  auto inputs = SaveInputsOutputsToOrtFormat(builder, graph_inputs_including_initializers_);
  auto outputs = SaveInputsOutputsToOrtFormat(builder, graph_outputs_);

#if !defined(DISABLE_SPARSE_TENSORS)
  std::vector<flatbuffers::Offset<fbs::SparseTensor>> sparse_initializers_data;
  sparse_initializers_data.reserve(sparse_tensor_names_.size());
#endif
  const auto sparse_end = sparse_tensor_names_.end();

  std::vector<flatbuffers::Offset<fbs::Tensor>> initializers_data;
#if !defined(DISABLE_SPARSE_TENSORS)
  assert(sparse_tensor_names_.size() <= name_to_initial_tensor_.size());
  initializers_data.reserve(name_to_initial_tensor_.size() - sparse_tensor_names_.size());
#else
  initializers_data.reserve(name_to_initial_tensor_.size());
#endif
  const auto& model_path = ModelPath();

  for (const auto& pair : name_to_initial_tensor_) {
    if (sparse_tensor_names_.find(pair.first) == sparse_end) {
      flatbuffers::Offset<fbs::Tensor> fbs_tensor;
      ORT_RETURN_IF_ERROR(
          fbs::utils::SaveInitializerOrtFormat(builder, *pair.second, model_path, fbs_tensor));
      initializers_data.push_back(fbs_tensor);
    }
#if !defined(DISABLE_SPARSE_TENSORS)
    else {
      SparseTensorProto sparse_initializer;
      ORT_RETURN_IF_ERROR(utils::DenseTensorToSparseTensorProto(*pair.second, model_path, sparse_initializer));
      flatbuffers::Offset<fbs::SparseTensor> fbs_sparse_tensor;
      ORT_RETURN_IF_ERROR(
          fbs::utils::SaveSparseInitializerOrtFormat(builder, sparse_initializer, model_path, fbs_sparse_tensor));
      sparse_initializers_data.push_back(fbs_sparse_tensor);
    }
#endif
  }
#if !defined(DISABLE_SPARSE_TENSORS)
  auto sparse_initializers = builder.CreateVector(sparse_initializers_data);
#endif
  auto initializers = builder.CreateVector(initializers_data);

  std::vector<flatbuffers::Offset<fbs::ValueInfo>> node_args_data;
  node_args_data.reserve(node_args_.size());
  for (const auto& pair : node_args_) {
    flatbuffers::Offset<fbs::ValueInfo> fbs_val_info;
    ORT_RETURN_IF_ERROR(
        fbs::utils::SaveValueInfoOrtFormat(builder, pair.second->ToProto(), fbs_val_info));
    node_args_data.push_back(fbs_val_info);
  }
  auto node_args = builder.CreateVector(node_args_data);

  std::vector<flatbuffers::Offset<fbs::Node>> nodes_vec;
  std::vector<flatbuffers::Offset<fbs::NodeEdge>> node_edges_vec;
  node_edges_vec.reserve(nodes_.size());
  for (const auto& node : nodes_) {
    if (node != nullptr) {
      flatbuffers::Offset<fbs::Node> fbs_node;
      ORT_RETURN_IF_ERROR(node->SaveToOrtFormat(builder, fbs_node));
      nodes_vec.push_back(fbs_node);
      node_edges_vec.push_back(node->SaveEdgesToOrtFormat(builder));
    }
  }
  auto nodes = builder.CreateVector(nodes_vec);
  auto node_edges = builder.CreateVector(node_edges_vec);

#if !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD)
  auto runtime_optimizations = flatbuffers::Offset<fbs::RuntimeOptimizations>{};  // null value
  if (!RuntimeOptimizations().IsEmpty()) {
    flatbuffers::Offset<RuntimeOptimizationRecordContainer::FbsRuntimeOptimizationRecordContainer>
        runtime_optimization_records;
    ORT_RETURN_IF_ERROR(RuntimeOptimizations().SaveToOrtFormat(builder, runtime_optimization_records));
    runtime_optimizations = fbs::CreateRuntimeOptimizations(builder, runtime_optimization_records);
  }
#endif

  fbs::GraphBuilder gb(builder);
  gb.add_initializers(initializers);
  gb.add_node_args(node_args);
  gb.add_nodes(nodes);
  gb.add_max_node_index(gsl::narrow_cast<uint32_t>(nodes_.size()));
  gb.add_node_edges(node_edges);
  gb.add_inputs(inputs);
  gb.add_outputs(outputs);
#if !defined(DISABLE_SPARSE_TENSORS)
  gb.add_sparse_initializers(sparse_initializers);
#endif
#if !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD)
  gb.add_runtime_optimizations(runtime_optimizations);
#endif
  fbs_graph = gb.Finish();
  return Status::OK();
}
#endif  // !defined(ORT_MINIMAL_BUILD)

#if !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD)
std::string Graph::GenerateNodeArgName(const std::string& base_name) {
  std::string new_name = base_name;
  // Check if new_name has been used in as any of node_args_' names.
  // Check if new_name has been generated by this function.
  // If both are not, add new_name into name set and return the new_name
  // as the generated name. Otherwise, keep generating new names.
  while (node_args_.find(new_name) != node_args_.end() ||
         generated_node_arg_names_.find(new_name) != generated_node_arg_names_.end()) {
    std::ostringstream str;
    str << base_name << "_token_" << name_generator_++;
    new_name = str.str();
  }

  generated_node_arg_names_.insert(new_name);
  return new_name;
}

std::string Graph::GenerateNodeName(const std::string& base_name) {
  // Define name-checking function for node name.
  // Return true if the input name hasn't been used. Otherwise, return false.
  auto name_is_ok = [&](const std::string name) {
    for (auto it = nodes_.begin(); it != nodes_.end(); ++it) {
      if (*it == nullptr) {
        continue;
      }
      if (it->get()->Name() != name) {
        continue;
      }
      // Find a matched name so we cannot reuse the input name.
      return false;
    }

    if (generated_node_names_.find(name) != generated_node_names_.end()) {
      // Find a matched name so we cannot reuse the input name.
      return false;
    }

    // The input name can be reused.
    return true;
  };

  // Start with the input name.
  std::string new_name = base_name;

  while (!name_is_ok(new_name)) {
    std::ostringstream str;
    str << base_name << "_token_" << name_generator_++;
    new_name = str.str();
  }

  // Make sure this new_name is not going to be reused.
  generated_node_names_.insert(new_name);

  return new_name;
}

Node& Graph::AddNode(const std::string& name,
                     const std::string& op_type,
                     const std::string& description,
                     gsl::span<NodeArg* const> input_args,
                     gsl::span<NodeArg* const> output_args,
                     const NodeAttributes* attributes,
                     const std::string& domain) {
  InlinedVector<NodeArg*> inputs;
  InlinedVector<NodeArg*> outputs;
  inputs.resize(input_args.size());
  outputs.resize(output_args.size());
  int i = 0;
  for (auto input_arg : input_args) {
    inputs[i++] = &GetOrCreateNodeArg(input_arg->Name(), input_arg->TypeAsProto());
  }
  i = 0;
  for (auto output_arg : output_args) {
    outputs[i++] = &GetOrCreateNodeArg(output_arg->Name(), output_arg->TypeAsProto());
  }

  const gsl::not_null<Node*> node = AllocateNode();
  node->Init(name, op_type, description, inputs, outputs, attributes, domain);
  if (0 != op_type.compare(kNoOp)) {
    GraphProtoSyncNeeded(true);
  }

  return *node;
}

Node& Graph::AddNode(const std::string& name,
                     const std::string& op_type,
                     const std::string& description,
                     gsl::span<NodeArg* const> input_args,
                     gsl::span<NodeArg* const> output_args,
                     NodeAttributes&& attributes,
                     const std::string& domain) {
  InlinedVector<NodeArg*> inputs;
  InlinedVector<NodeArg*> outputs;
  inputs.resize(input_args.size());
  outputs.resize(output_args.size());
  int i = 0;
  for (auto input_arg : input_args) {
    inputs[i++] = &GetOrCreateNodeArg(input_arg->Name(), input_arg->TypeAsProto());
  }
  i = 0;
  for (auto output_arg : output_args) {
    outputs[i++] = &GetOrCreateNodeArg(output_arg->Name(), output_arg->TypeAsProto());
  }

  const gsl::not_null<Node*> node = AllocateNode();
  node->Init(name, op_type, description, inputs, outputs, std::move(attributes), domain);
  if (0 != op_type.compare(kNoOp)) {
    GraphProtoSyncNeeded(true);
  }

  return *node;
}

bool Graph::RemoveNode(NodeIndex p_index) {
  auto node = GetNode(p_index);
  if (nullptr == node) {
    return false;
  }

  // Node must be disconnected from any downstream nodes before removal
  ORT_ENFORCE(node->GetOutputEdgesCount() == 0, "Can't remove node ", node->Name(), " as it still has output edges.");

  // Remove all input edges.
  // Need to copy the edge info first so we can remove the real edges while iterating the copy of edge info.
  auto input_edges = node->GetRelationships().input_edges;

  for (auto& input_edge : input_edges) {
    RemoveEdge(input_edge.GetNode().Index(), p_index, input_edge.GetSrcArgIndex(), input_edge.GetDstArgIndex());
  }

  return ReleaseNode(p_index);
}
#endif  // !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD)

#if !defined(ORT_MINIMAL_BUILD)
bool Graph::AddControlEdge(NodeIndex src_node_index, NodeIndex dst_node_index) {
  if (nodes_.size() <= src_node_index ||
      nodes_.size() <= dst_node_index ||
      nullptr == nodes_[src_node_index] ||
      nullptr == nodes_[dst_node_index]) {
    // Invalid node indexes specified.
    return false;
  }

  GSL_SUPPRESS(es.84) {  // ignoring return from insert()
    nodes_[src_node_index]->MutableRelationships().output_edges.insert(Node::EdgeEnd(*nodes_[dst_node_index]));
    nodes_[dst_node_index]->MutableRelationships().input_edges.insert(Node::EdgeEnd(*nodes_[src_node_index]));
    nodes_[dst_node_index]->MutableRelationships().control_inputs.insert(nodes_[src_node_index]->Name());
  }

  return true;
}

const ONNX_NAMESPACE::GraphProto& Graph::ToGraphProto() {
  if (!GraphProtoSyncNeeded()) {
    return *graph_proto_;
  }

  // Nodes.
  ToGraphProtoInternal(*graph_proto_);

  GraphProtoSyncNeeded(false);

  return *graph_proto_;
}

namespace {
// The function examines initializer tensor_proto and if the data is in memory it inlines
// and adds it to the output initializers. Otherwise, it copies the initializer.
// External data on disk references are preserved.
Status InlineOrCopyInitializer(const Graph& src_graph, const ONNX_NAMESPACE::TensorProto& initializer,
                               ONNX_NAMESPACE::TensorProto& output) {
  // copy any in-memory external data into raw data
  if (utils::HasExternalDataInMemory(initializer)) {
    OrtValue ort_value;
    ORT_RETURN_IF_NOT(src_graph.GetOrtValueInitializer(initializer.name(), ort_value, /*check_outer_scope=*/false),
                      "Unable to find OrtValue for initializer with data in memory");
    ORT_RETURN_IF_NOT(graph_utils::CheckInMemoryDataMatch(initializer, ort_value.Get<Tensor>()),
                      "Unable to match OrtValue for initializer with data in memory");
    output = utils::TensorToTensorProto(ort_value.Get<Tensor>(),
                                        initializer.name(), /* use_tensor_buffer */ false);
  } else {
    output = initializer;
  }
  return Status::OK();
}

}  // namespace

Status Graph::ProcessSubgraphsInMemoryData(ONNX_NAMESPACE::GraphProto& output_graph_proto,
                                           bool process_main) const {
  for (const auto& node : Nodes()) {
    if (node.ContainsSubgraph()) {
      // Let's find this node in the output_graph_proto
      // The node name is optional, so we may need to check by the output value name
      // given that they can only assigned once.
      auto hit = std::find_if(output_graph_proto.mutable_node()->begin(),
                              output_graph_proto.mutable_node()->end(),
                              [&node](const ONNX_NAMESPACE::NodeProto& proto) {
                                const auto& node_name = node.Name();
                                if (!node_name.empty())
                                  return proto.name() == node_name;
                                return (proto.output_size() > 0 &&
                                        proto.output(0) == node.OutputDefs()[0]->Name());
                              });
      ORT_RETURN_IF_NOT(hit != output_graph_proto.mutable_node()->end(), "Node ", node.Name(),
                        " not found in output_graph_proto");
      auto& result_node = *hit;
      for (const auto& e : node.GetAttributeNameToSubgraphMap()) {
        const auto& name = e.first;
        const auto& subgraph = e.second;
        // Lets find this subgraph in the result_node
        auto sub_hit = std::find_if(result_node.mutable_attribute()->begin(),
                                    result_node.mutable_attribute()->end(),
                                    [&name](const ONNX_NAMESPACE::AttributeProto& proto) {
                                      return proto.name() == name;
                                    });
        ORT_RETURN_IF_NOT(sub_hit != result_node.mutable_attribute()->end() && utils::HasGraph(*sub_hit),
                          "Subgraph ", name, " is referred to in GetAttributeNameToSubgraphMap, but not found in node ",
                          node.Name(), " while attempting to recurse into it.");
        auto& result_subgraph = *sub_hit->mutable_g();
        ORT_RETURN_IF_ERROR(subgraph->ProcessSubgraphsInMemoryData(result_subgraph, process_main));
      }
    }
  }

  // When graph_proto is copied from graph_proto, initializers already present in the main graph
  if (parent_graph_ != nullptr || process_main) {
#if !defined(DISABLE_SPARSE_TENSORS)
    auto* mutable_initializers = output_graph_proto.mutable_initializer();
    const auto& model_path = ModelPath();
    const bool has_sparse_initializers = !sparse_tensor_names_.empty();
    const auto sparse_end = sparse_tensor_names_.end();

    // We want to make sure that sparse initializers do not appear
    // as dense duplicates within the initializers list.
    std::optional<InlinedHashSet<std::string>> initializer_to_remove;
    if (has_sparse_initializers) {
      // We need to remove the dense initializers that are sparse tensors
      initializer_to_remove.emplace();
    }

    for (auto first = mutable_initializers->begin(), end = mutable_initializers->end(); first != end; ++first) {
      auto& initializer = *first;
      if (utils::HasExternalDataInMemory(initializer)) {
        // If the initializer has external data in memory, we need to inline it.
        ORT_RETURN_IF_ERROR(InlineOrCopyInitializer(*this, initializer, initializer));
      }
      if (has_sparse_initializers && sparse_end != sparse_tensor_names_.find(initializer.name())) {
        auto& sparse_initializer = *output_graph_proto.add_sparse_initializer();
        ORT_RETURN_IF_ERROR(utils::DenseTensorToSparseTensorProto(initializer, model_path, sparse_initializer));
        initializer_to_remove->insert(initializer.name());
      }
    }

    // erase/remove dense initializers that are sparse tensors so no duplicates are present
    if (initializer_to_remove && !initializer_to_remove->empty()) {
      mutable_initializers->erase(std::remove_if(
                                      mutable_initializers->begin(), mutable_initializers->end(),
                                      [&initializer_to_remove](const ONNX_NAMESPACE::TensorProto& initializer) {
                                        return initializer_to_remove->count(initializer.name()) > 0;
                                      }),
                                  mutable_initializers->end());
    }
#else
    for (auto& initializer : *output_graph_proto.mutable_initializer()) {
      if (utils::HasExternalDataInMemory(initializer)) {
        // If the initializer has external data in memory, we need to inline it.
        ORT_RETURN_IF_ERROR(InlineOrCopyInitializer(*this, initializer, initializer));
      }
    }
#endif
  }
  return Status::OK();
}

ONNX_NAMESPACE::GraphProto Graph::ToGraphProto() const {
  GraphProto result;
  if (!GraphProtoSyncNeeded()) {
    result = *graph_proto_;
    ORT_THROW_IF_ERROR(ProcessSubgraphsInMemoryData(result, /*process_main*/ true));
  } else {
    ToGraphProtoInternal(result);

    ORT_THROW_IF_ERROR(ProcessSubgraphsInMemoryData(result, /*process_main*/ false));

    // Add initializers to parent graph by copy converting them from graph_proto_
    // ToGraphProtoInternal() does not copy initializers for the main graph
    auto* mutable_initializers = result.mutable_initializer();

#if !defined(DISABLE_SPARSE_TENSORS)
    const auto& model_path = ModelPath();
    const bool has_sparse_initializers = !sparse_tensor_names_.empty();
    const auto sparse_end = sparse_tensor_names_.end();

    for (const auto& initializer : graph_proto_->initializer()) {
      if (!has_sparse_initializers || sparse_end == sparse_tensor_names_.find(initializer.name())) {
        ORT_THROW_IF_ERROR(InlineOrCopyInitializer(*this, initializer,
                                                   *mutable_initializers->Add()));
      } else {
        auto& sparse_initializer = *result.add_sparse_initializer();
        if (utils::HasExternalDataInMemory(initializer)) {
          ONNX_NAMESPACE::TensorProto tensor_proto;
          ORT_THROW_IF_ERROR(InlineOrCopyInitializer(*this, initializer,
                                                     tensor_proto));
          ORT_THROW_IF_ERROR(utils::DenseTensorToSparseTensorProto(tensor_proto, model_path, sparse_initializer));
        } else {
          ORT_THROW_IF_ERROR(utils::DenseTensorToSparseTensorProto(initializer, model_path, sparse_initializer));
        }
      }
    }
#else
    for (const auto& initializer : graph_proto_->initializer()) {
      ORT_THROW_IF_ERROR(InlineOrCopyInitializer(*this, initializer, *mutable_initializers->Add()));
    }
#endif
  }

  return result;
}

Status Graph::AddExternalInitializersToGraphProtoImpl(
    const std::filesystem::path& model_path,
    const std::filesystem::path& external_file_path,
    const std::filesystem::path& model_external_file_path,
    const ModelSavingOptions& model_saving_options,
    ONNX_NAMESPACE::GraphProto& output_graph_proto,
    std::ostream& external_stream,
    int64_t& external_offset) const {
  // Process initializers in a subgraph, check their size and
  // write to an external file. This function also saves pre-packed
  // blobs for the initializer being saved to disk, if the initializer has any pre-packs.
  // This function is invoked by ToGraphProtoWithExternalInitiallizers() and processes subgraphs
  // bottom up.
  for (const auto& node : Nodes()) {
    if (node.ContainsSubgraph()) {
      // Let's find this node in the output_graph_proto
      // The node name is optional, so we may need to check by the output value name
      // given that they can only assigned once.
      auto hit = std::find_if(output_graph_proto.mutable_node()->begin(),
                              output_graph_proto.mutable_node()->end(),
                              [&node](const ONNX_NAMESPACE::NodeProto& proto) {
                                const auto& node_name = node.Name();
                                if (!node_name.empty())
                                  return proto.name() == node_name;
                                return (proto.output_size() > 0 &&
                                        proto.output(0) == node.OutputDefs()[0]->Name());
                              });
      ORT_RETURN_IF_NOT(hit != output_graph_proto.mutable_node()->end(), "Node ", node.Name(),
                        " not found in output_graph_proto");
      auto& result_node = *hit;
      for (const auto& e : node.GetAttributeNameToSubgraphMap()) {
        const auto& name = e.first;
        const auto& subgraph = e.second;
        // Lets find this subgraph in the result_node
        auto sub_hit = std::find_if(result_node.mutable_attribute()->begin(),
                                    result_node.mutable_attribute()->end(),
                                    [&name](const ONNX_NAMESPACE::AttributeProto& proto) {
                                      return proto.name() == name;
                                    });
        ORT_RETURN_IF_NOT(sub_hit != result_node.mutable_attribute()->end() && utils::HasGraph(*sub_hit),
                          "Subgraph ", name, " is referred to in GetAttributeNameToSubgraphMap, but not found in node ",
                          node.Name(), " while attempting to recurse into it.");
        auto& result_subgraph = *sub_hit->mutable_g();
        ORT_RETURN_IF_ERROR(subgraph->AddExternalInitializersToGraphProtoImpl(
            model_path, external_file_path,
            model_external_file_path, model_saving_options,
            result_subgraph,
            external_stream, external_offset));
      }
    }
  }

  // Used only when pre-packed weights are serialized
  InlinedHashSet<std::string> processed_weights;
  // prepacked_weights_for_graph_ is present only when SessionState is finalized.
  const bool process_prepacks = prepacked_weights_for_graph_.has_value() &&
                                prepacked_weights_for_graph_->GetNumberOfWeightsForWriting() > 0;
  if (process_prepacks) {
    processed_weights.reserve(graph_proto_->initializer_size());
  }

  // Add the initializers to the result graph.
  for (const auto& initializer : graph_proto_->initializer()) {
#if !defined(DISABLE_SPARSE_TENSORS)
    if (IsSparseInitializer(initializer.name())) {
      // Sparse tensors are added to the ONNX file.
      auto& sparse_initializer = *output_graph_proto.add_sparse_initializer();
      auto status = utils::DenseTensorToSparseTensorProto(initializer, model_path, sparse_initializer);
      ORT_RETURN_IF_NOT(status.IsOK(), "Failed to convert dense initializer to sparse");
    } else {
#endif

      // We do not externalize string tensors.
      if (utils::HasString(initializer)) {
        ORT_RETURN_IF_ERROR(InlineOrCopyInitializer(
            *this, initializer,
            *output_graph_proto.mutable_initializer()->Add()));
        continue;
      }

      // Dense tensors larger than the threshold are added to the external file.
      TensorProto* output_proto = output_graph_proto.add_initializer();
      std::vector<uint8_t> raw_data;
      ORT_RETURN_IF_ERROR(utils::UnpackInitializerData(initializer, model_path, raw_data));
      size_t tensor_bytes_size = raw_data.size();
      if (model_saving_options.force_embed_external_ini ||
          tensor_bytes_size < model_saving_options.initializer_size_threshold) {
        *output_proto = initializer;
        // Data with size above the threshold is written into the new external initializer file
        // Data with size below the threshold should be kept inside the new model file
        // instead of leaving it in the old external initializer file for the old Onnx file
        if (utils::HasExternalData(initializer)) {
          output_proto->set_raw_data(raw_data.data(), raw_data.size());
          output_proto->clear_data_location();
          output_proto->clear_external_data();
        }
        if (process_prepacks) {
          // These pre-packs will reside in memory
          processed_weights.insert(initializer.name());
        }
        continue;
      }

      // update external_offset for alignment
      // need to do padding before write actual tensor data as we do offset alignment at the begin of
      // large tensors (offset need to be page aligned and allocation granularity aligned) like below:
      // \242\2557\256\023.\031&0000000000000000\332)k+\253\246\342\246(&\006!\347\232\374\236\325\026\032+\36XXXX
      // |<---smaller tensor---->|<---padding--->|<------------------large tensor----------------------------->|
      if (model_saving_options.align_offset && static_cast<int64_t>(tensor_bytes_size) >
                                                   model_saving_options.align_threshold) {
        ORT_RETURN_IF_NOT(ExternalDataInfo::AlignAndPad(external_stream, model_saving_options.allocation_granularity,
                                                        external_offset),
                          "Failed writing external data to: ", model_external_file_path);
      }

      ORT_RETURN_IF_NOT(external_stream.write(reinterpret_cast<const char*>(raw_data.data()), tensor_bytes_size),
                        "Failed to write external initializers to file: ", model_external_file_path);

      output_proto->set_name(initializer.name());
      output_proto->set_data_type(initializer.data_type());
      output_proto->mutable_dims()->CopyFrom(initializer.dims());
      output_proto->set_doc_string(initializer.doc_string());

      ExternalDataInfo::SetExternalLocationToProto(external_file_path, external_offset,
                                                   tensor_bytes_size, *output_proto);

      external_offset = SafeInt<int64_t>(external_offset) + tensor_bytes_size;

      if (process_prepacks) {
        // check if this weight was referred to in subgraphs
        InlinedHashSet<std::string> blob_keys_to_external_data;

        // See if this weight has any pre-prepacks referred to in this graph.
        const auto* blobs_keys_for_weight = prepacked_weights_for_graph_->GetKeysForWeightForSaving(initializer.name());
        if (blobs_keys_for_weight != nullptr && !blobs_keys_for_weight->empty()) {
          // Add all the blob_keys to the set of keys to process
          blob_keys_to_external_data.insert(blobs_keys_for_weight->begin(), blobs_keys_for_weight->end());
        }

        if (!blob_keys_to_external_data.empty()) {
          auto& os = ExternalDataInfo::WritePrepackedToFileAndAddToProto(
              *prepacked_weights_for_graph_, blob_keys_to_external_data,
              model_saving_options.align_offset, model_saving_options.align_threshold,
              model_saving_options.allocation_granularity,
              external_stream, external_offset, *output_proto);
          ORT_RETURN_IF_NOT(os.good(), "Failed to write pre-packed blobs to external file");
        }

        processed_weights.insert(initializer.name());
      }

#if !defined(DISABLE_SPARSE_TENSORS)
    }
#endif
  }

  // Check if there are any pre-packed weights this graph refers to, but they have
  // not been processed.
  if (process_prepacks) {
    const auto& sorted_by_weights = prepacked_weights_for_graph_->GetWeightToPrepack();
    for (const auto& [weight_name, blob_keys] : sorted_by_weights) {
      ORT_ENFORCE(processed_weights.find(weight_name) != processed_weights.end());
    }
  }

  return Status::OK();
}

ONNX_NAMESPACE::GraphProto Graph::ToGraphProtoWithExternalInitializers(
    const std::filesystem::path& external_file_path,
    const std::filesystem::path& model_file_path,
    const ModelSavingOptions& model_saving_options) const {
  GraphProto result;
  ToGraphProtoInternal(result);
  ORT_ENFORCE(external_file_path.is_relative());
  // If model_file_path is just a file name without a path separator, for example: "model.onnx". Its parent path could
  // be empty. Else, save external data file in same directory as the model.
  const std::filesystem::path modified_external_file_path = model_file_path.parent_path() / external_file_path;
  const auto& model_path = ModelPath();

  std::ofstream external_stream;
  std::streampos external_empty_pos;
  int64_t external_offset = 0;
  if (!model_saving_options.force_embed_external_ini) {
    // Create the external file.
    external_stream.open(modified_external_file_path, std::ofstream::out | std::ofstream::binary);
    external_empty_pos = external_stream.tellp();
    ORT_ENFORCE(external_stream.is_open(), "Failed to open for writing:", modified_external_file_path);
  }

  ORT_THROW_IF_ERROR(AddExternalInitializersToGraphProtoImpl(model_path, external_file_path,
                                                             modified_external_file_path, model_saving_options,
                                                             result,
                                                             external_stream, external_offset));

  if (!model_saving_options.force_embed_external_ini) {
    if (!external_stream.flush()) {
      ORT_THROW("Failed to flush file with external initializers: ", modified_external_file_path);
    }

    // Delete if the external data file is empty
    if (external_empty_pos == external_stream.tellp()) {
      external_stream.close();
      std::remove(modified_external_file_path.string().c_str());
    }
  }

  return result;
}

void Graph::ToGraphProtoInternal(ONNX_NAMESPACE::GraphProto& graph_proto) const {
  graph_proto_->clear_node();
  graph_proto_->clear_input();
  graph_proto_->clear_output();
  graph_proto_->clear_value_info();
  graph_proto.set_name(Name());
  graph_proto.set_doc_string(Description());

  for (const auto* input_arg : GetInputsIncludingInitializers()) {
    *(graph_proto.mutable_input()->Add()) = input_arg->ToProto();
  }

  for (const auto* output_arg : GetOutputs()) {
    *(graph_proto.mutable_output()->Add()) = output_arg->ToProto();
  }

  // Before adding value info to GraphProto, sort its unordered values so that across multiple calls
  // to this function, its values are consistently written to the GraphProto
  auto sort_predicate = [](const NodeArg* v1, const NodeArg* v2) { return v1->Name() < v2->Name(); };
  std::vector<const NodeArg*> value_info_sorted{value_info_.begin(), value_info_.end()};
  std::sort(value_info_sorted.begin(), value_info_sorted.end(), sort_predicate);

  for (const auto* value_info : value_info_sorted) {
    *(graph_proto.mutable_value_info()->Add()) = value_info->ToProto();
  }

  // add the NodeArg info for outer scope NodeArgs so we capture the type information
  for (const auto& name : outer_scope_node_arg_names_) {
    auto* node_arg = GetNodeArg(name);
    ORT_ENFORCE(node_arg, "Outer scope node arg name '" + name + "'was added but does not exist. ");
    *(graph_proto.mutable_value_info()->Add()) = node_arg->ToProto();
  }

  GraphViewer graph_viewer(*this);
  // Nodes must be sorted in Topological Order in the GraphProto per ONNX spec.
  for (auto& node_idx : graph_viewer.GetNodesInTopologicalOrder()) {
    const gsl::not_null<NodeProto*> node_proto{graph_proto.add_node()};
    const gsl::not_null<const Node*> p_node{GetNode(node_idx)};
    // we need to update any GraphProto attributes for subgraphs so that any changes made by things
    // such as the optimizers are captured. otherwise we can end up saving an invalid graph.
    p_node->ToProto(*node_proto, /* update_subgraphs */ true);
  }
}

#endif  // !defined(ORT_MINIMAL_BUILD)

#if !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD)
void Graph::CleanUnusedInitializersAndNodeArgs(const std::unordered_set<std::string>* initializer_names_to_preserve) {
  // Node Args being used
  std::unordered_set<const NodeArg*> used_args;
  used_args.reserve(node_args_.size());

  // Node Args we want to preserved even not being used
  std::unordered_set<const NodeArg*> node_args_to_preserve;
  if (initializer_names_to_preserve) {
    node_args_to_preserve.reserve(initializer_names_to_preserve->size());
    for (const auto& initializer_name : *initializer_names_to_preserve) {
      const auto* initializer_node_arg = GetNodeArg(initializer_name);
      if (initializer_node_arg != nullptr) {
        ORT_IGNORE_RETURN_VALUE(node_args_to_preserve.insert(initializer_node_arg));
      }
    }
  }

  // anything that provides a required graph input (GetInputs), an optional graph input (GetOverridableInitializers)
  // or a graph output (GetOutputs) cannot be removed
  const auto& inputs = GetInputs();
  const auto& overridable_initializers = GetOverridableInitializers();
  const auto& outputs = GetOutputs();

  std::for_each(inputs.cbegin(), inputs.cend(), [&used_args](const NodeArg* input) {
    ORT_IGNORE_RETURN_VALUE(used_args.insert(input));
  });

  std::for_each(overridable_initializers.cbegin(), overridable_initializers.cend(),
                [&used_args](const NodeArg* input) {
                  ORT_IGNORE_RETURN_VALUE(used_args.insert(input));
                });

  std::for_each(outputs.cbegin(), outputs.cend(), [&used_args](const NodeArg* output) {
    ORT_IGNORE_RETURN_VALUE(used_args.insert(output));
  });

  for (const auto& node : Nodes()) {
    for (const auto* def : node.InputDefs()) {
      ORT_IGNORE_RETURN_VALUE(used_args.insert(def));
    }

    for (const auto* def : node.ImplicitInputDefs()) {
      ORT_IGNORE_RETURN_VALUE(used_args.insert(def));
    }
  }

  std::vector<std::string> erase_list;
  auto used_args_end = used_args.cend();
  for (const auto& pv : name_to_initial_tensor_) {
    const std::string& name = pv.first;
    const auto* initializer_node_arg = GetNodeArg(name);
    ORT_ENFORCE(initializer_node_arg != nullptr, "Cannot find NodeArgs for [", name, "]");
    if (used_args.find(initializer_node_arg) == used_args_end &&
        node_args_to_preserve.find(initializer_node_arg) == node_args_to_preserve.cend()) {
      // on the first call to Graph::Resolve we are removing unnecessary initializers that should be removed
      // from the model.
      // on later calls we are removing initializers that optimizations have made redundant.
      if (num_resolves_ == 0) {
        LOGS(logger_, WARNING) << "Removing initializer '"
                               << name << "'. It is not used by any node and should be removed from the model.";
      } else {
        LOGS(logger_, INFO) << "Removing initializer '" << name << "'. It is no longer used by any node.";
      }

      erase_list.push_back(name);
    }
  }

  std::for_each(erase_list.cbegin(), erase_list.cend(),
                [this](const std::string& name) {
                  RemoveInitializedTensor(name);

                  // handle edge case where the unused initializer has a matching graph input.
                  // this can only happen when initializers cannot be overridden via an optional graph input.
                  // (otherwise this initializer wouldn't be allowed to be removed due to it backing an optional
                  // graph input).
                  if (CanOverrideInitializer() == false) {
                    auto& proto_inputs = *graph_proto_->mutable_input();
                    auto i = std::find_if(proto_inputs.begin(), proto_inputs.end(),
                                          [&name](const ONNX_NAMESPACE::ValueInfoProto& input) {
                                            return input.name() == name;
                                          });

                    if (i != proto_inputs.end()) {
                      RemoveRepeatedFieldEntry(proto_inputs, i);
                    }

                    auto& inputs_including_initializers = graph_inputs_including_initializers_;
                    auto j = std::find_if(inputs_including_initializers.begin(), inputs_including_initializers.end(),
                                          [&name](const NodeArg* input) { return input->Name() == name; });

                    if (j != inputs_including_initializers.end()) {
                      inputs_including_initializers.erase(j);
                    }
                  }
                });

  // Clear the unused NodeArgs
  // We also want to scan the output NodeArgs of each node
  // In case one output of a node is neither used as an input of another node nor an output of graph
  for (const auto& node : Nodes()) {
    for (const auto* def : node.OutputDefs()) {
      ORT_IGNORE_RETURN_VALUE(used_args.insert(def));
    }
  }

  // We also need to check the Outer Scope NodeArgs
  for (const auto& outer_scope_node_arg_name : outer_scope_node_arg_names_) {
    const auto* outer_scope_node_arg = GetNodeArg(outer_scope_node_arg_name);
    ORT_ENFORCE(outer_scope_node_arg != nullptr, "Cannot find NodeArgs for [", outer_scope_node_arg_name, "]");
    ORT_IGNORE_RETURN_VALUE(node_args_to_preserve.insert(outer_scope_node_arg));
  }

  auto node_args_to_preserve_end = node_args_to_preserve.cend();
  for (auto it = node_args_.cbegin(), node_args_end = node_args_.cend(); it != node_args_end; /* no increment */) {
    auto current_entry = it++;
    const auto* current_node_arg = current_entry->second.get();
    const auto& node_arg_name = current_entry->first;
    // For some reason, we still have some code hold the raw pointer to the unused NodeArgs,
    // Remove only the NodeArgs with no type for now
    // TODO, investigate the issue when running using mpirun
    if (!node_arg_name.empty() && used_args.find(current_node_arg) == used_args_end &&
        node_args_to_preserve.find(current_node_arg) == node_args_to_preserve_end &&
        !current_node_arg->ToProto().has_type()) {
      LOGS(logger_, INFO) << "Removing NodeArg '" << node_arg_name << "'. It is no longer used by any node.";
      // Need to remove the NodeArg from both value_info_ and node_args_
      value_info_.erase(current_node_arg);
      node_args_.erase(current_entry);
    }
  }
}
#endif  // !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD)

void Graph::ComputeOverridableInitializers() {
  graph_overridable_initializers_.clear();
  if (CanOverrideInitializer()) {
    // graph_inputs_excluding_initializers_ and graph_inputs_including_initializers_
    // are inserted in the same order. So we walk and compute the difference.
    auto f_incl = graph_inputs_including_initializers_.cbegin();
    const auto l_incl = graph_inputs_including_initializers_.cend();
    auto f_excl = graph_inputs_excluding_initializers_.cbegin();
    const auto l_excl = graph_inputs_excluding_initializers_.cend();

    while (f_incl != l_incl) {
      // Equal means not an initializer
      if (f_excl != l_excl && *f_incl == *f_excl) {
        ++f_incl;
        ++f_excl;
        continue;
      }
      graph_overridable_initializers_.push_back(*f_incl);
      ++f_incl;
    }
  }
}

#if !defined(ORT_MINIMAL_BUILD)

GSL_SUPPRESS(es .84)  // warning about ignoring return value from insert(...)
Status Graph::SetGraphInputsOutputs() {
  // If loaded from a model file, we start from the specified inputs and
  // outputs set earlier by InitializeStateFromModelFileGraphProto().
  // Otherwise (!is_loaded_from_model_file_), we need to fix up the inputs and
  // may also need to infer inputs and outputs.
  // In either case, calls to SetInputs() or SetOutputs() may affect the actual
  // inputs and outputs.
  if (is_loaded_from_model_file_) return Status::OK();

  // Reset value_info.
  value_info_.clear();

  std::unordered_map<std::string, size_t> output_name_to_node_arg_index;
  std::vector<const NodeArg*> output_node_args_in_order;

  // if something is coming from outer scope, consider it already added
  std::unordered_set<std::string> added_input_names{outer_scope_node_arg_names_};
  graph_inputs_excluding_initializers_.clear();
  if (!graph_inputs_manually_set_) {
    graph_inputs_including_initializers_.clear();
  } else {
    // If we've set graph_inputs_including_initializers_ by calling SetInputs,
    // we copy its non-duplicate elements to graph_inputs_excluding_initializers_.
    // Later, we will erase initializers from graph_inputs_excluding_initializers_
    // if graph_inputs_manually_set_ is true.
    // In this way, we can ensure graph_inputs_excluding_initializers_ is the full
    // set of inputs less initializers, which could be a graph input used only
    // by a subgraph and thereby only an implicit input to a node, or a graph input
    // not used anywhere.
    // We also make sure graph_inputs_excluding_initializers_ list doesn't have any
    // duplicate names.
    std::unordered_set<std::string> existing_names;
    for (auto arg : graph_inputs_including_initializers_) {
      const std::string& name = arg->Name();
      if (existing_names.count(name) == 0) {
        graph_inputs_excluding_initializers_.push_back(arg);
        existing_names.insert(name);
      }
    }
  }

  if (!graph_outputs_manually_set_) {
    graph_outputs_.clear();
  }

  // Collect all nodes' outputs
  for (const auto& node : Nodes()) {
    for (const auto* output_def : node.OutputDefs()) {
      if (output_def->Exists()) {
        output_node_args_in_order.push_back(output_def);
        output_name_to_node_arg_index.insert({output_def->Name(), output_node_args_in_order.size() - 1});
      }
    }
  }

  // Init graph output args with copy of all node output args.
  auto graph_output_args = output_name_to_node_arg_index;
  for (const auto& node : Nodes()) {
    // Go thru all node's inputs.
    for (const auto* input_arg : node.InputDefs()) {
      if (!input_arg->Exists()) {
        // It's an optional input and does not exist in this case.
        continue;
      }

      auto output_arg_iter = output_name_to_node_arg_index.find(input_arg->Name());
      if (output_name_to_node_arg_index.end() == output_arg_iter) {
        // This input arg is not the output of another node so must come from either a graph input or an initializer.
        const std::string& name = input_arg->Name();

        if (added_input_names.end() == added_input_names.find(name)) {
          // This graph input has not been added into <graph_inputs_>.
          bool is_initializer = name_to_initial_tensor_.find(name) != name_to_initial_tensor_.end();

          if (!graph_inputs_manually_set_) {
            // if IR version < 4 all initializers must have a matching graph input
            // (even though the graph input is not allowed to override the initializer).
            // if IR version >= 4 initializers are not required to have a matching graph input.
            // any graph inputs that are to override initializers must be specified by calling SetInputs.
            if (!is_initializer || ir_version_ < 4) {
              graph_inputs_including_initializers_.push_back(input_arg);
            }
            if (!is_initializer) {
              // If input_arg is not of an initializer, we add it into graph_inputs_excluding_initializers_.
              graph_inputs_excluding_initializers_.push_back(input_arg);
            }
          } else {
            // graph_inputs_including_initializers_ has been manually populated by SetInputs.
            // Validation: the <input_arg> must be in graph inputs or initializers when it's manually set.
            if (!is_initializer) {
              const auto& inputs = graph_inputs_including_initializers_;
              bool in_inputs = std::find(inputs.begin(), inputs.end(), input_arg) != inputs.end();
              if (!in_inputs) {
                return Status(ONNXRUNTIME, onnxruntime::common::StatusCode::FAIL,
                              name + " must be either specified in graph inputs or graph initializers.");
              }
            } else {
              // If arg_input is of an initializer, we remove it from graph_inputs_excluding_initializers_
              // whose initial content has both initializers and non-initializers.
              auto input_pos = std::find(graph_inputs_excluding_initializers_.begin(),
                                         graph_inputs_excluding_initializers_.end(),
                                         input_arg);
              if (input_pos != graph_inputs_excluding_initializers_.end()) {
                graph_inputs_excluding_initializers_.erase(input_pos);
              }
            }
          }

          added_input_names.insert(name);
        }
      } else if (graph_output_args.erase(output_arg_iter->first) >= 1) {
        // Remove the output arg name from graph outputs since it's
        // the input of this node, which we call it intermediate result
        // and store it in <m_valueinfo>.
        value_info_.insert(input_arg);
      }
    }
  }

  if (!graph_outputs_manually_set_) {
    // Set graph outputs in order.
    std::vector<size_t> graph_output_args_index;
    graph_output_args_index.reserve(graph_output_args.size());
    for (const auto& output_arg : graph_output_args) {
      graph_output_args_index.push_back(output_arg.second);
    }

    std::sort(graph_output_args_index.begin(), graph_output_args_index.end());
    for (auto& output_arg_index : graph_output_args_index) {
      graph_outputs_.push_back(output_node_args_in_order[output_arg_index]);
    }
  }

  ComputeOverridableInitializers();

  return Status::OK();
}

IOnnxRuntimeOpSchemaCollectionPtr Graph::GetSchemaRegistry() const {
  return schema_registry_;
}

bool Graph::SetOpSchemaFromRegistryForNode(Node& node) {
  if (node.op_ != nullptr) return true;

  node.op_ = [&]() -> const ONNX_NAMESPACE::OpSchema* {
    const auto domain_to_version_it = DomainToVersionMap().find(node.Domain());
    if (domain_to_version_it == DomainToVersionMap().end()) {
      return nullptr;
    }
    const auto max_inclusive_version = domain_to_version_it->second;
    return schema_registry_->GetSchema(node.OpType(), max_inclusive_version, node.Domain());
  }();

  if (node.op_) {
    node.since_version_ = node.op_->since_version();

    if (node.op_->Deprecated()) {
      node.op_ = nullptr;
    }
  }

  return node.op_ != nullptr;
}
#endif  // !defined(ORT_MINIMAL_BUILD)

#if !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD)
Status Graph::PopulateNodeArgToProducerConsumerLookupsFromNodes() {
  node_arg_to_producer_node_.clear();
  node_arg_to_consumer_nodes_.clear();

  for (const auto& node : Nodes()) {
    node.ForEachDef([&](const NodeArg& node_arg, bool is_input) {
      if (is_input) {
        node_arg_to_consumer_nodes_[node_arg.Name()].insert(node.Index());
      } else {
        node_arg_to_producer_node_.insert({node_arg.Name(), node.Index()});
      }
    });
  }

  return Status::OK();
}

// calling private ctor
GSL_SUPPRESS(r .11)
gsl::not_null<Node*> Graph::AllocateNode() {
  ORT_ENFORCE(nodes_.size() < static_cast<unsigned int>(std::numeric_limits<int>::max()));
  std::unique_ptr<Node> new_node(new Node(nodes_.size(), *this));
  Node* node{new_node.get()};

  nodes_.push_back(std::move(new_node));
  ++num_of_nodes_;
  GraphResolveNeeded(true);

  return gsl::not_null<Node*>{node};
}

// TODO(s): Does this need (and maybe AllocateNode) to be threadsafe so nodes_ and num_of_nodes_ managed more carefully?
bool Graph::ReleaseNode(NodeIndex index) {
  if (index >= nodes_.size()) {
    return false;
  }

  // index is valid, but the entry may already be empty
  if (nodes_[index] != nullptr) {
    nodes_[index] = nullptr;
    --num_of_nodes_;
    GraphProtoSyncNeeded(true);
    GraphResolveNeeded(true);
  }

  return true;
}

Node& Graph::CreateFusedSubGraphNode(const IndexedSubGraph& sub_graph, const std::string& fused_node_name) {
  const auto* func_meta_def = sub_graph.GetMetaDef();
  ORT_ENFORCE(nullptr != func_meta_def);
  std::vector<NodeArg*> input_args;
  std::vector<NodeArg*> output_args;
  std::unordered_map<std::string, int> input_indexes;
  std::unordered_map<std::string, int> output_indexes;

  int cur_idx = 0;
  for (const auto& arg_name : func_meta_def->inputs) {
    // In some cases, it needs to get the NodeArgs from ancestors.
    // For example, if the subgraph we are going to build is the subgraph of the original graph
    // and the NodeArgs of the outer scope values are defined in the top-level original graph.
    input_args.push_back(GetNodeArgIncludingParentGraphs(arg_name));
    input_indexes[arg_name] = cur_idx++;
  }

  cur_idx = 0;
  for (const auto& arg_name : func_meta_def->outputs) {
    // In some cases, it needs to get the NodeArgs from ancestors.
    output_args.push_back(GetNodeArgIncludingParentGraphs(arg_name));
    output_indexes[arg_name] = cur_idx++;
  }

  auto& fused_node = AddNode(fused_node_name,
                             func_meta_def->name,
                             func_meta_def->doc_string,
                             input_args,
                             output_args,
                             &func_meta_def->attributes,
                             func_meta_def->domain);

  fused_node.SetNodeType(Node::Type::Fused);
  fused_node.SetSinceVersion(func_meta_def->since_version);

#if !defined(ORT_MINIMAL_BUILD)
  // if this is a full build create the lightweight Function implementation that provides the schema so that
  // kernel lookup works as per usual, if not using an existing schema.
  if (sub_graph.schema_source == IndexedSubGraph::SourceOfSchema::EXISTING) {
    ORT_ENFORCE(SetOpSchemaFromRegistryForNode(fused_node),
                "Schema was not found for fused node. Domain:", fused_node.Domain(), " OpType:", fused_node.OpType(),
                " SinceVersion:", fused_node.SinceVersion());
  } else if (IndexedSubGraph::SourceOfSchema::REUSE_OR_CREATE == sub_graph.schema_source) {
    auto schema_key = GenerateSchemaKey(sub_graph);
    if (reusable_fused_schema_map_.count(schema_key) == 0) {
      fused_schemas_containers_.push_back(
          function_utils::CreateSchema(*this, sub_graph, /*allow_aggregated_tensor_type=*/true));
      reusable_fused_schema_map_.emplace(schema_key, *fused_schemas_containers_.back());
    }

    fused_node.op_ = &(reusable_fused_schema_map_.at(schema_key).get());
  } else {
    fused_schemas_containers_.push_back(function_utils::CreateSchema(*this, sub_graph));
    fused_node.op_ = fused_schemas_containers_.back().get();
  }
#endif
  return fused_node;
}

Node& Graph::BeginFuseSubGraph(const IndexedSubGraph& sub_graph, const std::string& fused_node_name) {
  Node& node = CreateFusedSubGraphNode(sub_graph, fused_node_name);

  return node;
}

void Graph::FinalizeFuseSubGraph(const IndexedSubGraph& sub_graph, Node& fused_node) {
  const auto* func_meta_def = sub_graph.GetMetaDef();
  ORT_ENFORCE(nullptr != func_meta_def);

  std::unordered_map<std::string, int> input_indexes;
  std::unordered_map<std::string, int> output_indexes;

  int cur_idx = 0;
  for (auto& arg_name : func_meta_def->inputs) {
    input_indexes[arg_name] = cur_idx++;
  }

  cur_idx = 0;
  for (auto& arg_name : func_meta_def->outputs) {
    output_indexes[arg_name] = cur_idx++;
  }

  auto new_node_idx = fused_node.Index();

  // Remove nodes that were fused
  for (auto node_index : sub_graph.nodes) {
    auto node = GetNode(node_index);
    if (nullptr == node) {
      continue;
    }

    // move any applicable input edges to the new node. remove all others
    auto input_edges = node->GetRelationships().input_edges;  // copy so RemoveEdge doesn't invalidate iterator
    for (const auto& input_edge : input_edges) {
      const auto& producer = input_edge.GetNode();
      auto producer_idx = producer.Index();
      auto src_idx = input_edge.GetSrcArgIndex();
      auto dst_idx = input_edge.GetDstArgIndex();

      // if this input is an input of the fused node add an edge for that
      if (dst_idx < static_cast<int>(node->InputDefs().size())) {
        auto it = input_indexes.find(node->InputDefs()[dst_idx]->Name());
        if (it != input_indexes.cend()) {
          AddEdge(producer_idx, new_node_idx, src_idx, it->second);
        }
      } else {
        int dst_implicit_input_idx = dst_idx - static_cast<int>(node->InputDefs().size());
        ORT_ENFORCE(dst_implicit_input_idx < (int)node->ImplicitInputDefs().size());
        auto it = input_indexes.find(node->ImplicitInputDefs()[dst_implicit_input_idx]->Name());
        if (it != input_indexes.cend()) {
          AddEdge(producer_idx, new_node_idx, src_idx, it->second);
        }
      }
      RemoveEdge(producer_idx, node_index, src_idx, dst_idx);
    }

    // move any applicable output edges to the new node
    auto output_edges = node->GetRelationships().output_edges;  // copy so RemoveEdge doesn't invalidate iterator
    for (const auto& output_edge : output_edges) {
      const auto& consumer = output_edge.GetNode();
      auto consumer_idx = consumer.Index();
      auto src_idx = output_edge.GetSrcArgIndex();
      auto dst_idx = output_edge.GetDstArgIndex();

      // if this output is an output of the fused node add an edge for that
      auto it = output_indexes.find(node->OutputDefs()[src_idx]->Name());
      if (it != output_indexes.cend()) {
        AddEdge(new_node_idx, consumer_idx, it->second, dst_idx);
      }

      RemoveEdge(node_index, consumer_idx, src_idx, dst_idx);
    }

    RemoveNode(node_index);
  }
}

#endif  // #if !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD)

#if !defined(ORT_MINIMAL_BUILD)

Node& Graph::FuseSubGraph(const IndexedSubGraph& sub_graph,
                          const std::string& fused_node_name) {
  Node& fused_node = CreateFusedSubGraphNode(sub_graph, fused_node_name);

  // create Function before we remove nodes
  fused_node.func_body_ = std::make_unique<FunctionImpl>(*this, sub_graph);
  // remove nodes and update edges
  FinalizeFuseSubGraph(sub_graph, fused_node);

  return fused_node;
}

Status Graph::AddConstantProtoAsInitializer(const ONNX_NAMESPACE::NodeProto& node_proto,
                                            std::optional<std::string_view> new_name) {
  ONNX_NAMESPACE::TensorProto tensor_proto;
  ORT_RETURN_IF_ERROR(utils::ConstantNodeProtoToTensorProto(node_proto, ModelPath(), tensor_proto, node_proto.output(0)));
  if (new_name.has_value()) {
    tensor_proto.set_name(std::string(new_name.value()));
  }

  // In the constant node, we won't have symbolic dims.
  const auto tensor_shape = utils::GetTensorShapeFromTensorProto(tensor_proto);
  auto ml_data = DataTypeImpl::TensorTypeFromONNXEnum(tensor_proto.data_type())->GetElementType();
  const size_t size_in_bytes = Tensor::CalculateTensorStorageSize(ml_data, tensor_shape);

  if (size_in_bytes > utils::kSmallTensorExternalDataThreshold) {
    OrtValue ort_value;
    ORT_RETURN_IF_ERROR(utils::TensorProtoToOrtValue(Env::Default(), ModelPath(), tensor_proto,
                                                     CPUAllocator::DefaultInstance(), ort_value));

    constexpr const bool use_tensor_buffer_true = true;
    auto tensor_proto_to_add = utils::TensorToTensorProto(ort_value.Get<Tensor>(), tensor_proto.name(),
                                                          use_tensor_buffer_true);
    ORT_RETURN_IF_ERROR(AddInitializedOrtValue(tensor_proto_to_add, ort_value));
  } else {
    AddInitializedTensor(tensor_proto);
  }

  if (GetNodeArg(tensor_proto.name()) == nullptr) {
    TypeProto t{utils::TypeProtoFromTensorProto(tensor_proto)};
    ORT_IGNORE_RETURN_VALUE(GetOrCreateNodeArg(tensor_proto.name(), &t));
  }

#if !defined(DISABLE_SPARSE_TENSORS)
  if (node_proto.attribute(0).type() == AttributeProto_AttributeType_SPARSE_TENSOR) {
    ORT_IGNORE_RETURN_VALUE(sparse_tensor_names_.emplace(tensor_proto.name()));
  }
#endif

  return Status::OK();
}

static void ReassignSubgraphDependentNodeArgs(const InlinedHashMap<std::string, NodeArg*>& name_to_nodearg,
                                              Graph& graph) {
  for (auto& node : graph.Nodes()) {
    if (node.ContainsSubgraph()) {
      for (auto& [name, subgraph] : node.GetAttributeNameToMutableSubgraphMap()) {
        ReassignSubgraphDependentNodeArgs(name_to_nodearg, *subgraph);
      }
    }

    // NodeArgs need to be updated
    for (auto& input_def : node.MutableInputDefs()) {
      if (input_def->Exists()) {
        auto hit = name_to_nodearg.find(input_def->Name());
        if (hit != name_to_nodearg.cend()) {
          // Make sure we create a local to this subgraph definition
          const auto* new_name_arg = hit->second;
          input_def = &graph.GetOrCreateNodeArg(new_name_arg->Name(), input_def->TypeAsProto());
        }
      }
    }
  }
}

Status Graph::InlineIfSubgraph(bool condition_value, Node& if_node, const logging::Logger& logger) {
  static const std::string then_branch{"then_branch"};
  static const std::string else_branch{"else_branch"};
  Graph* sub_graph;
  if (condition_value) {
    sub_graph = if_node.GetMutableGraphAttribute(then_branch);
  } else {
    sub_graph = if_node.GetMutableGraphAttribute(else_branch);
  }

  if (sub_graph == nullptr) {
    auto str = MakeString("Unable to constant fold If node: '", if_node.Name(), "' Unable to fetch: ",
                          (condition_value ? then_branch : else_branch));
    LOGS(logger, WARNING) << str;
    return Status::OK();
  }

  Graph& graph_to_inline = *sub_graph;

  std::string unique_id{"_if_"};
  if (condition_value) {
    unique_id.append(then_branch);
  } else {
    unique_id.append(else_branch);
  }

  unique_id = GenerateNodeName(unique_id);

  auto make_unique = [&unique_id](const std::string& name) {
    return unique_id + '_' + name;
  };

  // Check if the name is an input or implicit input.
  // These are not renamed, and we do not need to adjust subgraphs for them.
  // Implicit inputs would cover both If node input and implicit inputs.
  // Reason: there are no explicit inputs to the subgraphs, and the subgraph's
  // implicit inputs must be covered by the implicit inputs of the If node.
  InlinedHashMap<std::string_view, NodeArg*> outer_scope_values;
  const auto& if_implicit_inputs = if_node.MutableImplicitInputDefs();
  outer_scope_values.reserve(if_implicit_inputs.size());

  for (auto* input : if_implicit_inputs) {
    const auto& name = input->Name();
    ORT_IGNORE_RETURN_VALUE(outer_scope_values.emplace(name, input));
  }

  // Name mapping from the graph to inline to the graph we are inlining into
  // we also use this to process any subgraphs in the graph we are inlining
  InlinedHashMap<std::string, NodeArg*> name_to_nodearg;

  // We are going to map the outputs of the graph to inline to the outputs of the If node.
  // They are assumed to be in the same order.
  const auto& node_output_defs = if_node.MutableOutputDefs();
  const auto& graph_output_defs = graph_to_inline.GetOutputs();
  for (size_t i = 0; i < graph_output_defs.size(); ++i) {
    name_to_nodearg.emplace(graph_output_defs[i]->Name(), node_output_defs[i]);
  }

  // Move initializers from the subgraph to the destination graph.
  for (int i = 0, limit = graph_to_inline.graph_proto_->initializer_size(); i < limit; ++i) {
    auto* initializer = graph_to_inline.graph_proto_->mutable_initializer(i);
    const std::string src_name = initializer->name();

#if !defined(DISABLE_SPARSE_TENSORS)
    bool has_sparse_origin = false;
    if (!graph_to_inline.sparse_tensor_names_.empty()) {
      auto hit = graph_to_inline.sparse_tensor_names_.find(src_name);
      if (hit != graph_to_inline.sparse_tensor_names_.cend()) {
        has_sparse_origin = true;
        // Erase the entry that will be invalidated
        graph_to_inline.sparse_tensor_names_.erase(hit);
      }
    }
#endif

    graph_to_inline.name_to_initial_tensor_.erase(src_name);
    const gsl::not_null<TensorProto*> tensor{graph_proto_->add_initializer()};
    *tensor = std::move(*initializer);

    // Check if this is an output of the graph
    auto hit = name_to_nodearg.find(src_name);
    if (hit != name_to_nodearg.cend()) {
      // We rename it to If node output.
      tensor->set_name(hit->second->Name());
    } else {
      NodeArg* node_arg = graph_to_inline.GetNodeArg(src_name);
      assert(node_arg != nullptr);
      auto new_name = GenerateNodeArgName(make_unique(src_name));
      NodeArg& new_arg = GetOrCreateNodeArg(new_name, node_arg->TypeAsProto());
      ORT_IGNORE_RETURN_VALUE(name_to_nodearg.insert_or_assign(src_name, &new_arg));
      tensor->set_name(std::move(new_name));
    }

    // We have the following cases:
    // No external data, just copy the proto. If it was too big,
    // it would have already been converted to OrtInitializer.
    // External data in file - copy the proto, it can be loaded during session finalization
    //          or it would be loaded by EP
    // External data in memory two cases
    // - points to flatbuffers ort format (no OrtValue), simply copy the proto
    // - points to external data in memory (OrtValue), create a copy of OrtValue and tensor_proto

    if (utils::HasExternalDataInMemory(*tensor)) {
      OrtValue ort_value;
      if (graph_to_inline.GetOrtValueInitializer(src_name, ort_value)) {
        ortvalue_initializers_.insert_or_assign(tensor->name(), std::move(ort_value));
      }
    }

    auto insert_result = name_to_initial_tensor_.emplace(tensor->name(), tensor);
    ORT_ENFORCE(insert_result.second, "Initializer name: ", tensor->name(), " from graph: ",
                graph_to_inline.Name(), " conflicts with graph initializer. Check name generation above.");

#if !defined(DISABLE_SPARSE_TENSORS)
    if (has_sparse_origin) {
      ORT_IGNORE_RETURN_VALUE(sparse_tensor_names_.emplace(tensor->name()));
    }
#endif
  }

  // Look up nodes that would be providing input to our nodes (implicit and explicit)
  // and any nodes that take the output of our nodes (used to be If output)
  // Map of NodeArg name to pair of Node* and input index in the destination node
  using NodeAndIndex = std::pair<gsl::not_null<Node*>, int>;
  using ArgNameToNodeMap = InlinedHashMap<std::string_view, NodeAndIndex>;
  ArgNameToNodeMap input_args;
  // Map of NodeArg name to pair of Node* and output index in the source node.
  ArgNameToNodeMap output_args;

  auto map_defs = [](Node& node, ArgNameToNodeMap& map, bool input) {
    const auto defs = (input) ? node.InputDefs() : node.OutputDefs();
    map.reserve(map.size() + defs.size());
    int arg_pos = -1;
    for (auto* node_arg : defs) {
      ++arg_pos;
      if (node_arg->Exists()) {
        map.emplace(node_arg->Name(), std::make_pair(&node, arg_pos));
      }
    }
  };

  const bool is_this_main_graph = (parent_graph_ == nullptr);
  // Map the inputs and outputs of the If node to the nodes in the graph to inline.
  if (!is_this_main_graph) {
    for (auto& node : Nodes()) {
      if (node.Index() == if_node.Index()) {
        continue;
      }
      map_defs(node, input_args, true);
      map_defs(node, output_args, false);
    }
  }

  auto* non_existing_arg = &GetOrCreateNodeArg(std::string(), nullptr);
  // We want to make sure we get nodes in topological order
  // because Constant folding may cause the nodes appear in
  // a different order.
  InlinedVector<Node*> new_nodes;
  GraphViewer graph(graph_to_inline);
  for (const auto node_idx : graph.GetNodesInTopologicalOrder()) {
    // GraphViewer filters out nullptrs
    auto* node = graph_to_inline.GetNode(node_idx);
    assert(node->OpType() != kConstant);

    // Inputs
    // Chop off trailing non-existing defs, but preserve non-existing in the middle
    auto& input_defs = node->MutableInputDefs();
    auto last_existing = std::find_if(input_defs.rbegin(), input_defs.rend(),
                                      [](const NodeArg* node_arg) { return node_arg->Exists(); });
    input_defs.resize(std::distance(input_defs.begin(), last_existing.base()));

    InlinedVector<NodeArg*> new_input_defs;
    for (auto* input_def : node->InputDefs()) {
      if (input_def->Exists()) {
        // Check if this is one of the implicit graph inputs
        // then re-assign the def to the outer scope value.
        const auto& input_name = input_def->Name();
        auto outer_hit = outer_scope_values.find(input_name);
        if (outer_hit != outer_scope_values.cend()) {
          // get/create local definition
          NodeArg* outer_arg = outer_hit->second;
          auto& this_scope_arg = GetOrCreateNodeArg(outer_arg->Name(), input_def->TypeAsProto());
          new_input_defs.push_back(&this_scope_arg);
        } else {
          auto hit = name_to_nodearg.find(input_name);
          if (hit != name_to_nodearg.cend()) {
            // This is other node output in the dest graph,
            // constant node or initializer that was renamed.
            new_input_defs.push_back(hit->second);
          } else {
            ORT_THROW("Node's: ", node->Name(), " input: ", input_name,
                      " is not If node's input or previous node output in this subgraph");
          }
        }
      } else {
        new_input_defs.push_back(non_existing_arg);
      }
    }

    // Outputs
    // Chop off trailing non-existing defs
    auto& output_defs = node->MutableOutputDefs();
    last_existing = std::find_if(output_defs.rbegin(), output_defs.rend(),
                                 [](const NodeArg* node_arg) { return node_arg->Exists(); });
    output_defs.resize(std::distance(output_defs.begin(), last_existing.base()));

    InlinedVector<NodeArg*> new_output_defs;
    for (auto* output_def : node->OutputDefs()) {
      if (output_def->Exists()) {
        const auto& output_name = output_def->Name();
        auto hit = name_to_nodearg.find(output_name);
        if (hit != name_to_nodearg.cend()) {
          // This is one of the If node outputs, simply reassign the def.
          // If node defs are already in the destination graph
          new_output_defs.push_back(hit->second);
        } else {
          // We generate an output to downstream nodes.
          auto new_name = GenerateNodeArgName(make_unique(output_name));
          NodeArg& new_arg = GetOrCreateNodeArg(new_name, output_def->TypeAsProto());
          new_output_defs.push_back(&new_arg);
          ORT_IGNORE_RETURN_VALUE(name_to_nodearg.emplace(output_name, &new_arg));
        }
      } else {
        new_output_defs.push_back(non_existing_arg);
      }
    }

    const auto new_node_name = GenerateNodeName(make_unique(node->OpType()));
    Node& new_node = AddNode(new_node_name, node->OpType(), node->Description(),
                             new_input_defs,
                             new_output_defs,
                             nullptr,
                             node->Domain());

    new_node.SetSinceVersion(node->SinceVersion());
    new_node.op_ = node->op_;

    if (!is_this_main_graph) {
      map_defs(new_node, input_args, true);
      map_defs(new_node, output_args, false);
      new_nodes.push_back(&new_node);
    }

    if (node->ContainsSubgraph()) {
      auto& subgraphs = node->MutableSubgraphs();

      // Check if any of this node implicit inputs of this graph is in the renaming map
      // that would mean they come from the destination graph, not from the parent
      // of the destination graph.
      int renames_subgraph_names = 0;
      auto& implicit_defs = node->MutableImplicitInputDefs();
      for (auto& input_def : implicit_defs) {
        auto hit = name_to_nodearg.find(input_def->Name());
        if (hit != name_to_nodearg.cend()) {
          input_def = hit->second;
          ++renames_subgraph_names;
        }
      }

      for (auto& subgraph : subgraphs) {
        if (renames_subgraph_names > 0) {
          // We need to rename the subgraph node names
          // because they may refer to the implicit inputs
          // that were renamed.
          ReassignSubgraphDependentNodeArgs(name_to_nodearg, *subgraph);
        }
        subgraph->parent_node_ = &new_node;
        subgraph->parent_graph_ = this;
      }

      new_node.MutableSubgraphs() = std::move(subgraphs);
      new_node.GetMutableMapOfAttributeNameToSubgraph() = std::move(node->GetMutableMapOfAttributeNameToSubgraph());
      new_node.MutableImplicitInputDefs() = std::move(implicit_defs);
    }

    new_node.GetMutableAttributes() = std::move(node->GetMutableAttributes());
  }

  // Let's rebuild local connections, so next time a GraphViewer is able to perform topological sort.
  // We only need to do so if this graph is not the main graph, because the main graph is going to resolve
  // and it is not possible to inline the same nodes again.
  if (!is_this_main_graph) {
    for (auto* node : new_nodes) {
      int arg_pos = -1;
      for (auto* input_def : node->InputDefs()) {
        ++arg_pos;
        auto hit = output_args.find(input_def->Name());
        if (hit != output_args.cend()) {
          // The input to this node is an output from a previous node in this graph.
          // Create relationship between this node (node), and the node providing the output (output_node).
          const auto& [producer, src_idx] = hit->second;
          AddEdge(producer->Index(), node->Index(), src_idx, arg_pos);
        }
      }

      // Check if any of the outputs for inlined nodes are inputs to other nodes in the graph.
      // (outputs of If node)
      arg_pos = -1;
      for (auto& output_def : node->OutputDefs()) {
        ++arg_pos;
        auto hit = input_args.find(output_def->Name());
        if (hit != input_args.cend()) {
          // The output of this node is an input to another node in this graph.
          // Create relationship between this node (node), and the node using the input (input_node).
          const auto& [consumer, dst_idx] = hit->second;
          AddEdge(node->Index(), consumer->Index(), arg_pos, dst_idx);
        }
      }
    }
  }

  LOGS(logger, INFO) << "Constant folded (inlined) " << (condition_value ? then_branch : else_branch)
                     << " for If node: " << if_node.Name();

  return Status::OK();
}

Status Graph::InlineFunctionProto(const ONNX_NAMESPACE::FunctionProto& func_to_inline) {
  auto to_node_arg = [this](const std::string& name) {
    return &this->GetOrCreateNodeArg(name, nullptr);
  };

  // Process constant nodes first and create NodeArg for these as they become initializers
  // It is important for the initializers to have NodeArg created, first they are needed
  // if the initializer is unused and removed, second if the node depends on the initializer,
  // we can have Type attached to it.
  InlinedVector<const NodeProto*> non_constant_nodes;
  non_constant_nodes.reserve(func_to_inline.node_size());
  for (const auto& inlined_node : func_to_inline.node()) {
    if (inlined_node.op_type() == kConstant) {
      // Copy constant nodes _value to name_to_initial_tensor_
      ORT_RETURN_IF_ERROR(AddConstantProtoAsInitializer(inlined_node, std::nullopt));
    } else {
      non_constant_nodes.push_back(&inlined_node);
    }
  }

  for (const auto* inlined_node : non_constant_nodes) {
    InlinedVector<onnxruntime::NodeArg*> inputs;
    InlinedVector<onnxruntime::NodeArg*> outputs;

    for (const auto& tensor_name : inlined_node->input())
      inputs.push_back(to_node_arg(tensor_name));

    for (const auto& tensor_name : inlined_node->output())
      outputs.push_back(to_node_arg(tensor_name));

    onnxruntime::NodeAttributes new_attr_map;
    new_attr_map.reserve(inlined_node->attribute_size());
    for (const auto& node_attr : inlined_node->attribute()) {
      new_attr_map.insert_or_assign(node_attr.name(), node_attr);
    }
    ORT_IGNORE_RETURN_VALUE(AddNode(inlined_node->name(), inlined_node->op_type(),
                                    inlined_node->doc_string(), inputs, outputs,
                                    &new_attr_map, inlined_node->domain()));
  }

  return Status::OK();
}

Status Graph::InlineFunction(Node& callnode) {
  // Remove output edges. Requirement for RemoveNode() below.
  auto output_edges = callnode.GetRelationships().output_edges;  // copy so RemoveEdge doesn't invalidate iterator
  for (const auto& output_edge : output_edges) {
    RemoveEdge(callnode.Index(), output_edge.GetNode().Index(), output_edge.GetSrcArgIndex(),
               output_edge.GetDstArgIndex());
  }

  // create a uniq_identifier to append to every node name and intermediate input\outputs
  // to make sure there are no unintended duplicates
  std::string base_uniq_identifier{"_inlfunc_"};
  base_uniq_identifier.append(callnode.OpType());
  const auto uniq_identifier = GenerateNodeName(base_uniq_identifier);

  // Replace a (function-call) node by an inlined graph.
  if (!callnode.GetFunctionBody()) {
    // This is the normal use-case: inlining a FunctionProto (representing
    // a model-local function or a schema-defined function).
    ONNX_NAMESPACE::FunctionProto inlined_fp;
    ORT_ENFORCE(callnode.TryGetFunctionProto(inlined_fp), "Node has no function body and cannot be inlined.");

    // Make all the names unique and resolve nested graphs inputs to the outer scope.
    function_utils::Specialize(inlined_fp, callnode, uniq_identifier);

    // In this case, global Resolve() will take care of everything.
    ORT_RETURN_IF_ERROR(InlineFunctionProto(inlined_fp));
  } else {
    // Uncommon scenario. Inlining a node representing a fused sub-graph.
    // TODO: Unclear that this feature is needed. Can this be removed?
    const Graph& subgraph = callnode.GetFunctionBody()->Body();

    for (const auto& subgraph_node : subgraph.Nodes()) {
      if (subgraph_node.OpType() != kConstant) {
        InlinedVector<onnxruntime::NodeArg*> inputs;
        InlinedVector<onnxruntime::NodeArg*> outputs;
        for (auto* input : subgraph_node.InputDefs()) {
          auto& n_input = GetOrCreateNodeArg(input->Name(), input->TypeAsProto());
          inputs.push_back(&n_input);
        }
        for (auto* output : subgraph_node.OutputDefs()) {
          auto& n_output = GetOrCreateNodeArg(output->Name(), output->TypeAsProto());
          outputs.push_back(&n_output);
        }

        AddNode(subgraph_node.Name() + uniq_identifier, subgraph_node.OpType(), subgraph_node.Description(),
                inputs,
                outputs,
                &subgraph_node.GetAttributes(),
                subgraph_node.Domain());
      }
    }

    // Process constant nodes and initializers after all other nodes
    // so NodeArgs are created from the nodes
    for (const auto& subgraph_node : subgraph.Nodes()) {
      if (subgraph_node.OpType() == kConstant) {
        // Copy constant nodes _value to name_to_initial_tensor_
        ONNX_NAMESPACE::NodeProto subgraph_node_proto{};
        subgraph_node.ToProto(subgraph_node_proto);
        ORT_RETURN_IF_ERROR(AddConstantProtoAsInitializer(subgraph_node_proto, std::nullopt));
      }
    }

    for (const auto& init : subgraph.name_to_initial_tensor_) {
      const auto& [name, tensor_proto] = init;
      auto tensor_proto_to_add = *tensor_proto;
      tensor_proto_to_add.set_name(name + uniq_identifier);
      if (OrtValue ort_value; subgraph.GetOrtValueInitializer(name, ort_value)) {
        ORT_RETURN_IF_ERROR(AddInitializedOrtValue(tensor_proto_to_add, ort_value));
      } else {
        AddInitializedTensor(tensor_proto_to_add);
      }
    }
  }

  RemoveNode(callnode.Index());

  // std::cout << "Graph after inlining\n\n" << *this << std::endl << std::flush;

  return Status::OK();
}

void Graph::SetInputs(gsl::span<const NodeArg* const> inputs) {
  graph_inputs_including_initializers_.clear();
  graph_inputs_excluding_initializers_.clear();

  // creating graph from scratch
  // rely on SetGraphInputsOutputs() to fix up graph_inputs_excluding_initializers_
  // if is_loaded_from_model_file_ == false
  graph_inputs_including_initializers_.reserve(inputs.size());
  graph_inputs_including_initializers_.assign(inputs.begin(), inputs.end());

  if (is_loaded_from_model_file_) {
    // graph loaded from model file
    for (const auto* input : inputs) {
      ORT_ENFORCE(input->Exists(), "Input to set must exist.");
      if (name_to_initial_tensor_.find(input->Name()) == name_to_initial_tensor_.end()) {
        graph_inputs_excluding_initializers_.emplace_back(input);
      }
    }

    ComputeOverridableInitializers();
  }

  graph_inputs_manually_set_ = true;
  GraphProtoSyncNeeded(true);
  GraphResolveNeeded(true);
}

void Graph::SetOutputs(gsl::span<const NodeArg* const> outputs) {
  graph_outputs_.clear();
  graph_outputs_.reserve(outputs.size());
  graph_outputs_.assign(outputs.begin(), outputs.end());

  graph_outputs_manually_set_ = true;
  GraphProtoSyncNeeded(true);
  GraphResolveNeeded(true);
}

#endif  // !defined(ORT_MINIMAL_BUILD)

#if !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD)
void Graph::SetNodeArgType(NodeArg& arg, const ONNX_NAMESPACE::TypeProto& type_proto) {
  arg.SetType(type_proto);
  GraphResolveNeeded(true);
}
#endif  // !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD)

Graph::~Graph() {
  // nothing to do, but we put it here so we don't need to fully define types in Graph that are held in unique_ptr
  // such as   std::unique_ptr<FunctionContainer> function_container_;
}

#if !defined(ORT_MINIMAL_BUILD)
std::ostream& operator<<(std::ostream& out, const NodeArg& node_arg) {
  out << "\"" << node_arg.Name() << "\"";
  if (node_arg.Type()) {
    out << ": " << *node_arg.Type();
  }
  return out;
}

std::ostream& operator<<(std::ostream& out, const Node& node) {
  out << "(\"" << node.Name() << "\""
      << ", "
      << node.OpType()
      << ", "
      // Use quote so default ONNX domain is shown as ""
      // rather than misleading empty string.
      << "\"" << node.Domain() << "\""
      << ", "
      << node.SinceVersion()
      << ") : (";
  for (const auto* x : node.InputDefs()) {
    if (x->Exists()) {
      out << *x << ",";
    } else {
      // Print missing (or optional) inputs
      // because operator schema uses positional
      // arguments in ONNX.
      out << "\"\""
          << ",";
    }
  }
  out << ") -> (";
  for (const auto* x : node.OutputDefs()) {
    if (x->Exists()) {
      out << *x << ",";
    } else {
      // Print missing (or optional) outputs
      // because operator schema uses positional
      // arguments in ONNX.
      out << "\"\""
          << ",";
    }
  }
  out << ") ";
  return out;
}

std::ostream& operator<<(std::ostream& out, const Graph& graph) {
  out << "Inputs:\n";
  for (const auto* x : graph.GetInputs()) {
    // Unlike we print missing input and output for operator, we don't
    // print missing input for graph because they are not helpful (we
    // don't have a fixed schema for graph to match arguments).
    if (x) {
      out << "   " << *x << "\n";
    }
  }
  out << "Nodes:\n";
  for (const auto& node : graph.Nodes()) {
    out << "   " << node << "\n";
  }
  out << "Outputs:\n";
  for (const auto* x : graph.GetOutputs()) {
    // Similar to graph input, missing graph output is not printed.
    if (x) {
      out << "   " << *x << "\n";
    }
  }
  return out;
}
#endif  // !defined(ORT_MINIMAL_BUILD)

Status Graph::LoadFromOrtFormat(const onnxruntime::fbs::Graph& fbs_graph,
                                const Model& owning_model,
                                const std::unordered_map<std::string, int>& domain_to_version,
#if !defined(ORT_MINIMAL_BUILD)
                                IOnnxRuntimeOpSchemaCollectionPtr schema_registry,
#endif
                                const OrtFormatLoadOptions& load_options,
                                const logging::Logger& logger, std::unique_ptr<Graph>& graph) {
  graph = std::make_unique<Graph>(owning_model, domain_to_version,
#if !defined(ORT_MINIMAL_BUILD)
                                  schema_registry,
#endif
                                  nullptr, nullptr, logger,
                                  // Assume anything in ORT format has already been validated.
                                  false);

  ORT_RETURN_IF_ERROR(graph->LoadFromOrtFormat(fbs_graph, load_options));

#if !defined(ORT_MINIMAL_BUILD)
  // in a full build we need to run Resolve to fully populate ResolveContext and Node::op_,
  // which will allow optimizers to run or non-ORT EPs to take nodes.
  // TODO: We could decide that an ORT model is load only even in a full build,
  // and in InferenceSession::Initialize skip partitioning and running optimizers.
  graph->SetGraphResolveNeeded();
  ORT_RETURN_IF_ERROR(graph->Resolve());
#endif

  return Status::OK();
}

Status Graph::LoadFromOrtFormat(const onnxruntime::fbs::Graph& fbs_graph,
                                Graph& parent_graph, const Node& parent_node,
                                const OrtFormatLoadOptions& load_options,
                                const logging::Logger& logger, std::unique_ptr<Graph>& graph) {
  graph = std::make_unique<Graph>(parent_graph.owning_model_,
                                  parent_graph.domain_to_version_,
#if !defined(ORT_MINIMAL_BUILD)
                                  parent_graph.schema_registry_,
#endif
                                  &parent_graph, &parent_node,
                                  logger,
                                  // Assume anything in ORT format has already been validated.
                                  false);

  return graph->LoadFromOrtFormat(fbs_graph, load_options);
}

Graph::Graph(const Model& owning_model,
             const std::unordered_map<std::string, int>& domain_to_version,
#if !defined(ORT_MINIMAL_BUILD)
             IOnnxRuntimeOpSchemaCollectionPtr schema_registry,
#endif
             Graph* parent_graph, const Node* parent_node,
             const logging::Logger& logger,
             bool strict_shape_type_inference)
    : owning_model_(owning_model),
      graph_proto_(&deserialized_proto_data_),
#if !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD)
      runtime_optimizations_ptr_(std::make_unique<RuntimeOptimizationRecordContainer>()),
      runtime_optimizations_(*runtime_optimizations_ptr_),
#endif
#if !defined(ORT_MINIMAL_BUILD)
      schema_registry_(schema_registry),
#endif
      domain_to_version_(domain_to_version),
      ir_version_(owning_model.IrVersion()),
      parent_graph_(parent_graph),
      parent_node_(parent_node),
      logger_(logger),
      strict_shape_type_inference_(strict_shape_type_inference),
      is_loaded_from_model_file_(true) {  // true as the Graph isn't manually constructed from scratch
}

common::Status Graph::LoadFromOrtFormat(const onnxruntime::fbs::Graph& fbs_graph,
                                        const OrtFormatLoadOptions& load_options) {
  // We deserialize the graph from ORT format in the following order:
  // 1. Deserialize the initializers and sparse initializers. Convert sparse to dense.
  // 2. Deserialize the NodeArgs
  //        We need all NodeArg instances to exist when deserializing Nodes to setup the Node's
  //        inputs/outputs/implicit inputs which are collections of NodeArg*.
  // 3. Deserialize the Nodes
  // 4. Deserialize the NodeEdges
  //        We need all the Node instances to exist as the EdgeEnd has a Node* for the other end of the edge
  // 5. Deserialize the Inputs/Outputs/outer_scope_node_args
  // 6. Deserialize the runtime optimizations, if enabled

  // Initializers
  auto fbs_initializers = fbs_graph.initializers();
#if !defined(DISABLE_SPARSE_TENSORS)
  auto fbs_sparse_initializers = fbs_graph.sparse_initializers();
  flatbuffers::uoffset_t map_size = (fbs_initializers != nullptr ? fbs_initializers->size() : 0U) +
                                    (fbs_sparse_initializers != nullptr ? fbs_sparse_initializers->size() : 0U);
#else
  flatbuffers::uoffset_t map_size = (fbs_initializers != nullptr ? fbs_initializers->size() : 0U);
#endif

  if (map_size > 0) {
    name_to_initial_tensor_.reserve(map_size);
  }

  if (fbs_initializers) {
    for (const auto* fbs_tensor : *fbs_initializers) {
      ORT_RETURN_IF(nullptr == fbs_tensor, "Initializer tensor is missing. Invalid ORT format model.");
      TensorProto* initializer = deserialized_proto_data_.add_initializer();
      ORT_RETURN_IF_ERROR(fbs::utils::LoadInitializerOrtFormat(*fbs_tensor, *initializer, load_options));
      auto p = name_to_initial_tensor_.emplace(initializer->name(), initializer);
      if (!p.second) {
        LOGS(logger_, WARNING) << "Duplicate initializer (dense or ConstantNode): '" << initializer->name()
                               << "' the model will use the latest encountered initializer"
                               << ". Please, fix your model.";
        p.first->second = initializer;
      }
      /// Create an OrtValue on top of the flatbuffer for consistency
      /// This is created for a special case when we deserialize TensorProtos on top of the
      /// user supplied buffer, and they are responsible for keeping this alive.
      if (load_options.can_use_flatbuffer_for_initializers && utils::HasExternalDataInMemory(*initializer)) {
        std::unique_ptr<ExternalDataInfo> external_data_info;
        ORT_RETURN_IF_ERROR(ExternalDataInfo::Create(initializer->external_data(), external_data_info));
        auto data_offset = external_data_info->GetOffset();
        OrtValue ort_value;
        // const_cast due to Tensor constructor requiring non-const void* for data.
        Tensor::InitOrtValue(DataTypeImpl::TensorTypeFromONNXEnum(
                                 initializer->data_type())
                                 ->GetElementType(),
                             onnxruntime::utils::GetTensorShapeFromTensorProto(*initializer),
                             reinterpret_cast<void*>(data_offset), CPUAllocator::DefaultInstance()->Info(),
                             ort_value);
        ortvalue_initializers_.insert_or_assign(initializer->name(), std::move(ort_value));
      }
    }
  }

#if !defined(DISABLE_SPARSE_TENSORS)
  if (fbs_sparse_initializers) {
    sparse_tensor_names_.reserve(fbs_sparse_initializers->size());
    const auto& model_path = ModelPath();

    for (const auto* fbs_sparse_tensor : *fbs_sparse_initializers) {
      ORT_RETURN_IF(nullptr == fbs_sparse_tensor, "Sparse Initializer tensor is missing. Invalid ORT format model.");
      SparseTensorProto sparse_initializer;
      ORT_RETURN_IF_ERROR(fbs::utils::LoadSparseInitializerOrtFormat(*fbs_sparse_tensor, sparse_initializer,
                                                                     load_options));
      TensorProto& initializer = *deserialized_proto_data_.add_initializer();
      ORT_RETURN_IF_ERROR(utils::SparseTensorProtoToDenseTensorProto(sparse_initializer, model_path, initializer));
      auto p = name_to_initial_tensor_.emplace(initializer.name(), &initializer);
      if (!p.second) {
        LOGS(logger_, WARNING) << "Duplicate initializer (dense, sparse or ConstantNode): '" << initializer.name()
                               << "' the model will use the latest encountered initializer"
                               << ". Please, fix your model.";
        p.first->second = &initializer;
      }
      sparse_tensor_names_.emplace(initializer.name());
    }
  }
#endif

  // NodeArgs
  auto fbs_node_args = fbs_graph.node_args();
  if (fbs_node_args) {
    node_args_.reserve(fbs_node_args->size());
    for (const auto* fbs_value_info : *fbs_node_args) {
      ORT_RETURN_IF(nullptr == fbs_value_info, "NodeArg is missing. Invalid ORT format model.");
      NodeArgInfo node_arg_info;
      ORT_RETURN_IF_ERROR(fbs::utils::LoadValueInfoOrtFormat(*fbs_value_info, node_arg_info));
      node_args_[fbs_value_info->name()->str()] = std::make_unique<NodeArg>(std::move(node_arg_info));
    }
  }

  // Nodes
  //
  // Since we access a node using its index, we need to have nodes_ with size max_node_index to avoid
  // out of bounds access.
  nodes_.resize(fbs_graph.max_node_index());
  auto* fbs_nodes = fbs_graph.nodes();

  // It is possible to have no nodes in the model. Most likely scenario is the subgraph of an If Node
  // where the subgraph returns a Constant node. The Constant node will be lifted to an initializer by ORT
  // (prior to serializing to ORT format), leaving a valid Graph that contains no nodes.
  if (fbs_nodes != nullptr) {
    for (const auto* fbs_node : *fbs_nodes) {
      ORT_RETURN_IF(nullptr == fbs_node, "Node is missing. Invalid ORT format model.");
      std::unique_ptr<Node> node;
      ORT_RETURN_IF_ERROR(Node::LoadFromOrtFormat(*fbs_node, *this, load_options, logger_, node));
      ORT_RETURN_IF(node->Index() >= fbs_graph.max_node_index(), "Node index is out of range");
      nodes_[node->Index()] = std::move(node);
      ++num_of_nodes_;
    }
  }

  // NodeEdges
  auto* fbs_node_edges = fbs_graph.node_edges();
  if (fbs_node_edges != nullptr) {
    for (const auto* fbs_node_edge : *fbs_node_edges) {
      ORT_RETURN_IF(nullptr == fbs_node_edge, "NodeEdge is missing. Invalid ORT format model.");
      ORT_RETURN_IF(fbs_node_edge->node_index() >= fbs_graph.max_node_index(), "Node index is out of range");
      ORT_RETURN_IF_ERROR(nodes_[fbs_node_edge->node_index()]->LoadEdgesFromOrtFormat(*fbs_node_edge, *this));
    }
  }

  // Inputs/Outputs/outer_scope_node_args
  auto add_node_args = [&](const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>* fbs_node_args,
                           std::vector<const NodeArg*>& node_args) -> Status {
    if (fbs_node_args != nullptr) {
      node_args.reserve(fbs_node_args->size());
      for (const auto* fbs_node_arg_name : *fbs_node_args) {
        ORT_RETURN_IF(nullptr == fbs_node_arg_name, "NodeArg Name is missing. Invalid ORT format model.");
        gsl::not_null<NodeArg*> node_arg = GetNodeArg(fbs_node_arg_name->str());
        node_args.push_back(node_arg);
      }
    }
    return Status::OK();
  };

  ORT_RETURN_IF_ERROR(add_node_args(fbs_graph.inputs(), graph_inputs_including_initializers_));
  for (const auto* input_arg : graph_inputs_including_initializers_) {
    if (name_to_initial_tensor_.count(input_arg->Name()) == 0) {
      graph_inputs_excluding_initializers_.push_back(input_arg);
    }
  }

  ComputeOverridableInitializers();

  ORT_RETURN_IF_ERROR(add_node_args(fbs_graph.outputs(), graph_outputs_));

#if !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD)
  // populate NodeArg lookups after loading Nodes and NodeArgs
  ORT_RETURN_IF_ERROR(PopulateNodeArgToProducerConsumerLookupsFromNodes());

  // runtime optimizations
  if (!load_options.ignore_saved_runtime_optimizations) {
    if (const auto* fbs_runtime_optimizations = fbs_graph.runtime_optimizations()) {
      if (const auto* fbs_runtime_optimization_records = fbs_runtime_optimizations->records()) {
        ORT_RETURN_IF_ERROR(MutableRuntimeOptimizations().LoadFromOrtFormat(*fbs_runtime_optimization_records));
      }
    }
  }
#endif  // !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD)

  return Status::OK();
}

#if !defined(ORT_MINIMAL_BUILD)
namespace {
ValueInfoProto ModelEditorValueInfoToOnnx(const onnxruntime::ModelEditorValueInfo& vi) {
  // the model builder API checks that the OrtValueInfo has a complete and valid OrtTypeInfo instance and that the
  // name is not null/empty.
  ORT_ENFORCE(vi.type_info->type == ONNX_TYPE_TENSOR,
              "Internal error. Model Editor API should only allow OrtValueInfo for tensor to be created.");

  ValueInfoProto value_info_proto;
  value_info_proto.set_name(vi.name);

  auto* tensor = value_info_proto.mutable_type()->mutable_tensor_type();
  const OrtTensorTypeAndShapeInfo& tensor_info = *vi.type_info->tensor_type_info.get();
  tensor->set_elem_type(tensor_info.type);

  auto& shape = *tensor->mutable_shape();

  size_t idx = 0;
  for (auto dim : tensor_info.shape.GetDims()) {
    auto& dim_proto = *shape.add_dim();
    if (dim >= 0) {
      dim_proto.set_dim_value(dim);
    } else {
      const std::string& dim_param = tensor_info.dim_params[idx];
      // if empty leave the new dim_proto with neither dim_value nor dim_param set. this represents an 'unknown' dim
      if (!dim_param.empty()) {
        dim_proto.set_dim_param(dim_param);
      }
    }
  }

  return value_info_proto;
}
}  // namespace

Status Graph::LoadFromModelEditorApiModel(const OrtGraph& api_graph, bool updating_existing_graph) {
  ArgNameToTypeMap name_to_type_map;

  // NOTE: need to create NodeArgs as we go along

  // add inputs first. the shape from an input for a non-const initializer is preferred, so we want to create the
  // NodeArg for the value using that

  auto add_graph_inputs_outputs = [&, this](
                                      const InlinedVector<std::unique_ptr<onnxruntime::ModelEditorValueInfo>>& graph_inputs_or_outputs,
                                      bool is_input) {
    // when updating a model we don't require the inputs or outputs to be set if they're unchanged.
    if (updating_existing_graph && graph_inputs_or_outputs.empty()) {
      return;
    }

    std::vector<const NodeArg*> node_args;
    node_args.reserve(graph_inputs_or_outputs.size());
    for (auto& ort_value_info : graph_inputs_or_outputs) {
      ValueInfoProto value_info = ModelEditorValueInfoToOnnx(*ort_value_info);

      name_to_type_map[value_info.name()] = value_info.type();
      node_args.push_back(&GetOrCreateNodeArg(value_info.name(), &value_info.type()));
    }

    if (is_input) {
      SetInputs(node_args);
    } else {
      SetOutputs(node_args);
    }
  };

  auto add_initializers = [this](const std::unordered_map<std::string, std::unique_ptr<OrtValue>>& initializers,
                                 bool is_external) {
    for (auto& name_and_ortvalue : initializers) {
      // convert from OrtValue to TensorProto
      const std::string& name = name_and_ortvalue.first;
      OrtValue& v = *name_and_ortvalue.second;

      ORT_ENFORCE(v.IsTensor(), "Initializers must be Tensors");
      const Tensor& t = v.Get<Tensor>();
      TensorProto& tensor_proto = *graph_proto_->add_initializer();

      tensor_proto.set_name(name);
      tensor_proto.set_data_type(t.GetElementType());
      for (auto dim : t.Shape().GetDims()) {
        tensor_proto.add_dims(dim);
      }

      if (is_external) {
        // pre-existing memory that we don't own. avoid a copy by storing the pointer in the ExternalDataInfo
        const void* data_offset = t.DataRaw();  // address of memory not offset into file
        auto offset = narrow<ExternalDataInfo::OFFSET_TYPE>(reinterpret_cast<intptr_t>(data_offset));

        ExternalDataInfo::SetExternalLocationToProto(onnxruntime::utils::kTensorProtoMemoryAddressTag,
                                                     offset, t.SizeInBytes(), tensor_proto);

        // add OrtValue to ortvalue_initializers_ to keep it alive and to store the deleter if provided.
        ortvalue_initializers_.emplace(name, std::move(v));
      } else {
        tensor_proto.set_raw_data(t.DataRaw(), t.SizeInBytes());
      }

      TypeProto type_proto{utils::TypeProtoFromTensorProto(tensor_proto)};
      ORT_IGNORE_RETURN_VALUE(GetOrCreateNodeArg(name, &type_proto));

      name_to_initial_tensor_.emplace(name, &tensor_proto);
    }
  };

  // process graph inputs first as we want the type/shape from them to be preferred if a graph input
  // has a matching initializer
  const auto* editor_graph = onnxruntime::ModelEditorGraph::ToInternal(&api_graph);
  ORT_RETURN_IF(editor_graph == nullptr, "Invalid OrtGraph variant for use in the model editor API.");

  add_graph_inputs_outputs(editor_graph->inputs, /*input*/ true);

  // add initializers
  ortvalue_initializers_.reserve(editor_graph->external_initializers.size());
  add_initializers(editor_graph->external_initializers, /*is_external*/ true);
  add_initializers(editor_graph->initializers, /*is_external*/ false);

  // add graph outputs
  add_graph_inputs_outputs(editor_graph->outputs, /*input*/ false);

  // add nodes
  for (const auto& editor_node : editor_graph->nodes) {
    const onnxruntime::ModelEditorNode& node = *editor_node;

    // convert Constant nodes to initializers
    if (node.operator_name == "Constant" && node.domain_name == kOnnxDomain) {
      // graph_proto_ provides storage
      TensorProto& tensor = *graph_proto_->add_initializer();

      // create NodeProto from OrtNode so we can use the existing conversion functions
      NodeProto node_proto;

      // 'Constant' node has no inputs or attributes
      ORT_RETURN_IF_NOT(node.input_names.empty() && node.attributes.size() == 1 && node.output_names.size() == 1,
                        node.node_name,
                        " is an invalid 'Constant' node. "
                        "Must have no inputs, one attribute and one output. ");

      node_proto.add_attribute()->CopyFrom(node.attributes[0]);
      node_proto.add_output(node.output_names[0]);

      node_proto.set_op_type(node.operator_name);
      node_proto.set_name(node.node_name);
      node_proto.set_domain(node.domain_name);

      ORT_RETURN_IF_ERROR(utils::ConstantNodeProtoToTensorProto(node_proto, /*model_path*/ "", tensor));
      name_to_initial_tensor_.emplace(node.output_names[0], &tensor);

      continue;
    }

    auto input_defs = CreateNodeArgs(node.input_names, name_to_type_map);
    auto output_defs = CreateNodeArgs(node.output_names, name_to_type_map);

    const auto num_attributes = node.attributes.size();

    NodeAttributes attributes;
    attributes.reserve(num_attributes);

    for (const auto& attr : node.attributes) {
      attributes[attr.name()] = attr;
    }

    ORT_IGNORE_RETURN_VALUE(AddNode(node.node_name, node.operator_name, /*doc_string*/ "",
                                    input_defs, output_defs, &attributes, node.domain_name));
  }

  return Resolve();
}

// static
Status Graph::LoadFromModelEditorApiModel(const OrtGraph& api_graph,
                                          const Model& owning_model,
                                          const std::unordered_map<std::string, int>& domain_to_version,
                                          IOnnxRuntimeOpSchemaCollectionPtr schema_registry,
                                          bool strict_shape_type_inference,
                                          const logging::Logger& logger,
                                          std::unique_ptr<Graph>& graph) {
  graph = std::make_unique<Graph>(owning_model,
                                  domain_to_version,
                                  schema_registry,
                                  /*parent_graph*/ nullptr, /*parent_node*/ nullptr,
                                  logger,
                                  strict_shape_type_inference);

  return graph->LoadFromModelEditorApiModel(api_graph);
}

Status Graph::UpdateUsingModelEditorApiModel(const OrtModel& api_model) {
  for (auto& entry : api_model.domain_to_version) {
    if (auto it = domain_to_version_.find(entry.first); it != domain_to_version_.end()) {
      if (it->second != entry.second) {
        return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                               "Domain version can not be changed for '", entry.first,
                               "'. Current version: ", it->second);
      }
    } else {
      domain_to_version_.insert(entry);
    }
  }

  // this will replace inputs/outputs and add nodes.
  return LoadFromModelEditorApiModel(*api_model.graph, /*updating_existing_graph*/ true);
}

#endif  // !defined(ORT_MINIMAL_BUILD)
}  // namespace onnxruntime
