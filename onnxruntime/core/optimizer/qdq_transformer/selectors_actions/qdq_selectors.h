// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#if !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD)

#include <utility>
#include "core/framework/node_unit.h"
#include "core/optimizer/selectors_actions/selector_action_transformer.h"

namespace onnxruntime {
class Graph;
class Node;

namespace QDQ {

class NodeGroupSelector {
 public:
  // This is a QDQ Selectors only function, will return QDQ::NodeGroup instead of NodesToOptimizeIndices
  // Can be used in QDQ handling in EPs such as NNAPI
  std::optional<NodeGroup> GetQDQSelection(const GraphViewer& graph_viewer, const Node& node) const;

  virtual ~NodeGroupSelector() = default;

 protected:
  // base check that we have the expected number of QDQ inputs/outputs, and `node` isn't producing a graph output.
  // num_dq_inputs defaults to the number of inputs `node` has if not explicitly specified
  bool CheckQDQNodes(const GraphViewer& graph_viewer, const Node& node, const Node* redundant_clip_node,
                     const std::vector<const Node*>& dq_nodes,
                     const std::vector<const Node*>& q_nodes,
                     int num_dq_inputs = -1,
                     bool is_empty_q_nodes_allowed = false) const;

 private:
  // derived classes should implement this check
  bool virtual Check(const GraphViewer& graph_viewer, const Node& node, const Node* redundant_clip_node,
                     const std::vector<const Node*>& dq_nodes,
                     const std::vector<const Node*>& q_nodes) const = 0;
};

/*
 * NodeGroup selectors. These are general purpose and used in both the QDQ SelectorActionTransformer setup that the
 * CPU EP has, and directly in compiling EPs such as NNAPI and CoreML.
 */

// Single DQ -> node that does not change data -> Q.
// Zero point and scale are constant scalars and must match
class DropQDQNodeGroupSelector : public NodeGroupSelector {
 public:
  explicit DropQDQNodeGroupSelector(bool allow_16bit = true, bool allow_4bit = true,
                                    bool allow_nonpositive_scale = true)
      : allow_16bit_(allow_16bit), allow_4bit_(allow_4bit), allow_nonpositive_scale_(allow_nonpositive_scale) {}

 private:
  bool Check(const GraphViewer& graph_viewer, const Node& node, const Node* redundant_clip_node,
             const std::vector<const Node*>& dq_nodes,
             const std::vector<const Node*>& q_nodes) const override;

  bool allow_16bit_;
  bool allow_4bit_;
  bool allow_nonpositive_scale_;
};

// Single DQ -> node.
class DropDQNodeGroupSelector : public NodeGroupSelector {
 public:
  explicit DropDQNodeGroupSelector(bool allow_16bit = true, bool allow_4bit = true)
      : allow_16bit_(allow_16bit), allow_4bit_(allow_4bit) {}

 private:
  bool Check(const GraphViewer& graph_viewer, const Node& node, const Node* redundant_clip_node,
             const std::vector<const Node*>& dq_nodes,
             const std::vector<const Node*>& q_nodes) const override;

  bool allow_16bit_;
  bool allow_4bit_;
};

// single input. default is to only support uint8.
class UnaryNodeGroupSelector : public NodeGroupSelector {
 public:
  explicit UnaryNodeGroupSelector(bool allow_16bit = true, bool allow_4bit = true)
      : allow_16bit_(allow_16bit), allow_4bit_(allow_4bit) {}

 private:
  bool Check(const GraphViewer& graph_viewer, const Node& node, const Node* redundant_clip_node,
             const std::vector<const Node*>& dq_nodes,
             const std::vector<const Node*>& q_nodes) const override;

  bool allow_16bit_;
  bool allow_4bit_;
};

// 2 DQ nodes providing input -> node -> Q
class BinaryNodeGroupSelector : public NodeGroupSelector {
 public:
  explicit BinaryNodeGroupSelector(bool allow_16bit = true, bool allow_4bit = true)
      : allow_16bit_(allow_16bit), allow_4bit_(allow_4bit) {}

