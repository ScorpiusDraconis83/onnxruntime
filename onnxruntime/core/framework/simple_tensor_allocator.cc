// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "simple_tensor_allocator.h"
#include "tensorprotoutils.h"

namespace onnxruntime {
common::Status SimpleTensorAllocator::Trace(int /*id*/, const ONNX_NAMESPACE::TensorProto* /*value*/) {
  return Status::OK();
}

common::Status SimpleTensorAllocator::GetPreallocatedBuffer(int ort_value_index, const std::string& /*name*/,
                                                            std::optional<MemBuffer>& /*buf_out*/,
                                                            AllocatorPtr& alloc_out) {
  const struct OrtDevice& location = seq_plan_.GetLocation(ort_value_index);
  // just return allocator and let others handle it.
  alloc_out = GetInitializerAllocator(location);
  return Status::OK();
}
}  // namespace onnxruntime
