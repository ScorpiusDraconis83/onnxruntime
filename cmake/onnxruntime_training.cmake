# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.

set (CXXOPTS ${cxxopts_SOURCE_DIR}/include)

# training lib
file(GLOB_RECURSE onnxruntime_training_srcs
    "${ORTTRAINING_SOURCE_DIR}/core/framework/*.h"
    "${ORTTRAINING_SOURCE_DIR}/core/framework/*.cc"
    "${ORTTRAINING_SOURCE_DIR}/core/framework/tensorboard/*.h"
    "${ORTTRAINING_SOURCE_DIR}/core/framework/tensorboard/*.cc"
    "${ORTTRAINING_SOURCE_DIR}/core/framework/adasum/*"
    "${ORTTRAINING_SOURCE_DIR}/core/framework/communication/*"
    "${ORTTRAINING_SOURCE_DIR}/core/session/*.h"
    "${ORTTRAINING_SOURCE_DIR}/core/session/*.cc"
    "${ORTTRAINING_SOURCE_DIR}/core/agent/*.h"
    "${ORTTRAINING_SOURCE_DIR}/core/agent/*.cc"
    )


# This needs to be built in framework.cmake
file(GLOB_RECURSE onnxruntime_training_framework_excluded_srcs CONFIGURE_DEPENDS
    "${ORTTRAINING_SOURCE_DIR}/core/framework/torch/*.h"
    "${ORTTRAINING_SOURCE_DIR}/core/framework/torch/*.cc"
    "${ORTTRAINING_SOURCE_DIR}/core/framework/triton/*.h"
    "${ORTTRAINING_SOURCE_DIR}/core/framework/triton/*.cc"
)

list(REMOVE_ITEM onnxruntime_training_srcs ${onnxruntime_training_framework_excluded_srcs})

onnxruntime_add_static_library(onnxruntime_training ${onnxruntime_training_srcs})
add_dependencies(onnxruntime_training onnx tensorboard ${onnxruntime_EXTERNAL_DEPENDENCIES})
onnxruntime_add_include_to_target(onnxruntime_training onnxruntime_common onnx onnx_proto tensorboard ${PROTOBUF_LIB} flatbuffers::flatbuffers re2::re2 Boost::mp11 safeint_interface Eigen3::Eigen)

# fix event_writer.cc 4100 warning
if(WIN32)
  target_compile_options(onnxruntime_training PRIVATE /wd4100)
endif()

target_include_directories(onnxruntime_training PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${ONNXRUNTIME_ROOT} ${ORTTRAINING_ROOT} PUBLIC ${onnxruntime_graph_header} ${MPI_CXX_INCLUDE_DIRS})

if (onnxruntime_USE_NCCL)
  target_include_directories(onnxruntime_training PRIVATE ${NCCL_INCLUDE_DIRS})
endif()