 private:
  bool Check(const GraphViewer& graph_viewer, const Node& node, const Node* redundant_clip_node,
             const std::vector<const Node*>& dq_nodes,
             const std::vector<const Node*>& q_nodes) const override;

  bool allow_16bit_;
  bool allow_4bit_;
};

// Variadic DQ nodes -> node -> Q
class VariadicNodeGroupSelector : public NodeGroupSelector {
 public:
  explicit VariadicNodeGroupSelector(bool allow_16bit = true, bool allow_4bit = true)
      : allow_16bit_(allow_16bit), allow_4bit_(allow_4bit) {}

 private:
  bool Check(const GraphViewer& graph_viewer, const Node& node, const Node* redundant_clip_node,
             const std::vector<const Node*>& dq_nodes,
             const std::vector<const Node*>& q_nodes) const override;

  bool allow_16bit_;
  bool allow_4bit_;
};

// DQ node -> Split -> multiple Q nodes with equal quantization types.
// Optionally, the selector can require all input and output quantization parameters to be
// equal and constant.
class SplitNodeGroupSelector : public NodeGroupSelector {
 public:
  explicit SplitNodeGroupSelector(bool req_equal_quant_params = false, bool allow_4bit = true)
      : req_equal_quant_params_(req_equal_quant_params), allow_4bit_(allow_4bit) {}

 private:
  bool Check(const GraphViewer& graph_viewer, const Node& node, const Node* redundant_clip_node,
             const std::vector<const Node*>& dq_nodes,
             const std::vector<const Node*>& q_nodes) const override;

  bool req_equal_quant_params_;  // If true, only selects a node group if the input and output
                                 // quantization parameters are all equal/constant, which enables the
                                 // optimizer to drop the Q/DQ ops if the group is assigned to the CPU EP.
  bool allow_4bit_;
};

// DQ nodes for X, W and optionally B -> node -> Q
class ConvNodeGroupSelector : public NodeGroupSelector {
 public:
  // default to 'true'
  ConvNodeGroupSelector(bool int8_allowed = true, bool allow_16bit = true, bool allow_4bit_weight = true)
      : int8_allowed_(int8_allowed), allow_16bit_(allow_16bit), allow_4bit_weight_(allow_4bit_weight) {}

 private:
  bool Check(const GraphViewer& graph_viewer, const Node& node, const Node* redundant_clip_node,
             const std::vector<const Node*>& dq_nodes,
             const std::vector<const Node*>& q_nodes) const override;

  bool int8_allowed_;
  bool allow_16bit_;
  bool allow_4bit_weight_;
};

class WhereNodeGroupSelector : public NodeGroupSelector {
 public:
  explicit WhereNodeGroupSelector(bool allow_16bit = true, bool allow_4bit = true)
      : allow_16bit_(allow_16bit), allow_4bit_(allow_4bit) {}

 private:
  bool Check(const GraphViewer& graph_viewer, const Node& node, const Node* redundant_clip_node,
             const std::vector<const Node*>& dq_nodes,
             const std::vector<const Node*>& q_nodes) const override;

  bool allow_16bit_;
  bool allow_4bit_;
};

class PadNodeGroupSelector : public NodeGroupSelector {
 public:
  PadNodeGroupSelector() = default;

 private:
  bool Check(const GraphViewer& graph_viewer, const Node& node, const Node* redundant_clip_node,
             const std::vector<const Node*>& dq_nodes,
             const std::vector<const Node*>& q_nodes) const override;
};

// one ore more DQ nodes for each input -> node -> Q
class EinsumNodeGroupSelector : public NodeGroupSelector {
 public:
  explicit EinsumNodeGroupSelector(bool allow_int8 = true, bool allow_16bit = true, bool allow_4bit = true)
      : allow_int8_(allow_int8), allow_16bit_(allow_16bit), allow_4bit_(allow_4bit) {}

