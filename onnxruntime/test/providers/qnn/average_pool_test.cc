// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#if !defined(ORT_MINIMAL_BUILD)

#include <string>
#include <unordered_map>
#include <vector>

#include "core/graph/node_attr_utils.h"
#include "test/optimizer/qdq_test_utils.h"
#include "test/providers/qnn/qnn_test_utils.h"

#include "core/graph/onnx_protobuf.h"

#include "gtest/gtest.h"

namespace onnxruntime {
namespace test {

// Runs an AveragePool model on the QNN CPU backend. Checks the graph node assignment, and that inference
// outputs for QNN and CPU match.
static void RunAveragePoolOpTest(const std::string& op_type,
                                 const std::vector<TestInputDef<float>>& input_defs,
                                 const std::vector<ONNX_NAMESPACE::AttributeProto>& attrs,
                                 ExpectedEPNodeAssignment expected_ep_assignment,
                                 const std::string& backend_name = "cpu", int opset = 18) {
  ProviderOptions provider_options;
  provider_options["backend_type"] = backend_name;
  provider_options["offload_graph_io_quantization"] = "0";

  RunQnnModelTest(BuildOpTestCase<float>(op_type, input_defs, {}, attrs),
                  provider_options,
                  opset,
                  expected_ep_assignment);
}

// Runs a QDQ AveragePool model on the QNN HTP backend. Checks the graph node assignment, and that accuracy
// on QNN EP is at least as good as on CPU EP.
template <typename QuantType>
static void RunQDQAveragePoolOpTest(const std::string& op_type,
                                    const std::vector<TestInputDef<float>>& input_defs,
                                    const std::vector<ONNX_NAMESPACE::AttributeProto>& attrs,
                                    ExpectedEPNodeAssignment expected_ep_assignment,
                                    int opset = 18,
                                    QDQTolerance tolerance = QDQTolerance()) {
  ProviderOptions provider_options;
  provider_options["backend_type"] = "htp";
  provider_options["offload_graph_io_quantization"] = "0";

  TestQDQModelAccuracy(BuildOpTestCase<float>(op_type, input_defs, {}, attrs),
                       BuildQDQOpTestCase<QuantType>(op_type, input_defs, {}, attrs),
                       provider_options,
                       opset,
                       expected_ep_assignment,
                       tolerance);
}

//
// CPU tests:
//

// AveragePool with kernel size equal to the spatial dimension of input tensor.
TEST_F(QnnCPUBackendTests, AveragePool_AsGlobal) {
  RunAveragePoolOpTest("AveragePool",
                       {TestInputDef<float>({1, 2, 3, 3}, false, GetFloatDataInRange(-10.0f, 10.0f, 18))},
                       {utils::MakeAttribute("kernel_shape", std::vector<int64_t>{3, 3}),
                        utils::MakeAttribute("strides", std::vector<int64_t>{3, 3})},
                       ExpectedEPNodeAssignment::All);
}

// Test GlobalAveragePool on QNN CPU backend.
TEST_F(QnnCPUBackendTests, GlobalAveragePool) {
  RunAveragePoolOpTest("GlobalAveragePool",
                       {TestInputDef<float>({1, 2, 3, 3}, false, GetFloatDataInRange(-10.0f, 10.0f, 18))},
                       {},
                       ExpectedEPNodeAssignment::All);
}

// AveragePool that counts padding.
TEST_F(QnnCPUBackendTests, AveragePool_CountIncludePad) {
  RunAveragePoolOpTest("AveragePool",
                       {TestInputDef<float>({1, 2, 3, 3}, false, GetFloatDataInRange(-10.0f, 10.0f, 18))},
                       {utils::MakeAttribute("kernel_shape", std::vector<int64_t>{1, 1}),
                        utils::MakeAttribute("count_include_pad", static_cast<int64_t>(1))},
                       ExpectedEPNodeAssignment::All);
}

// AveragePool that use auto_pad 'SAME_UPPER'.
TEST_F(QnnCPUBackendTests, AveragePool_AutopadSameUpper) {
  RunAveragePoolOpTest("AveragePool",
                       {TestInputDef<float>({1, 2, 3, 3}, false, GetFloatDataInRange(-10.0f, 10.0f, 18))},
                       {utils::MakeAttribute("kernel_shape", std::vector<int64_t>{1, 1}),
                        utils::MakeAttribute("count_include_pad", static_cast<int64_t>(1)),
                        utils::MakeAttribute("auto_pad", "SAME_UPPER")},
                       ExpectedEPNodeAssignment::All);
}

// AveragePool that use auto_pad 'SAME_LOWER'.
TEST_F(QnnCPUBackendTests, AveragePool_AutopadSameLower) {
  RunAveragePoolOpTest("AveragePool",
                       {TestInputDef<float>({1, 2, 3, 3}, false, GetFloatDataInRange(-10.0f, 10.0f, 18))},
                       {utils::MakeAttribute("kernel_shape", std::vector<int64_t>{1, 1}),
                        utils::MakeAttribute("count_include_pad", static_cast<int64_t>(1)),
                        utils::MakeAttribute("auto_pad", "SAME_LOWER")},
                       ExpectedEPNodeAssignment::All);
}

// AveragePool 3D as GlobalAveragePool.
TEST_F(QnnCPUBackendTests, AveragePool_3D_AsGlobal) {
  RunAveragePoolOpTest("AveragePool",
                       {TestInputDef<float>({1, 2, 3, 3, 3}, false, -10.0f, 10.0f)},
                       {utils::MakeAttribute("kernel_shape", std::vector<int64_t>{3, 3, 3}),
                        utils::MakeAttribute("strides", std::vector<int64_t>{3, 3, 3})},
                       ExpectedEPNodeAssignment::All);
}

// GlobalAveragePool 3D.
TEST_F(QnnCPUBackendTests, GlobalAveragePool_3D) {
  RunAveragePoolOpTest("GlobalAveragePool",
                       {TestInputDef<float>({1, 2, 3, 3, 3}, false, -10.0f, 10.0f)},
                       {},
                       ExpectedEPNodeAssignment::All);
}

#if defined(__aarch64__) || defined(_M_ARM64) || defined(__linux__)
//
// HTP tests:
//

// QDQ AveragePool with kernel size equal to the spatial dimension of input tensor.
TEST_F(QnnHTPBackendTests, AveragePool_AsGlobal) {
  std::vector<float> input = {32.1289f, -59.981f, -17.2799f, 62.7263f, 33.6205f, -19.3515f, -54.0113f, 37.5648f, 61.5357f,
                              -52.5769f, 27.3637f, -9.01382f, -65.5612f, 19.9497f, -47.9228f, 26.9813f, 83.064f, 0.362503f};
  RunQDQAveragePoolOpTest<uint8_t>("AveragePool",
                                   {TestInputDef<float>({1, 2, 3, 3}, false, input)},
                                   {utils::MakeAttribute("kernel_shape", std::vector<int64_t>{3, 3}),
                                    utils::MakeAttribute("strides", std::vector<int64_t>{3, 3})},
                                   ExpectedEPNodeAssignment::All);
}

// Test accuracy for 8-bit QDQ GlobalAveragePool with input of rank 4.
TEST_F(QnnHTPBackendTests, GlobalAveragePool) {
  std::vector<float> input = GetFloatDataInRange(-32.0f, 32.0f, 18);

  RunQDQAveragePoolOpTest<uint8_t>("GlobalAveragePool",
                                   {TestInputDef<float>({1, 2, 3, 3}, false, input)},
                                   {},
                                   ExpectedEPNodeAssignment::All);
}

// QDQ AveragePool that counts padding.
TEST_F(QnnHTPBackendTests, AveragePool_CountIncludePad_HTP_u8) {
  std::vector<float> input = {-9.0f, -7.33f, -6.0f, -5.0f, -4.0f, -3.0f, -2.0f, -1.0f, 0.0f,
                              1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f};

  RunQDQAveragePoolOpTest<uint8_t>("AveragePool",
                                   {TestInputDef<float>({1, 2, 3, 3}, false, input)},
                                   {utils::MakeAttribute("kernel_shape", std::vector<int64_t>{1, 1}),
                                    utils::MakeAttribute("count_include_pad", static_cast<int64_t>(1))},
                                   ExpectedEPNodeAssignment::All,
                                   18);
}

// QDQ AveragePool that use auto_pad 'SAME_UPPER'.
TEST_F(QnnHTPBackendTests, AveragePool_AutopadSameUpper_HTP_u8) {
  std::vector<float> input = {-9.0f, -7.33f, -6.0f, -5.0f, -4.0f, -3.0f, -2.0f, -1.0f, 0.0f,
                              1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f};

  RunQDQAveragePoolOpTest<uint8_t>("AveragePool",
                                   {TestInputDef<float>({1, 2, 3, 3}, false, input)},
                                   {utils::MakeAttribute("kernel_shape", std::vector<int64_t>{1, 1}),
                                    utils::MakeAttribute("auto_pad", "SAME_UPPER")},
                                   ExpectedEPNodeAssignment::All,
                                   18);
}

// QDQ AveragePool that use auto_pad 'SAME_LOWER'.
TEST_F(QnnHTPBackendTests, AveragePool_AutopadSameLower_HTP_u8) {
  std::vector<float> input = {-9.0f, -7.33f, -6.0f, -5.0f, -4.0f, -3.0f, -2.0f, -1.0f, 0.0f,
                              1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f};

  RunQDQAveragePoolOpTest<uint8_t>("AveragePool",
                                   {TestInputDef<float>({1, 2, 3, 3}, false, input)},
                                   {utils::MakeAttribute("kernel_shape", std::vector<int64_t>{1, 1}),
                                    utils::MakeAttribute("auto_pad", "SAME_LOWER")},
                                   ExpectedEPNodeAssignment::All,
                                   18);
}

// QDQ AveragePool 3D.
TEST_F(QnnHTPBackendTests, AveragePool_3D_u8) {
  RunQDQAveragePoolOpTest<uint8_t>("AveragePool",
                                   {TestInputDef<float>({1, 2, 8, 8, 8}, false, -10.0f, 10.0f)},
                                   {utils::MakeAttribute("kernel_shape", std::vector<int64_t>{3, 3, 3}),
                                    utils::MakeAttribute("strides", std::vector<int64_t>{2, 2, 2})},
                                   ExpectedEPNodeAssignment::All);
}

// QDQ AveragePool 3D with auto_pad SAME_UPPER.
TEST_F(QnnHTPBackendTests, AveragePool_3D_AutoPad_SAME_UPPER_u8) {
  RunQDQAveragePoolOpTest<uint8_t>("AveragePool",
                                   {TestInputDef<float>({1, 2, 8, 8, 8}, false, -10.0f, 10.0f)},
                                   {utils::MakeAttribute("kernel_shape", std::vector<int64_t>{2, 2, 2}),
                                    utils::MakeAttribute("auto_pad", "SAME_UPPER")},
                                   ExpectedEPNodeAssignment::All);
}

// QDQ AveragePool 3D with auto_pad SAME_LOWER.
TEST_F(QnnHTPBackendTests, AveragePool_3D_AutoPad_SAME_LOWER_u8) {
  RunQDQAveragePoolOpTest<uint8_t>("AveragePool",
                                   {TestInputDef<float>({1, 2, 8, 8, 8}, false, -10.0f, 10.0f)},
                                   {utils::MakeAttribute("kernel_shape", std::vector<int64_t>{2, 2, 2}),
                                    utils::MakeAttribute("auto_pad", "SAME_LOWER")},
                                   ExpectedEPNodeAssignment::All);
}

#endif  // defined(__aarch64__) || defined(_M_ARM64) || defined(__linux__)

#if defined(_M_ARM64)
//
// GPU tests:
//

// AveragePool with kernel size equal to the spatial dimension of input tensor.
TEST_F(QnnGPUBackendTests, AveragePool_AsGlobal) {
  RunAveragePoolOpTest("AveragePool",
                       {TestInputDef<float>({1, 2, 3, 3}, false, GetFloatDataInRange(-10.0f, 10.0f, 18))},
                       {utils::MakeAttribute("kernel_shape", std::vector<int64_t>{3, 3}),
                        utils::MakeAttribute("strides", std::vector<int64_t>{3, 3})},
                       ExpectedEPNodeAssignment::All, "gpu");
}

// Test GlobalAveragePool on QNN GPU backend.
TEST_F(QnnGPUBackendTests, GlobalAveragePool) {
  RunAveragePoolOpTest("GlobalAveragePool",
                       {TestInputDef<float>({1, 2, 3, 3}, false, GetFloatDataInRange(-10.0f, 10.0f, 18))},
                       {},
                       ExpectedEPNodeAssignment::All, "gpu");
}

// AveragePool that counts padding.
TEST_F(QnnGPUBackendTests, AveragePool_CountIncludePad) {
  RunAveragePoolOpTest("AveragePool",
                       {TestInputDef<float>({1, 3, 4, 5}, false, GetFloatDataInRange(-10.0f, 10.0f, 3 * 4 * 5))},
                       {utils::MakeAttribute("kernel_shape", std::vector<int64_t>{3, 3}),
                        utils::MakeAttribute("count_include_pad", static_cast<int64_t>(1))},
                       ExpectedEPNodeAssignment::All, "gpu");
}

// AveragePool that use auto_pad 'SAME_UPPER'.
TEST_F(QnnGPUBackendTests, AveragePool_AutopadSameUpper) {
  RunAveragePoolOpTest("AveragePool",
                       {TestInputDef<float>({1, 3, 4, 5}, false, GetFloatDataInRange(-10.0f, 10.0f, 3 * 4 * 5))},
                       {utils::MakeAttribute("kernel_shape", std::vector<int64_t>{3, 3}),
                        utils::MakeAttribute("count_include_pad", static_cast<int64_t>(1)),
                        utils::MakeAttribute("auto_pad", "SAME_UPPER")},
                       ExpectedEPNodeAssignment::All, "gpu");
}

// AveragePool that use auto_pad 'SAME_LOWER'.
TEST_F(QnnGPUBackendTests, AveragePool_AutopadSameLower) {
  RunAveragePoolOpTest("AveragePool",
                       {TestInputDef<float>({1, 3, 4, 5}, false, GetFloatDataInRange(-10.0f, 10.0f, 3 * 4 * 5))},
                       {utils::MakeAttribute("kernel_shape", std::vector<int64_t>{3, 3}),
                        utils::MakeAttribute("count_include_pad", static_cast<int64_t>(1)),
                        utils::MakeAttribute("auto_pad", "SAME_LOWER")},
                       ExpectedEPNodeAssignment::All, "gpu");
}

#endif  // defined(_M_ARM64) GPU tests

}  // namespace test
}  // namespace onnxruntime

#endif  // !defined(ORT_MINIMAL_BUILD)
