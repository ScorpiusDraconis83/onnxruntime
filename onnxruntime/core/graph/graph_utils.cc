// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/graph/graph_utils.h"

#include "core/framework/tensorprotoutils.h"
#include "core/graph/graph.h"
#include "core/common/logging/logging.h"

#include <algorithm>
#include <queue>
#include <string>
#include <vector>

namespace onnxruntime {

namespace graph_utils {

//---------------------
//--- local helpers ---
//---------------------

#if !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD)

static int GetIndexFromName(const Node& node, const std::string& name, bool is_input) {
  const auto& node_args = is_input ? node.InputDefs() : node.OutputDefs();
  auto itr = std::find_if(node_args.begin(), node_args.end(),
                          [&name](const NodeArg* node_arg) { return node_arg->Name() == name; });
  ORT_ENFORCE(itr != node_args.end(),
              "Attempting to get index by a name which does not exist:", name, "for node: ", node.Name());
  auto index = std::distance(node_args.begin(), itr);
  return static_cast<int>(index);
}

#endif  // !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD)

#if !defined(ORT_MINIMAL_BUILD)

// check if an output edge provides an implicit input to the destination node
static bool OutputEdgeProvidesImplicitInput(const Graph& graph, const GraphEdge& output_edge) {
  // we treat the explicit and implicit inputs as sequential, so if the destination arg index of an output edge
  // is past the valid range for the node's explicit inputs, it is for an implicit input
  const size_t num_explicit_inputs = (*graph.GetNode(output_edge.dst_node)).InputDefs().size();
  return static_cast<size_t>(output_edge.dst_arg_index) >= num_explicit_inputs;
}

/** Checks if new_output_name can be used to replace removed_output_name in the subgraph input.
    If there is an existing NodeArg in a subgraph that implicitly consumes removed_output_name, it is not safe. */
static bool CanUpdateImplicitInputNameInSubgraph(const Node& node,
                                                 const std::string& removed_output_name,
                                                 const std::string& new_output_name) {
  if (!node.ContainsSubgraph())
    return true;

  for (const gsl::not_null<const Graph*>& subgraph : node.GetSubgraphs()) {
    // if we have an existing NodeArg in the subgraph with the new_output_name that would override an implicit input
    // with the same name
    if (subgraph->GetNodeArg(new_output_name) != nullptr) {
      return false;
    }

    for (auto& subgraph_node : subgraph->Nodes()) {
      // recurse if this node also consumes removed_output_name as an implicit input (i.e. there are multiple levels
      // of nested
      // subgraphs, and at least one level lower uses removed_output_name as an implicit input
      const auto subgraph_node_implicit_inputs = subgraph_node.ImplicitInputDefs();
      if (!subgraph_node_implicit_inputs.empty()) {
        auto subgraph_node_also_consumes_nodearg_as_implicit_input =
            std::find_if(subgraph_node_implicit_inputs.cbegin(), subgraph_node_implicit_inputs.cend(),
                         [&removed_output_name](const NodeArg* input) {
                           return input != nullptr && input->Name() == removed_output_name;
                         });

        if (subgraph_node_also_consumes_nodearg_as_implicit_input != subgraph_node_implicit_inputs.cend()) {
          if (!CanUpdateImplicitInputNameInSubgraph(subgraph_node, removed_output_name, new_output_name))
            return false;
        }
      }
    }
  }

  return true;
}

/** Updates removed_output_name with new_output_name in the subgraph input. */
static void UpdateImplicitInputNameInSubgraph(Node& node,
                                              const std::string& removed_output_name,
                                              const std::string& new_output_name) {
  for (auto& attr_subgraph_pair : node.GetAttributeNameToMutableSubgraphMap()) {
    Graph& subgraph = *attr_subgraph_pair.second;

    for (auto& subgraph_node : subgraph.Nodes()) {
      // recurse if this node also consumes removed_output_name as an implicit input
      // (i.e. there are multiple levels of nested subgraphs, and at least one level lower uses
      // removed_output_name as an implicit input
      const auto subgraph_node_implicit_inputs = subgraph_node.ImplicitInputDefs();
      if (!subgraph_node_implicit_inputs.empty()) {
        auto subgraph_node_also_consumes_nodearg_as_implicit_input =
            std::find_if(subgraph_node_implicit_inputs.cbegin(), subgraph_node_implicit_inputs.cend(),
                         [&removed_output_name](const NodeArg* input) {
                           return input->Name() == removed_output_name;
                         });

        if (subgraph_node_also_consumes_nodearg_as_implicit_input != subgraph_node_implicit_inputs.cend()) {
          UpdateImplicitInputNameInSubgraph(subgraph_node, removed_output_name, new_output_name);
        }
      }

      // Need mutable input defs to be able to update the implicit input names
      auto& input_args = subgraph_node.MutableInputDefs();

      if (!input_args.empty()) {
        int input_slot_index = -1;
        for (const auto* input_arg : input_args) {
          ++input_slot_index;
          // if the input matches, replace the NodeArg with one using the new name
          if (input_arg->Exists() && input_arg->Name() == removed_output_name) {
            // sanity check there was no edge for this input. implicit inputs from outer scope do not have edges
            ORT_ENFORCE(std::count_if(subgraph_node.InputEdgesBegin(), subgraph_node.InputEdgesEnd(),
                                      [input_slot_index](const Node::EdgeEnd& entry) {
                                        return entry.GetDstArgIndex() == input_slot_index;
                                      }) == 0);

            // Create a new NodeArg with the new name
            input_args[input_slot_index] = &attr_subgraph_pair.second->GetOrCreateNodeArg(new_output_name,
                                                                                          input_arg->TypeAsProto());
          }
        }
      }
    }
  }
}

/** Given a graph, a list of edges, and a NodeArg name, checks if each of the edges provides an implicit input
    to a subgraph. If so, it checks if there is no clash of the given NodeArg name in each of the subgraphs.
    This is important when removing a node with this NodeArg as input. */
static bool CanUpdateImplicitInputNameInSubgraphs(const Graph& graph,
                                                  const std::vector<GraphEdge>& output_edges,
                                                  const std::string& new_arg_name, const logging::Logger& logger) {
  for (const auto& output_edge : output_edges) {
    if (OutputEdgeProvidesImplicitInput(graph, output_edge)) {
      const Node& output_edge_node = *graph.GetNode(output_edge.dst_node);
      if (!CanUpdateImplicitInputNameInSubgraph(output_edge_node, output_edge.arg_name, new_arg_name)) {
        LOGS(logger, WARNING) << " Implicit input name " << output_edge.arg_name
                              << " cannot be safely updated to " << new_arg_name << " in one of the subgraphs.";
        return false;
      }
    }
  }

  return true;
}

/** Removes a node with a single incoming node and connects the incoming node with the output node/s.*/
static bool RemoveNodeWithSingleNodeInSingleUsedOutput(Graph& graph, Node& node) {
  // Store info for input and output edges.
  std::vector<GraphEdge> output_edges = GraphEdge::GetNodeOutputEdges(node);

  if (!output_edges.empty()) {
    // get non-const incoming Node
    const Node::EdgeEnd& input_edge = *node.InputEdgesBegin();
    Node& incoming_node = *graph.GetNode(input_edge.GetNode().Index());

    auto src_idx = output_edges.front().src_arg_index;
    ORT_ENFORCE(std::all_of(output_edges.cbegin(), output_edges.cend(),
                            [&src_idx](const GraphEdge& edge) {
                              return edge.src_arg_index == src_idx;
                            }),
                "Node must only have one used output");

    // replace the output edges from 'node' with an edge to node's incoming node
    ReplaceDownstreamNodeInput(graph, node, src_idx, incoming_node, input_edge.GetSrcArgIndex());
  }

  graph.RemoveNode(node.Index());

  return true;
}

void MoveAllNodeInputEdges(Graph& graph, Node& src_node, Node& target_node) {
  auto target_idx = target_node.Index();
  auto input_edges = GraphEdge::GetNodeInputEdges(src_node);

  for (auto cur = input_edges.cbegin(), end = input_edges.cend(); cur != end; ++cur) {
    auto target_arg_index = GetNodeInputIndexFromInputName(target_node, cur->arg_name);
    graph.AddEdge(cur->src_node, target_idx, cur->src_arg_index, target_arg_index);
  }

  GraphEdge::RemoveGraphEdges(graph, input_edges);
}

/** Move the output defs and edges from src_node to target_node.
After the move is complete src_node will have no output edges and can be safely removed by Graph::RemoveNode.
*/
static void MoveAllNodeOutputs(Graph& graph, Node& src_node, Node& target_node) {
  // copy the NodeArg*'s for all output defs.
  target_node.MutableOutputDefs() = src_node.MutableOutputDefs();

  auto target_idx = target_node.Index();
  auto output_edges = GraphEdge::GetNodeOutputEdges(src_node);

  for (auto cur = output_edges.cbegin(), end = output_edges.cend(); cur != end; ++cur) {
    graph.AddEdge(target_idx, cur->dst_node, cur->src_arg_index, cur->dst_arg_index);
  }

  GraphEdge::RemoveGraphEdges(graph, output_edges);
}

#endif  // !defined(ORT_MINIMAL_BUILD)

//----------------------------
//--- end of local helpers ---
//----------------------------

bool MatchesOpSinceVersion(const Node& node, std::initializer_list<ONNX_NAMESPACE::OperatorSetVersion> versions) {
  return std::find(versions.begin(), versions.end(), node.SinceVersion()) != versions.end();
}

bool MatchesOpSinceVersion(const Node& node, gsl::span<const ONNX_NAMESPACE::OperatorSetVersion> versions) {
  return std::find(versions.begin(), versions.end(), node.SinceVersion()) != versions.end();
}

bool MatchesOpSetDomain(const Node& node, std::string_view domain) {
  const auto& node_domain = node.Domain();
  return node_domain == domain;
}

bool CheckInMemoryDataMatch(const ONNX_NAMESPACE::TensorProto& tensor_proto, const Tensor& tensor) {
  if (utils::HasExternalData(tensor_proto)) {
    // Retrieve external data using ExternalData structure
    std::unique_ptr<ExternalDataInfo> external_data;
    ORT_THROW_IF_ERROR(ExternalDataInfo::Create(tensor_proto.external_data(), external_data));
    return (external_data->GetRelPath().compare(utils::kTensorProtoMemoryAddressTag) == 0) &&
           (tensor.DataRaw() == reinterpret_cast<const void*>(external_data->GetOffset()));
  }
  return false;
}

bool IsSupportedOptypeVersionAndDomain(const Node& node,
                                       std::string_view op_type,
                                       std::initializer_list<ONNX_NAMESPACE::OperatorSetVersion> versions,
                                       std::string_view domain) {
  return IsSupportedOptypeVersionAndDomain(node, op_type, gsl::span{versions.begin(), versions.size()}, domain);
}

bool IsSupportedOptypeVersionAndDomain(const Node& node, std::string_view op_type,
                                       gsl::span<const ONNX_NAMESPACE::OperatorSetVersion> versions,
                                       std::string_view domain) {
  return (node.OpType() == op_type &&
  // we don't have op schemas in the minimal build so there's no way to check the deprecated flag
#if !defined(ORT_MINIMAL_BUILD)
          !node.Op()->Deprecated() &&
#endif
          MatchesOpSinceVersion(node, versions) && MatchesOpSetDomain(node, domain));
}

#if !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD)

const ONNX_NAMESPACE::AttributeProto* GetNodeAttribute(const Node& node, const std::string& attr_name) {
  const auto& attrs = node.GetAttributes();
  const auto iter = attrs.find(attr_name);
  return iter == attrs.end() ? nullptr : &iter->second;
}

static NodeArg& GetOrCreateNodeArg(Graph& graph, const ONNX_NAMESPACE::TensorProto& new_initializer) {
  ONNX_NAMESPACE::TypeProto new_type;
  auto* typeproto_tensor = new_type.mutable_tensor_type();
  typeproto_tensor->set_elem_type(new_initializer.data_type());

  auto* shape = typeproto_tensor->mutable_shape();
  for (auto dim : new_initializer.dims()) {
    shape->add_dim()->set_dim_value(dim);
  }

  return graph.GetOrCreateNodeArg(new_initializer.name(), &new_type);
}

NodeArg& AddInitializer(Graph& graph, const ONNX_NAMESPACE::TensorProto& new_initializer) {
  // sanity check as AddInitializedTensor silently ignores attempts to add a duplicate initializer
  const ONNX_NAMESPACE::TensorProto* existing = nullptr;
  ORT_ENFORCE(!graph.GetInitializedTensor(new_initializer.name(), existing),
              "Initializer with same name exists. Name:", new_initializer.name());

  graph.AddInitializedTensor(new_initializer);
  return GetOrCreateNodeArg(graph, new_initializer);
}

NodeArg& AddInitializerWithExternalData(Graph& graph, const ONNX_NAMESPACE::TensorProto& new_initializer) {
  const bool has_external_data = utils::HasExternalData(new_initializer);
  ORT_ENFORCE(!has_external_data, "Expecting an initializer that contains data inline");

  Tensor tensor;
  ORT_THROW_IF_ERROR(utils::CreateTensorFromTensorProto(Env::Default(), graph.ModelPath(),
                                                        new_initializer, tensor));
  auto tensor_proto_with_ptr = utils::TensorToTensorProto(tensor, new_initializer.name(), true);
  return AddInitializerWithExternalData(graph, tensor_proto_with_ptr, std::move(tensor));
}

NodeArg& AddInitializerWithExternalData(Graph& graph, const ONNX_NAMESPACE::TensorProto& new_initializer,
                                        Tensor&& tensor) {
  OrtValue ort_value;
  if (utils::HasExternalDataInMemory(new_initializer)) {
    Tensor::InitOrtValue(std::move(tensor), ort_value);
  }

  ORT_THROW_IF_ERROR(graph.AddInitializedOrtValue(new_initializer, ort_value));
  return GetOrCreateNodeArg(graph, new_initializer);
}

NodeArg& AddInitializerWithExternalData(Graph& graph, const ONNX_NAMESPACE::TensorProto& new_initializer,
                                        OrtValue ort_value) {
  ORT_THROW_IF_ERROR(graph.AddInitializedOrtValue(new_initializer, ort_value));
  return GetOrCreateNodeArg(graph, new_initializer);
}

void MakeInitializerCopyIfNotExist(const Graph& src_graph, Graph& dst_graph, const std::string& name,
                                   bool copy_in_memory_data) {
  const ONNX_NAMESPACE::TensorProto* initializer = nullptr;
  if (src_graph.GetInitializedTensor(name, initializer)) {
    // check if the initializer already exists in the destination graph
    const ONNX_NAMESPACE::TensorProto* existing = nullptr;
    if (!dst_graph.GetInitializedTensor(name, existing)) {
      const bool data_in_memory = utils::HasExternalDataInMemory(*initializer);
      if (data_in_memory) {
        if (copy_in_memory_data) {
          ONNX_NAMESPACE::TensorProto tensor_proto;
          ORT_THROW_IF_ERROR(utils::TensorProtoWithExternalDataToTensorProto(*initializer, {}, tensor_proto));
          dst_graph.AddInitializedTensor(tensor_proto);
          GetOrCreateNodeArg(dst_graph, tensor_proto);
        } else {
          OrtValue ort_value;
          if (src_graph.GetOrtValueInitializer(name, ort_value)) {
            // add the initializer to the destination graph
            ORT_THROW_IF_ERROR(dst_graph.AddInitializedOrtValue(*initializer, ort_value));
          } else {
            // Data may be in memory, but stored in flatbuffers etc.
            dst_graph.AddInitializedTensor(*initializer);
          }
          GetOrCreateNodeArg(dst_graph, *initializer);
        }
      } else {
        dst_graph.AddInitializedTensor(*initializer);
        GetOrCreateNodeArg(dst_graph, *initializer);
      }
    }
  }
}

void MakeConstantInitializerCopyIfNotExist(const Graph& src_graph, Graph& dst_graph,
                                           const std::string& name, bool check_outer_scope) {
  const auto* initializer = src_graph.GetConstantInitializer(name, check_outer_scope);
  if (initializer != nullptr) {
    const ONNX_NAMESPACE::TensorProto* subgraph_initializer = nullptr;
    if (!dst_graph.GetInitializedTensor(name, subgraph_initializer)) {
      OrtValue ort_value;
      ORT_IGNORE_RETURN_VALUE(src_graph.GetOrtValueInitializer(name, ort_value, check_outer_scope));
      ORT_THROW_IF_ERROR(dst_graph.AddInitializedOrtValue(*initializer, ort_value));
    }
  }
}

Status ConvertInMemoryDataToInline(Graph& graph, const std::string& name) {
  const ONNX_NAMESPACE::TensorProto* initializer = nullptr;
  if (graph.GetInitializedTensor(name, initializer) && utils::HasExternalDataInMemory(*initializer)) {
    ONNX_NAMESPACE::TensorProto tensor_proto;
    ORT_THROW_IF_ERROR(utils::TensorProtoWithExternalDataToTensorProto(*initializer, {}, tensor_proto));
    graph.RemoveInitializedTensor(name);
    graph.AddInitializedTensor(tensor_proto);
  }
  return Status::OK();
}

int GetNodeOutputIndexFromOutputName(const Node& node, const std::string& output_name) {
  return GetIndexFromName(node, output_name, false);
}

std::vector<const Node*> FindParentsByType(const Node& node, const std::string& parent_type) {
  // find parents and sort them by destination argument index
  // as there is at most one input edge for each input argument,
  // there is no need of extra work like FindChildrenByType
  std::vector<const Node*> parents(node.InputDefs().size(), nullptr);
  for (auto it = node.InputEdgesBegin(); it != node.InputEdgesEnd(); it++) {
    if (it->GetNode().OpType().compare(parent_type) == 0) {
      parents[it->GetDstArgIndex()] = &(it->GetNode());
    }
  }

  // remove unmatched nodes
  parents.erase(std::remove(parents.begin(), parents.end(), nullptr), parents.end());
  return parents;
}

std::vector<const Node*> FindChildrenByType(const Node& node, const std::string& child_type) {
  // find children and sort them by source argument index:
  //     Create a 2D vector to hold the result.
  //     1st dimension index is output index,
  //     and the 2nd dimension stores the edges from the output.
  std::vector<std::vector<const Node*>> children(node.OutputDefs().size(), std::vector<const Node*>());
  for (auto it = node.OutputEdgesBegin(); it != node.OutputEdgesEnd(); it++) {
    if (it->GetNode().OpType().compare(child_type) == 0) {
      children[it->GetSrcArgIndex()].push_back(&(it->GetNode()));
    }
  }

  // aggregate children
  std::vector<const Node*> agg_res;
  for (size_t output_idx = 0; output_idx < children.size(); output_idx++) {
    agg_res.insert(agg_res.end(), children[output_idx].begin(), children[output_idx].end());
  }
  return agg_res;
}

const std::string& GetNodeInputName(const Node& node, int index) {
  const auto& inputs = node.InputDefs();
  ORT_ENFORCE(index >= 0 && static_cast<size_t>(index) < inputs.size(),
              "Attempting to get an input that does not exist.");
  return inputs[index]->Name();
}

const std::string& GetNodeOutputName(const Node& node, int index) {
  const auto& outputs = node.OutputDefs();
  ORT_ENFORCE(index >= 0 && static_cast<size_t>(index) < outputs.size(),
              "Attempting to get an output that does not exist.");
  return outputs[index]->Name();
}

size_t RemoveNodeOutputEdges(Graph& graph, Node& node) {
  std::vector<GraphEdge> output_edges = GraphEdge::GetNodeOutputEdges(node);
  GraphEdge::RemoveGraphEdges(graph, output_edges);

  return output_edges.size();
}

size_t RemoveNodeOutputEdges(Graph& graph, Node& node, int output_idx) {
  std::vector<GraphEdge> output_edges = GraphEdge::GetNodeOutputEdges(node, output_idx);
  GraphEdge::RemoveGraphEdges(graph, output_edges);

  return output_edges.size();
}

const ONNX_NAMESPACE::TensorProto* GetConstantInitializer(const Graph& graph, const std::string& initializer_name,
                                                          bool check_outer_scope) {
  return graph.GetConstantInitializer(initializer_name, check_outer_scope);
}

GraphEdge::GraphEdge(NodeIndex src_node,
                     NodeIndex dst_node,
                     int src_arg_index,
                     int dst_arg_index,
                     const std::string& arg_name) : src_node(src_node),
                                                    dst_node(dst_node),
                                                    src_arg_index(src_arg_index),
                                                    dst_arg_index(dst_arg_index),
                                                    arg_name(arg_name) {}

// Constructs a GraphEdge given a node, an edge_end, and a boolean for the edge direction.
GraphEdge GraphEdge::CreateGraphEdge(const Node& node, const Node::EdgeEnd& edge_end, bool is_input_edge) {
  return is_input_edge
             ? GraphEdge(edge_end.GetNode().Index(),
                         node.Index(),
                         edge_end.GetSrcArgIndex(),
                         edge_end.GetDstArgIndex(),
                         GetNodeInputName(node, edge_end.GetDstArgIndex()))
             : GraphEdge(node.Index(),
                         edge_end.GetNode().Index(),
                         edge_end.GetSrcArgIndex(),
                         edge_end.GetDstArgIndex(),
                         GetNodeOutputName(node, edge_end.GetSrcArgIndex()));
}

const Node::EdgeEnd* GetInputEdge(const Node& node, int arg_index) {
  for (auto it = node.InputEdgesBegin(), end = node.InputEdgesEnd(); it != end; ++it) {
    if (arg_index == it->GetDstArgIndex()) {
      return &(*it);
    }
  }
  return nullptr;
}

/** Returns a vector of the input GraphEdges of a node. */
std::vector<GraphEdge> GraphEdge::GetNodeInputEdges(const Node& node) {
  std::vector<GraphEdge> input_edges;
  for (auto it = node.InputEdgesBegin(), end = node.InputEdgesEnd(); it != end; ++it) {
    input_edges.push_back(GraphEdge::CreateGraphEdge(node, *it, true));
  }

  return input_edges;
}

/** Returns a vector of the input GraphEdges of a node for the provided input index. */
std::vector<GraphEdge> GraphEdge::GetNodeInputEdges(const Node& node, size_t index) {
  std::vector<GraphEdge> input_edges;
  for (auto it = node.InputEdgesBegin(), end = node.InputEdgesEnd(); it != end; ++it) {
    if (static_cast<size_t>(it->GetDstArgIndex()) == index) {
      input_edges.push_back(GraphEdge::CreateGraphEdge(node, *it, true));
    }
  }

  return input_edges;
}

/** Returns a vector of the output GraphEdges of a node. */
std::vector<GraphEdge> GraphEdge::GetNodeOutputEdges(const Node& node) {
  std::vector<GraphEdge> output_edges;
  for (auto it = node.OutputEdgesBegin(), end = node.OutputEdgesEnd(); it != end; ++it) {
    output_edges.push_back(GraphEdge::CreateGraphEdge(node, *it, false));
  }

  return output_edges;
}

/** Returns a vector of output GraphEdges of a node for the provided output index. */
std::vector<GraphEdge> GraphEdge::GetNodeOutputEdges(const Node& node, size_t index) {
  std::vector<GraphEdge> output_edges;
  for (auto it = node.OutputEdgesBegin(), end = node.OutputEdgesEnd(); it != end; ++it) {
    if (static_cast<size_t>(it->GetSrcArgIndex()) == index) {
      output_edges.push_back(GraphEdge::CreateGraphEdge(node, *it, false));
    }
  }

  return output_edges;
}

/** Removes a set of GraphEdges from the graph. */
void GraphEdge::RemoveGraphEdges(Graph& graph, const std::vector<GraphEdge>& edges) {
  for (const auto& edge_to_remove : edges) {
    graph.RemoveEdge(edge_to_remove.src_node,
                     edge_to_remove.dst_node,
                     edge_to_remove.src_arg_index,
                     edge_to_remove.dst_arg_index);
  }
}

bool IsSupportedProvider(const Node& node,
                         const InlinedHashSet<std::string_view>& compatible_providers) {
  return !(!compatible_providers.empty() &&
           compatible_providers.find(node.GetExecutionProviderType()) == compatible_providers.end());
}

#endif  // !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD)

#if !defined(ORT_MINIMAL_BUILD)

int GetNodeInputIndexFromInputName(const Node& node, const std::string& input_name) {
  return GetIndexFromName(node, input_name, true);
}

/** Checks for nodes with >= 1 outputs, if only one of the outputs is input to downstream Operators.
Returns the name of the single used output in output_name. */
static bool IsOnlyOneOutputUsed(const Graph& graph, const Node& node, const std::string*& output_name) {
  constexpr int unassigned = -1;
  int first_output = unassigned;

  // check that there are only edges for one output, and set the output_name
  if (node.GetOutputEdgesCount() > 0) {
    for (auto it = node.OutputEdgesBegin(), end = node.OutputEdgesEnd(); it != end; ++it) {
      if (first_output == unassigned) {
        first_output = it->GetSrcArgIndex();
      } else if (first_output != it->GetSrcArgIndex()) {
        return false;
      }
    }

    output_name = &node.OutputDefs()[first_output]->Name();
  }

  // outputs could also be direct graph outputs so check if there are any graph outputs that
  // a) there's only 1, and b) it's the same as any output consumed by another node
  auto output_indexes = graph.GetNodeOutputsInGraphOutputs(node);
  auto num_graph_outputs = output_indexes.size();
  if (num_graph_outputs > 1) {
    return false;
  } else if (num_graph_outputs == 1) {
    if (first_output != unassigned) {
      // an output is consumed by other nodes, so make sure the same output is providing the graph output
      return output_indexes.front() == first_output;
    } else {
      // graph output only as no other nodes are consuming the output, so just update the output_name
      output_name = &node.OutputDefs()[output_indexes.front()]->Name();
    }
  }

  return true;
}

bool IsOutputUsed(const Node& node, int index) {
  for (auto it = node.OutputEdgesBegin(), end = node.OutputEdgesEnd(); it != end; ++it) {
    if (it->GetSrcArgIndex() == index) {
      return true;
    }
  }
  return false;
}

bool CanRemoveNode(const Graph& graph, const Node& node, const logging::Logger& logger) {
  const std::string* output_name = nullptr;
  if (!IsOnlyOneOutputUsed(graph, node, output_name)) {
    return false;
  }

  // TODO: Currently we remove the node and use the input name from the node being removed.
  // It may also be possible to instead update an upstream node to use the output name from the node being removed.
  // This would allow removal of a node that is providing a graph output, as that output name would come from updating
  // the upstream node. This should also enable removal if CanUpdateImplicitInputNameInSubgraphs returns false.

  if (graph.NodeProducesGraphOutput(node)) {
    return false;
  }

  bool can_remove = false;
  const std::string* new_name = nullptr;

  if (node.GetInputEdgesCount() == 1) {
    // we will merge the single input edge with the edges for the output that is used
    // Note that the node may have other inputs coming from initializers or graph inputs that do not have edges.
    new_name = &GetNodeInputName(node, node.InputEdgesBegin()->GetDstArgIndex());
  } else if (node.InputDefs().size() == 1) {
    // we can also handle a node with a single input from an initializer or graph input (no edges)
    new_name = &node.InputDefs()[0]->Name();
  } else {
    // No other node removal is supported
  }

  if (new_name) {
    // Check that changing the current output name to the new name won't break any subgraphs that consume it
    std::vector<GraphEdge> output_edges = GraphEdge::GetNodeOutputEdges(node);
    can_remove = CanUpdateImplicitInputNameInSubgraphs(graph, output_edges, *new_name, logger);
  }

  return can_remove;
}

bool RemoveNode(Graph& graph, Node& node) {
  // TODO: enable the check back
  // assert(CanRemoveNode(graph, node, nullptr));

  // Note: Node does not produce any graph outputs, and only a single output is used.

  // If there is a single input edge from another node (initializers are not connected with edges to nodes)
  if (node.GetInputEdgesCount() == 1) {
    // remove the node and wire its incoming node to its outgoing node/s
    return RemoveNodeWithSingleNodeInSingleUsedOutput(graph, node);
  }

  // single input def so replace node with that
  if (node.InputDefs().size() == 1) {
    return ReplaceNodeWithInitializer(graph, node, *node.MutableInputDefs()[0]);
  }

  ORT_THROW("Should be unreachable if CanRemoveNodeAndMergeEdges is in sync with the logic here.");
}

bool CanReplaceNodeWithInitializer(const Graph& graph, const Node& node, const std::string& initializer_name,
                                   const logging::Logger& logger) {
  // we have no way to handle replacing multiple outputs so check only one is used
  const std::string* output_name = nullptr;
  if (!IsOnlyOneOutputUsed(graph, node, output_name) || output_name == nullptr) {
    return false;
  }

  bool output_name_is_changing = *output_name != initializer_name;

  auto num_graph_outputs = graph.GetNodeOutputsInGraphOutputs(node).size();
  if (num_graph_outputs > 0) {
    // Cannot remove a node that provides more than one graph output,
    // or a node whose single graph output is not being replaced by an initializer with the same name
    if (num_graph_outputs > 1 || output_name_is_changing) {
      return false;
    }
  }

  bool can_remove = true;

  if (output_name_is_changing) {
    // Check that changing the current output name to the new name won't break any subgraphs
    // that consume the current name
    std::vector<GraphEdge> output_edges = GraphEdge::GetNodeOutputEdges(node);
    can_remove = CanUpdateImplicitInputNameInSubgraphs(graph, output_edges, initializer_name, logger);
  }

  return can_remove;
}

bool ReplaceNodeWithInitializer(Graph& graph, Node& node, NodeArg& replacement) {
  // We have to remove the output edges before we create replacement ones, so save the current output edge information
  std::vector<GraphEdge> output_edges = GraphEdge::GetNodeOutputEdges(node);

  // Remove the output edges of the node and then the node (this will remove any input edges).
  RemoveNodeOutputEdges(graph, node);
  graph.RemoveNode(node.Index());

  // Re-create the output edges using 'replacement' as the source NodeArg (input) to the destination node/s
  for (auto& output_edge : output_edges) {
    // Take care of subgraph inputs.
    if (OutputEdgeProvidesImplicitInput(graph, output_edge)) {
      Node& mutable_output_edge_node = *graph.GetNode(output_edge.dst_node);
      UpdateImplicitInputNameInSubgraph(mutable_output_edge_node, output_edge.arg_name,
                                        replacement.Name());
    }

    // Replace outgoing node's input.
    auto& output_node = *graph.GetNode(output_edge.dst_node);
    ReplaceNodeInput(output_node, output_edge.dst_arg_index, replacement);
  }

  return true;
}

bool IsGraphInput(const Graph& graph, const NodeArg* input) {
  const std::vector<const NodeArg*>& graph_inputs = graph.GetInputsIncludingInitializers();
  return std::find(graph_inputs.begin(), graph_inputs.end(), input) != graph_inputs.end();
}

bool IsGraphOutput(const Graph& graph, const NodeArg* output) {
  const auto& graph_outputs = graph.GetOutputs();
  return std::find(graph_outputs.begin(), graph_outputs.end(), output) != graph_outputs.end();
}

bool IsInitializer(const Graph& graph, const std::string& name, bool check_outer_scope) {
  bool is_initializer = false;
  const ONNX_NAMESPACE::TensorProto* initializer = nullptr;
  if (graph.GetInitializedTensor(name, initializer)) {
    is_initializer = true;
  } else if (check_outer_scope && graph.IsSubgraph() && graph.IsOuterScopeValue(name)) {
    is_initializer = IsInitializer(*graph.ParentGraph(), name, check_outer_scope);
  }

  return is_initializer;
}

bool IsConstantInitializer(const Graph& graph, const std::string& initializer_name, bool check_outer_scope) {
  const ONNX_NAMESPACE::TensorProto* initializer = GetConstantInitializer(graph, initializer_name, check_outer_scope);
  return initializer != nullptr;
}

bool NodeArgIsConstant(const Graph& graph, const NodeArg& node_arg) {
  return IsConstantInitializer(graph, node_arg.Name(), true);
}

bool AllNodeInputsAreConstant(const Graph& graph, const Node& node, InitializedTensorSet& constant_inputs,
                              const InlinedHashSet<std::string>& excluded_initializers) {
  // clear so we have a known state. if we fail part way through we go back to this state.
  constant_inputs.clear();

  // only initializers can be constant. There's no edge from a node to an initializer
  // so the input edges count will be 0 if all the inputs are initializers.
  if (node.GetInputEdgesCount() > 0) {
    return false;
  }

  for (const auto* input_def : node.InputDefs()) {
    // For optional node inputs which are missing, we can safely ignore them
    if (input_def->Name().empty()) {
      continue;
    }

    // Important note: when an initializer appears in the graph's input, this input will not be considered constant,
    // because it can be overridden by the user at runtime. For constant folding to be applied, the initializer should
    // not appear in the graph's inputs (that is the only way to guarantee it will always be constant).
    const ONNX_NAMESPACE::TensorProto* initializer = GetConstantInitializer(graph, input_def->Name(), true);
    if (initializer && excluded_initializers.find(input_def->Name()) == excluded_initializers.cend()) {
      constant_inputs.insert({input_def->Name(), initializer});
    } else {
      constant_inputs.clear();
      return false;
    }
  }

  return true;
}

const Node* FirstChildByType(const Node& node, const std::string& child_type) {
  for (auto it = node.OutputNodesBegin(); it != node.OutputNodesEnd(); ++it) {
    if ((*it).OpType().compare(child_type) == 0) {
      return &(*it);
    }
  }
  return nullptr;
}

const Node* FirstParentByType(const Node& node, const std::string& parent_type) {
  for (auto it = node.InputNodesBegin(); it != node.InputNodesEnd(); ++it) {
    if ((*it).OpType().compare(parent_type) == 0) {
      return &(*it);
    }
  }
  return nullptr;
}

void ReplaceDownstreamNodeInput(Graph& graph, Node& node, int output_idx, Node& replacement,
                                int replacement_output_idx) {
  // get the output edges from node for output_idx
  std::vector<GraphEdge> output_edges = GraphEdge::GetNodeOutputEdges(node, output_idx);

  if (!output_edges.empty()) {
    const auto& replacement_name = replacement.MutableOutputDefs()[replacement_output_idx]->Name();

    // Remove the output edges of the node first
    GraphEdge::RemoveGraphEdges(graph, output_edges);

    // Create connections between the replacement node and the outgoing nodes
    for (const auto& output_edge : output_edges) {
      // Take care of subgraph inputs.
      if (OutputEdgeProvidesImplicitInput(graph, output_edge)) {
        Node& mutable_output_edge_node = *graph.GetNode(output_edge.dst_node);
        UpdateImplicitInputNameInSubgraph(mutable_output_edge_node, output_edge.arg_name, replacement_name);
      }

      // Add new edge connecting the input with the output nodes directly.
      // This also updates the destination node's input node args
      graph.AddEdge(replacement.Index(), output_edge.dst_node, replacement_output_idx, output_edge.dst_arg_index);
    }
  }
}

void ReplaceNodeInput(Node& target, int target_input_idx, NodeArg& new_input) {
  size_t dst_arg_idx = static_cast<size_t>(target_input_idx);
  auto num_explicit_inputs = target.InputDefs().size();

  if (dst_arg_idx < num_explicit_inputs) {
    target.MutableInputDefs()[target_input_idx] = &new_input;
  } else if (dst_arg_idx < num_explicit_inputs + target.ImplicitInputDefs().size()) {
    // If we need to update an implicit input.
    target.MutableImplicitInputDefs()[dst_arg_idx - num_explicit_inputs] = &new_input;
  } else {
    // logic error in our code
    ORT_THROW("Invalid input index for node ", target.Name(), ". Index:", target_input_idx,
              " ExplicitInputs:", num_explicit_inputs,
              " ImplicitInputs:", target.ImplicitInputDefs().size());
  }
}

void AddNodeInput(Node& target, int target_input_idx, NodeArg& new_input) {
  auto num_explicit_inputs = target.InputDefs().size();
  ORT_ENFORCE(num_explicit_inputs == static_cast<size_t>(target_input_idx),
              "Can only add a new input at the end of the current ones.");

  target.MutableInputDefs().push_back(&new_input);

  // expect existing entry for all possible inputs
  assert(target.MutableInputArgsCount().size() > static_cast<size_t>(target_input_idx));
  target.MutableInputArgsCount()[target_input_idx] = 1;
}

void FinalizeNodeFusion(Graph& graph, Node& first_node, Node& second_node) {
  // move the outputs from second_node to first_node
  RemoveNodeOutputEdges(graph, first_node);
  MoveAllNodeOutputs(graph, second_node, first_node);

  // second node now has no output edges and can be removed
  graph.RemoveNode(second_node.Index());
}

void FinalizeNodeFusion(Graph& graph, gsl::span<const std::reference_wrapper<Node>> nodes, Node& replacement_node_start,
                        Node& replacement_node_end) {
  MoveAllNodeInputEdges(graph, *nodes.begin(), replacement_node_start);
  MoveAllNodeOutputs(graph, nodes.back(), replacement_node_end);

  for (Node& node : nodes) {
    RemoveNodeOutputEdges(graph, node);
    graph.RemoveNode(node.Index());
  }
}

const Node* GetInputNode(const Node& node, int arg_index) {
  const Node::EdgeEnd* edge = GetInputEdge(node, arg_index);
  if (nullptr == edge) {
    return nullptr;
  }
  return &(edge->GetNode());
}

inline std::string ToString(gsl::span<const ONNX_NAMESPACE::OperatorSetVersion> versions) {
  std::ostringstream output;
  if (!versions.empty()) {
    // Convert all but the last element to avoid a trailing ";"
    std::copy(versions.begin(), versions.end() - 1,
              std::ostream_iterator<ONNX_NAMESPACE::OperatorSetVersion>(output, ";"));
    // Now add the last element with no delimiter
    output << versions.back();
  }
  return output.str();
}

bool FindPath(const Node& node, bool is_input_edge, gsl::span<const EdgeEndToMatch> edges_to_match,
              std::vector<const Node::EdgeEnd*>& result, const logging::Logger& logger) {
  result.clear();
  result.reserve(edges_to_match.size());

  const Node* current_node = &node;
  for (const auto& edge : edges_to_match) {
    const Node::EdgeEnd* edge_found = nullptr;
#ifndef NDEBUG
    LOGS(logger, VERBOSE) << (is_input_edge ? "I:" : "O:") << edge.src_arg_index << "," << edge.dst_arg_index
                          << "," << edge.op_type << "," << edge.domain << "," << ToString(edge.versions);
#endif
    auto edges_begin = is_input_edge ? current_node->InputEdgesBegin() : current_node->OutputEdgesBegin();
    auto edges_end = is_input_edge ? current_node->InputEdgesEnd() : current_node->OutputEdgesEnd();
    for (auto it = edges_begin; it != edges_end; ++it) {
#ifndef NDEBUG
      LOGS(logger, VERBOSE) << "E:" << it->GetSrcArgIndex() << "," << it->GetDstArgIndex()
                            << "," << it->GetNode().OpType() << "," << it->GetNode().Domain() << ","
                            << it->GetNode().SinceVersion();
#endif
      if (edge.dst_arg_index == it->GetDstArgIndex() &&
          edge.src_arg_index == it->GetSrcArgIndex() &&
          edge.op_type == it->GetNode().OpType() &&
          MatchesOpSinceVersion(it->GetNode(), edge.versions) &&
          MatchesOpSetDomain(it->GetNode(), edge.domain)) {
        // For output edge, there could be multiple edges matched.
        // This function will return failure in such case by design.
        if (nullptr != edge_found) {
          LOGS(logger, WARNING) << "Failed since multiple edges matched:" << current_node->OpType() << "->"
                                << edge.op_type;
          return false;
        }
        edge_found = &(*it);

        // For input edge, each dst_arg_index only accepts one input edge so only there is at most one match.
        if (is_input_edge) {
          break;
        }
      }
    }

    if (nullptr == edge_found) {
      return false;
    }

    result.push_back(edge_found);
    current_node = &(edge_found->GetNode());
  }

  return true;
}

bool FindPath(Graph& graph, const Node& node, bool is_input_edge, gsl::span<const EdgeEndToMatch> edges_to_match,
              std::vector<std::reference_wrapper<Node>>& result, const logging::Logger& logger) {
  result.clear();

  std::vector<const Node::EdgeEnd*> edge_ends;
  if (!FindPath(node, is_input_edge, edges_to_match, edge_ends, logger)) {
    return false;
  }

  result.reserve(edges_to_match.size());
  std::transform(edge_ends.begin(), edge_ends.end(), std::back_inserter(result),
                 [&graph](const Node::EdgeEnd* edge_end) -> Node& {
                   return *graph.GetNode(edge_end->GetNode().Index());
                 });

  return true;
}

bool RemoveNodesWithOneOutputBottomUp(Graph& graph, const Node& start_node) {
  std::queue<NodeIndex> q;
  InlinedHashSet<NodeIndex> removed_nodes;

  NodeIndex start_node_index = start_node.Index();
  q.push(start_node_index);

  // From the current node, remove nodes bottom-up util it reaches a node with multiple outputs/graph output.
  while (!q.empty()) {
    NodeIndex cur_node_index = q.front();
    q.pop();

    if (removed_nodes.find(cur_node_index) != removed_nodes.end()) {
      continue;
    }
    // Each eligible node in the subgraph must have less than one output edge and no output should be
    // the graph output
    const Node& cur_node = *graph.GetNode(cur_node_index);
    if (cur_node.GetOutputEdgesCount() > 1 || graph.NodeProducesGraphOutput(cur_node)) {
      continue;
    }

    // push the parents of current node to the queue.
    for (size_t i = 0; i < cur_node.InputDefs().size(); ++i) {
      const std::string& input_name = GetNodeInputName(cur_node, static_cast<int>(i));
      if (IsInitializer(graph, input_name, true) || IsGraphInput(graph, cur_node.InputDefs()[static_cast<int>(i)])) {
        // skip initializers and graph inputs
        continue;
      }
      const Node* parent_node = GetInputNode(cur_node, static_cast<int>(i));
      if (nullptr == parent_node) {
        continue;
      }
      q.push(parent_node->Index());
    }

    if (cur_node_index == start_node_index || cur_node.GetOutputEdgesCount() == 0) {
      Node* cur_node_p = graph.GetNode(cur_node_index);
      RemoveNodeOutputEdges(graph, *cur_node_p);
      graph.RemoveNode(cur_node_index);

      removed_nodes.insert(cur_node_index);
    }
  }

  if (removed_nodes.size() == 0) {
    // Nothing to remove
    return false;
  }

  return true;
}

NodeArg& CreateNodeArg(Graph& graph, const NodeArg& base_arg) {
  return graph.GetOrCreateNodeArg(graph.GenerateNodeArgName(base_arg.Name()), base_arg.TypeAsProto());
}

#endif  // !defined(ORT_MINIMAL_BUILD)

}  // namespace graph_utils
}  // namespace onnxruntime