 private:
  bool Check(const GraphViewer& graph_viewer,
             const Node& node, const Node* redundant_clip_node,
             const std::vector<const Node*>& dq_nodes,
             const std::vector<const Node*>& q_nodes) const override;
  bool allow_int8_;
  bool allow_16bit_;
  bool allow_4bit_;
};

class ReciprocalNodeGroupSelector : public NodeGroupSelector {
 public:
  explicit ReciprocalNodeGroupSelector(bool allow_int8 = true, bool allow_16bit = true, bool allow_4bit = true)
      : allow_int8_(allow_int8), allow_16bit_(allow_16bit), allow_4bit_(allow_4bit) {}

 private:
  bool Check(const GraphViewer& graph_viewer,
             const Node& node, const Node* redundant_clip_node,
             const std::vector<const Node*>& dq_nodes,
             const std::vector<const Node*>& q_nodes) const override;
  bool allow_int8_;
  bool allow_16bit_;
  bool allow_4bit_;
};

// 2 DQ nodes for input -> node -> optional Q if QLinearMatMul, MatMulIntegerToFloat if not
// The lack of a trailing Q isn't really a QDQ node group, so we default support for that to off.
class MatMulNodeGroupSelector : public NodeGroupSelector {
 public:
  MatMulNodeGroupSelector(bool int8_allowed = true,
                          bool matmulintegertofloat_allowed = false,
                          bool allow_16bit = true,
                          bool allow_4bit = true)
      : int8_allowed_(int8_allowed),
        matmulintegertofloat_allowed_(matmulintegertofloat_allowed),
        allow_16bit_(allow_16bit),
        allow_4bit_(allow_4bit) {
  }

 private:
  bool Check(const GraphViewer& graph_viewer, const Node& node, const Node* redundant_clip_node,
             const std::vector<const Node*>& dq_nodes,
             const std::vector<const Node*>& q_nodes) const override;
  bool int8_allowed_;
  bool matmulintegertofloat_allowed_;
  bool allow_16bit_;
  bool allow_4bit_;
};

// Convert "1 DQ node for input B -> MatMul" to "MatMulNBits"
class DQMatMulNodeGroupSelector : public NodeGroupSelector {
 private:
  bool Check(const GraphViewer& graph_viewer, const Node& node, const Node* redundant_clip_node,
             const std::vector<const Node*>& dq_nodes,
             const std::vector<const Node*>& q_nodes) const override;
};

// Input: DQ nodes for A, B and optional C
// Output: optional Q node for Y
class GemmNodeGroupSelector : public NodeGroupSelector {
 public:
  explicit GemmNodeGroupSelector(bool allow_16bit = true, bool allow_4bit = true)
      : allow_16bit_(allow_16bit), allow_4bit_(allow_4bit) {}

 private:
  bool Check(const GraphViewer& graph_viewer, const Node& node, const Node* redundant_clip_node,
             const std::vector<const Node*>& dq_nodes,
             const std::vector<const Node*>& q_nodes) const override;

  bool allow_16bit_;
  bool allow_4bit_;
};

// Input: DQ nodes for input, scale, and B
// Output: Q node for output
class InstanceAndLayerNormalizationNodeGroupSelector : public NodeGroupSelector {
 private:
  bool Check(const GraphViewer& graph_viewer, const Node& node, const Node* redundant_clip_node,
             const std::vector<const Node*>& dq_nodes,
             const std::vector<const Node*>& q_nodes) const override;
};

// DQ nodes for X, W and optionally B, not used for mean, var -> node -> Q
class BatchNormalizationNodeGroupSelector : public NodeGroupSelector {
 public:
  // default to 'true'
  BatchNormalizationNodeGroupSelector(bool int8_allowed = true) : int8_allowed_(int8_allowed) {}

 private:
  bool Check(const GraphViewer& graph_viewer, const Node& node, const Node* redundant_clip_node,
             const std::vector<const Node*>& dq_nodes,
             const std::vector<const Node*>& q_nodes) const override;

