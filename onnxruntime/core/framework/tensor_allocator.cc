// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "tensor_allocator_with_mem_pattern.h"
#include "simple_tensor_allocator.h"

namespace onnxruntime {
AllocatorPtr ITensorAllocator::GetAllocator(const OrtDevice& device) {
  return session_state_.GetAllocator(device);
}

AllocatorPtr ITensorAllocator::GetInitializerAllocator(const OrtDevice& device) {
  return session_state_.GetInitializerAllocator(device);
}

std::unique_ptr<ITensorAllocator> ITensorAllocator::Create(bool enable_mem_pattern,
                                                           const ExecutionPlanBase& execution_plan,
                                                           const SessionState& session_state,
                                                           InlinedVector<BufferUniquePtr>& weights_buffers) {
  if (enable_mem_pattern) {
    return std::make_unique<TensorAllocatorWithMemPattern>(execution_plan, session_state, weights_buffers);
  } else {
    return std::make_unique<SimpleTensorAllocator>(execution_plan, session_state, weights_buffers);
  }
}
}  // namespace onnxruntime