if (onnxruntime_BUILD_UNIT_TESTS)
  set_target_properties(onnxruntime_training PROPERTIES FOLDER "ONNXRuntime")
  source_group(TREE ${ORTTRAINING_ROOT} FILES ${onnxruntime_training_srcs})

  # training runner lib
  file(GLOB_RECURSE onnxruntime_training_runner_srcs
      "${ORTTRAINING_SOURCE_DIR}/models/runner/*.h"
      "${ORTTRAINING_SOURCE_DIR}/models/runner/*.cc"
  )

  # perf test utils
  set(onnxruntime_perf_test_src_dir ${TEST_SRC_DIR}/perftest)
  set(onnxruntime_perf_test_src
  "${onnxruntime_perf_test_src_dir}/utils.h")

  if(WIN32)
    list(APPEND onnxruntime_perf_test_src
      "${onnxruntime_perf_test_src_dir}/windows/utils.cc")
  else ()
    list(APPEND onnxruntime_perf_test_src
      "${onnxruntime_perf_test_src_dir}/posix/utils.cc")
  endif()

  onnxruntime_add_static_library(onnxruntime_training_runner ${onnxruntime_training_runner_srcs} ${onnxruntime_perf_test_src})
  add_dependencies(onnxruntime_training_runner ${onnxruntime_EXTERNAL_DEPENDENCIES} onnx onnxruntime_providers)

  if (onnxruntime_ENABLE_TRAINING_TORCH_INTEROP)
    target_link_libraries(onnxruntime_training_runner PRIVATE Python::Python)
  endif()

  onnxruntime_add_include_to_target(onnxruntime_training_runner onnxruntime_training onnxruntime_framework onnxruntime_common onnx onnx_proto ${PROTOBUF_LIB} onnxruntime_training flatbuffers::flatbuffers Boost::mp11 safeint_interface Eigen3::Eigen)

  target_include_directories(onnxruntime_training_runner PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${ONNXRUNTIME_ROOT} ${ORTTRAINING_ROOT} PUBLIC ${onnxruntime_graph_header})
  target_link_libraries(onnxruntime_training_runner PRIVATE nlohmann_json::nlohmann_json)

  if (onnxruntime_USE_NCCL)
    target_include_directories(onnxruntime_training_runner PRIVATE ${NCCL_INCLUDE_DIRS})
  endif()



  check_cxx_compiler_flag(-Wno-maybe-uninitialized HAS_NO_MAYBE_UNINITIALIZED)
  if(UNIX AND NOT APPLE)
    if (HAS_NO_MAYBE_UNINITIALIZED)
      target_compile_options(onnxruntime_training_runner PUBLIC "-Wno-maybe-uninitialized")
    endif()
  endif()

  set_target_properties(onnxruntime_training_runner PROPERTIES FOLDER "ONNXRuntimeTest")
  source_group(TREE ${REPO_ROOT} FILES ${onnxruntime_training_runner_srcs} ${onnxruntime_perf_test_src})

  # MNIST
  file(GLOB_RECURSE training_mnist_src
      "${ORTTRAINING_SOURCE_DIR}/models/mnist/*.h"
      "${ORTTRAINING_SOURCE_DIR}/models/mnist/mnist_data_provider.cc"
      "${ORTTRAINING_SOURCE_DIR}/models/mnist/main.cc"
  )
  onnxruntime_add_executable(onnxruntime_training_mnist ${training_mnist_src})
  onnxruntime_add_include_to_target(onnxruntime_training_mnist onnxruntime_common onnx onnx_proto ${PROTOBUF_LIB} onnxruntime_training flatbuffers::flatbuffers Boost::mp11 safeint_interface)
  target_include_directories(onnxruntime_training_mnist PUBLIC ${CMAKE_CURRENT_BINARY_DIR} ${ONNXRUNTIME_ROOT} ${ORTTRAINING_ROOT} ${CXXOPTS} ${extra_includes} ${onnxruntime_graph_header} ${onnxruntime_exec_src_dir} ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_BINARY_DIR}/onnx onnxruntime_training_runner)

  set(ONNXRUNTIME_LIBS
      onnxruntime_session
      ${onnxruntime_libs}
      ${PROVIDERS_MKLDNN}
      ${PROVIDERS_DML}
      onnxruntime_optimizer
      onnxruntime_providers
      onnxruntime_util
      onnxruntime_framework
  )

  if (onnxruntime_ENABLE_TRAINING_TORCH_INTEROP)
    list(APPEND ONNXRUNTIME_LIBS Python::Python)
  endif()

  list(APPEND ONNXRUNTIME_LIBS
      onnxruntime_graph
      ${ONNXRUNTIME_MLAS_LIBS}
      onnxruntime_common
      onnxruntime_flatbuffers
      Boost::mp11 safeint_interface Eigen3::Eigen
  )

  if(UNIX AND NOT APPLE)
    if (HAS_NO_MAYBE_UNINITIALIZED)
      target_compile_options(onnxruntime_training_mnist PUBLIC "-Wno-maybe-uninitialized")
    endif()
  endif()
  target_link_libraries(onnxruntime_training_mnist PRIVATE onnxruntime_training_runner onnxruntime_lora onnxruntime_training ${ONNXRUNTIME_LIBS} ${onnxruntime_EXTERNAL_LIBRARIES})
  set_target_properties(onnxruntime_training_mnist PROPERTIES FOLDER "ONNXRuntimeTest")

  # squeezenet
  # Disabling build for squeezenet, as no one is using this
  #[[
  file(GLOB_RECURSE training_squeezene_src
      "${ORTTRAINING_SOURCE_DIR}/models/squeezenet/*.h"
      "${ORTTRAINING_SOURCE_DIR}/models/squeezenet/*.cc"
  )
  onnxruntime_add_executable(onnxruntime_training_squeezenet ${training_squeezene_src})
  onnxruntime_add_include_to_target(onnxruntime_training_squeezenet onnxruntime_common onnx onnx_proto ${PROTOBUF_LIB} onnxruntime_training flatbuffers::flatbuffers Boost::mp11 safeint_interface)
  target_include_directories(onnxruntime_training_squeezenet PUBLIC ${ONNXRUNTIME_ROOT} ${ORTTRAINING_ROOT} ${extra_includes} ${onnxruntime_graph_header} ${onnxruntime_exec_src_dir} ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_BINARY_DIR}/onnx onnxruntime_training_runner)

  if(UNIX AND NOT APPLE)
    target_compile_options(onnxruntime_training_squeezenet PUBLIC "-Wno-maybe-uninitialized")
  endif()
  target_link_libraries(onnxruntime_training_squeezenet PRIVATE onnxruntime_training_runner onnxruntime_training ${ONNXRUNTIME_LIBS} ${onnxruntime_EXTERNAL_LIBRARIES})
  set_target_properties(onnxruntime_training_squeezenet PROPERTIES FOLDER "ONNXRuntimeTest")
  ]]

  # BERT
  file(GLOB_RECURSE training_bert_src
      "${ORTTRAINING_SOURCE_DIR}/models/bert/*.h"
      "${ORTTRAINING_SOURCE_DIR}/models/bert/*.cc"
  )
  onnxruntime_add_executable(onnxruntime_training_bert ${training_bert_src})

  if(UNIX AND NOT APPLE)
    if (HAS_NO_MAYBE_UNINITIALIZED)
      target_compile_options(onnxruntime_training_bert PUBLIC "-Wno-maybe-uninitialized")
    endif()
  endif()

  onnxruntime_add_include_to_target(onnxruntime_training_bert onnxruntime_common onnx onnx_proto ${PROTOBUF_LIB} onnxruntime_training flatbuffers::flatbuffers Boost::mp11 safeint_interface)
  target_include_directories(onnxruntime_training_bert PUBLIC ${CMAKE_CURRENT_BINARY_DIR} ${ONNXRUNTIME_ROOT} ${ORTTRAINING_ROOT} ${MPI_CXX_INCLUDE_DIRS} ${CXXOPTS} ${extra_includes} ${onnxruntime_graph_header} ${onnxruntime_exec_src_dir} ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_BINARY_DIR}/onnx onnxruntime_training_runner)

  target_link_libraries(onnxruntime_training_bert PRIVATE onnxruntime_training_runner onnxruntime_training ${ONNXRUNTIME_LIBS} ${onnxruntime_EXTERNAL_LIBRARIES})
  set_target_properties(onnxruntime_training_bert PROPERTIES FOLDER "ONNXRuntimeTest")

  # Pipeline
  file(GLOB_RECURSE training_pipeline_poc_src
      "${ORTTRAINING_SOURCE_DIR}/models/pipeline_poc/*.h"
      "${ORTTRAINING_SOURCE_DIR}/models/pipeline_poc/*.cc"
  )
  onnxruntime_add_executable(onnxruntime_training_pipeline_poc ${training_pipeline_poc_src})

  if(UNIX AND NOT APPLE)
    if (HAS_NO_MAYBE_UNINITIALIZED)
      target_compile_options(onnxruntime_training_pipeline_poc PUBLIC "-Wno-maybe-uninitialized")
    endif()
  endif()

  onnxruntime_add_include_to_target(onnxruntime_training_pipeline_poc onnxruntime_common onnx onnx_proto ${PROTOBUF_LIB} onnxruntime_training flatbuffers::flatbuffers Boost::mp11 safeint_interface)
  target_include_directories(onnxruntime_training_pipeline_poc PUBLIC ${CMAKE_CURRENT_BINARY_DIR} ${ONNXRUNTIME_ROOT} ${ORTTRAINING_ROOT} ${MPI_CXX_INCLUDE_DIRS} ${CXXOPTS} ${extra_includes} ${onnxruntime_graph_header} ${onnxruntime_exec_src_dir} ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_BINARY_DIR}/onnx onnxruntime_training_runner)
  if (onnxruntime_USE_NCCL)
    target_include_directories(onnxruntime_training_pipeline_poc PRIVATE ${NCCL_INCLUDE_DIRS})
  endif()

  target_link_libraries(onnxruntime_training_pipeline_poc PRIVATE onnxruntime_training_runner onnxruntime_training ${ONNXRUNTIME_LIBS} ${onnxruntime_EXTERNAL_LIBRARIES})
  set_target_properties(onnxruntime_training_pipeline_poc PROPERTIES FOLDER "ONNXRuntimeTest")

  # GPT-2
  file(GLOB_RECURSE training_gpt2_src
      "${ORTTRAINING_SOURCE_DIR}/models/gpt2/*.h"
      "${ORTTRAINING_SOURCE_DIR}/models/gpt2/*.cc"
  )
  onnxruntime_add_executable(onnxruntime_training_gpt2 ${training_gpt2_src})
  if(UNIX AND NOT APPLE)
    if (HAS_NO_MAYBE_UNINITIALIZED)
      target_compile_options(onnxruntime_training_gpt2 PUBLIC "-Wno-maybe-uninitialized")
    endif()
  endif()

  target_include_directories(onnxruntime_training_gpt2 PUBLIC ${CMAKE_CURRENT_BINARY_DIR} ${ONNXRUNTIME_ROOT} ${ORTTRAINING_ROOT} ${MPI_CXX_INCLUDE_DIRS} ${CXXOPTS} ${extra_includes} ${onnxruntime_graph_header} ${onnxruntime_exec_src_dir} ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_BINARY_DIR}/onnx onnxruntime_training_runner)

  target_link_libraries(onnxruntime_training_gpt2 PRIVATE onnxruntime_training_runner onnxruntime_training ${ONNXRUNTIME_LIBS} ${onnxruntime_EXTERNAL_LIBRARIES})
  set_target_properties(onnxruntime_training_gpt2 PROPERTIES FOLDER "ONNXRuntimeTest")

endif()