  bool int8_allowed_;
};

// 2 DQ nodes providing input -> node with bool output tensor.
// Example: Equal, Less, Greater.
class LogicalComparisonNodeGroupSelector : public NodeGroupSelector {
  bool Check(const GraphViewer& graph_viewer, const Node& node, const Node* redundant_clip_node,
             const std::vector<const Node*>& dq_nodes,
             const std::vector<const Node*>& q_nodes) const override;
};

// TopK has 1 DQ input node and 1 Q output node.
// Zero point and scale are constant scalars and must match
class TopKNodeGroupSelector : public NodeGroupSelector {
  bool Check(const GraphViewer& graph_viewer, const Node& node, const Node* redundant_clip_node,
             const std::vector<const Node*>& dq_nodes,
             const std::vector<const Node*>& q_nodes) const override;
};

// one DQ node for first input -> node -> Q
class CumSumNodeGroupSelector : public NodeGroupSelector {
  bool Check(const GraphViewer& graph_viewer,
             const Node& node, const Node* redundant_clip_node,
             const std::vector<const Node*>& dq_nodes,
             const std::vector<const Node*>& q_nodes) const override;
};

// Input: DQ nodes for Data, and Update
// Output: Q node for output
class ScatterElementsNodeGroupSelector : public NodeGroupSelector {
  bool Check(const GraphViewer& graph_viewer,
             const Node& node, const Node* redundant_clip_node,
             const std::vector<const Node*>& dq_nodes,
             const std::vector<const Node*>& q_nodes) const override;
};

/*
 * NodeSelector instances for use in the QDQ::SelectorActionTransformer.
 */
// Base QDQ checker. Finds and provides the DQ and Q nodes to the operator specific checkers, as the QDQ optimizations
// always involve those nodes.
class BaseSelector : public NodeSelector {
 public:
  std::optional<NodesToOptimizeIndices> Select(const GraphViewer& graph_viewer, const Node& node) const override;

  // We std::move SelectorActionRegistry into the SelectorActionTransformer so this class needs to have a move ctor
  BaseSelector(BaseSelector&& rhs) noexcept
      : node_group_selector_{std::move(rhs.node_group_selector_)},
        compatible_providers_{std::move(rhs.compatible_providers_)} {
  }

 protected:
  BaseSelector(std::unique_ptr<NodeGroupSelector> node_group_selector, gsl::span<const char*> compatible_providers = {})
      : node_group_selector_{std::move(node_group_selector)},
        compatible_providers_(compatible_providers.begin(), compatible_providers.end()) {
  }

  // override if you need to adjust the values in NodesToOptimize.
  // e.g. add entries for missing optional DQ inputs or set num_inputs to handle variadic inputs
  // Called post-Check, if Check returned `true`
  virtual void UpdateBuilder(NodesToOptimizeIndicesBuilder&) const {}

 private:
  std::unique_ptr<NodeGroupSelector> node_group_selector_;
  std::vector<std::string> compatible_providers_;
};

class DropQDQNodesSelector : public BaseSelector {
 public:
  explicit DropQDQNodesSelector(bool allow_16bit = false, bool allow_4bit = false,
                                bool allow_nonpositive_scale = true,
                                gsl::span<const char*> compatible_providers = {})
      : BaseSelector(std::make_unique<DropQDQNodeGroupSelector>(allow_16bit, allow_4bit, allow_nonpositive_scale),
                     compatible_providers) {}
};

class DropDQNodesSelector : public BaseSelector {
 public:
  explicit DropDQNodesSelector(bool allow_16bit = false,
                               bool allow_4bit = false,
                               gsl::span<const char*> compatible_providers = {})
      : BaseSelector(std::make_unique<DropDQNodeGroupSelector>(allow_16bit, allow_4bit),
                     compatible_providers) {}
};

class UnarySelector : public BaseSelector {
 public:
  explicit UnarySelector(gsl::span<const char*> compatible_providers = {}, bool allow_16bit = false,
                         bool allow_4bit = false)
      : BaseSelector(std::make_unique<UnaryNodeGroupSelector>(allow_16bit, allow_4bit), compatible_providers) {}
};

class BinarySelector : public BaseSelector {
 public:
  explicit BinarySelector(gsl::span<const char*> compatible_providers = {}, bool allow_16bit = false,
                          bool allow_4bit = false)
      : BaseSelector(std::make_unique<BinaryNodeGroupSelector>(allow_16bit, allow_4bit), compatible_providers) {}
};

// Variadic DQ nodes -> node -> Q
class InputVariadicSelector : public BaseSelector {
 public:
  explicit InputVariadicSelector(bool allow_16bit = false,
                                 bool allow_4bit = false,
                                 gsl::span<const char*> compatible_providers = {})
      : BaseSelector(std::make_unique<VariadicNodeGroupSelector>(allow_16bit, allow_4bit),
                     compatible_providers) {}

  void UpdateBuilder(NodesToOptimizeIndicesBuilder&) const override;
};

//  DQ -> Split -> variadic Q nodes
class SplitSelector : public BaseSelector {
 public:
  SplitSelector(bool req_equal_quant_params = false, bool allow_4bit = false,
                gsl::span<const char*> compatible_providers = {})
      : BaseSelector(std::make_unique<SplitNodeGroupSelector>(req_equal_quant_params, allow_4bit),
                     compatible_providers) {}

  void UpdateBuilder(NodesToOptimizeIndicesBuilder&) const override;
};

// DQ nodes for X, W and optionally B -> node -> Q
class ConvSelector : public BaseSelector {
 public:
  ConvSelector(bool int8_allowed = false, bool allow_16bit = false, bool allow_4bit_weight = false,
               gsl::span<const char*> compatible_providers = {})
      : BaseSelector(std::make_unique<ConvNodeGroupSelector>(int8_allowed, allow_16bit, allow_4bit_weight),
                     compatible_providers) {}

  void UpdateBuilder(NodesToOptimizeIndicesBuilder&) const override;
};

class WhereSelector : public BaseSelector {
 public:
  explicit WhereSelector(gsl::span<const char*> compatible_providers = {}, bool allow_16bit = false,
                         bool allow_4bit = false)
      : BaseSelector(std::make_unique<WhereNodeGroupSelector>(allow_16bit, allow_4bit), compatible_providers) {}
};

// 2 DQ nodes for input -> node -> optional Q if QLinearMatMul, MatMulIntegerToFloat if not
class MatMulSelector : public BaseSelector {
 public:
  MatMulSelector(bool int8_allowed, bool allow_16bit = false, bool allow_4bit = false,
                 gsl::span<const char*> compatible_providers = {})
      : BaseSelector(std::make_unique<MatMulNodeGroupSelector>(int8_allowed, /*matmulintegertofloat_allowed*/ true,
                                                               allow_16bit, allow_4bit),
                     compatible_providers) {}
};

// Convert "1 DQ node for input B -> MatMul" to "MatMulNBits"
class DQMatMulToMatMulNBitsSelector : public BaseSelector {
 public:
  explicit DQMatMulToMatMulNBitsSelector(gsl::span<const char*> compatible_providers = {})
      : BaseSelector(std::make_unique<DQMatMulNodeGroupSelector>(), compatible_providers) {}
};

// Input: DQ nodes for A, B and optional C
// Output: optional Q node for Y
class GemmSelector : public BaseSelector {
 public:
  explicit GemmSelector(gsl::span<const char*> compatible_providers = {}, bool allow_16bit = false,
                        bool allow_4bit = false)
      : BaseSelector(std::make_unique<GemmNodeGroupSelector>(allow_16bit, allow_4bit), compatible_providers) {}

  void UpdateBuilder(NodesToOptimizeIndicesBuilder&) const override;
};

}  // namespace QDQ
}  // namespace onnxruntime

#endif  // !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD)
