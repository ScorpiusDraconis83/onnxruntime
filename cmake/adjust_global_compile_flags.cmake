# The flags set by this file are applied to 3rd-party libraries(e.g. protobuf) as well. If you want to set something that is only for ORT's code, please put it in the main CMakeLists.txt file.

if (ANDROID)
  # Build shared libraries with support for 16 KB ELF alignment
  # https://source.android.com/docs/core/architecture/16kb-page-size/16kb#build-lib-16kb-alignment
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-z,max-page-size=16384")
  # Also apply to MODULE libraries (like libonnxruntime4j_jni.so)
  set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,-z,max-page-size=16384")
endif()

# Enable space optimization for gcc/clang
# Cannot use "-ffunction-sections -fdata-sections" if we enable bitcode (iOS)
if (NOT MSVC AND NOT onnxruntime_ENABLE_BITCODE)
  string(APPEND CMAKE_CXX_FLAGS " -ffunction-sections -fdata-sections")
  string(APPEND CMAKE_C_FLAGS " -ffunction-sections -fdata-sections")
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -s ALLOW_UNIMPLEMENTED_SYSCALLS=1 -s DEFAULT_TO_CXX=1")

  # Enable LTO for release single-thread build
  if (NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    # NOTES:
    # (1) LTO does not work for WebAssembly multi-thread. (segment fault in worker)
    # (2) "-flto=thin" does not work correctly for wasm-ld.
    #     we don't set onnxruntime_ENABLE_LTO because it appends flag "-flto=thin"
    #     instead, we manually set CMAKE_CXX_FLAGS "-flto"
    string(APPEND CMAKE_C_FLAGS " -flto")
    string(APPEND CMAKE_CXX_FLAGS " -flto")
  endif()

  if (onnxruntime_ENABLE_WEBASSEMBLY_DEBUG_INFO)
    # "-g3" generates DWARF format debug info.
    # NOTE: With debug info enabled, web assembly artifacts will be very huge (>1GB). So we offer an option to build without debug info.
    set(CMAKE_CXX_FLAGS_DEBUG "-g3")
  else()
    set(CMAKE_CXX_FLAGS_DEBUG "-g2")
  endif()

  if (onnxruntime_ENABLE_WEBASSEMBLY_RELAXED_SIMD)
    string(APPEND CMAKE_C_FLAGS " -msimd128 -mrelaxed-simd")
    string(APPEND CMAKE_CXX_FLAGS " -msimd128 -mrelaxed-simd")
  elseif (onnxruntime_ENABLE_WEBASSEMBLY_SIMD)
    string(APPEND CMAKE_C_FLAGS " -msimd128")
    string(APPEND CMAKE_CXX_FLAGS " -msimd128")
  endif()

  if (onnxruntime_ENABLE_WEBASSEMBLY_EXCEPTION_CATCHING)
    string(APPEND CMAKE_C_FLAGS " -s DISABLE_EXCEPTION_CATCHING=0")
    string(APPEND CMAKE_CXX_FLAGS " -s DISABLE_EXCEPTION_CATCHING=0")
  endif()

  # Build WebAssembly with multi-threads support.
  if (onnxruntime_ENABLE_WEBASSEMBLY_THREADS)
    string(APPEND CMAKE_C_FLAGS " -pthread -Wno-pthreads-mem-growth")
    string(APPEND CMAKE_CXX_FLAGS " -pthread -Wno-pthreads-mem-growth")
  endif()
endif()

if (onnxruntime_EXTERNAL_TRANSFORMER_SRC_PATH)
  add_definitions(-DORT_TRAINING_EXTERNAL_GRAPH_TRANSFORMERS=1)
endif()

# ORT build with as much excluded as possible. Supports ORT flatbuffers models only.
if (onnxruntime_MINIMAL_BUILD)
  add_compile_definitions(ORT_MINIMAL_BUILD)

  if (onnxruntime_EXTENDED_MINIMAL_BUILD)
    # enable EPs that compile kernels at runtime
    add_compile_definitions(ORT_EXTENDED_MINIMAL_BUILD)
  endif()

  if (onnxruntime_MINIMAL_BUILD_CUSTOM_OPS)
    add_compile_definitions(ORT_MINIMAL_BUILD_CUSTOM_OPS)
  endif()

  if (MSVC)
    # undocumented internal flag to allow analysis of a minimal build binary size
    if (ADD_DEBUG_INFO_TO_MINIMAL_BUILD)
      string(APPEND CMAKE_CXX_FLAGS " /Zi")
      string(APPEND CMAKE_C_FLAGS " /Zi")
      string(APPEND CMAKE_SHARED_LINKER_FLAGS " /debug")
    endif()
  else()
    if (CMAKE_HOST_SYSTEM MATCHES "Darwin")
      add_link_options(-Wl, -dead_strip)
    else()
      add_link_options(-Wl,--gc-sections)
    endif()

    if (ADD_DEBUG_INFO_TO_MINIMAL_BUILD)
      string(APPEND CMAKE_CXX_FLAGS " -g")
      string(APPEND CMAKE_C_FLAGS " -g")
    endif()
  endif()
endif()

# ORT build with default settings more appropriate for client/on-device workloads.
if (onnxruntime_CLIENT_PACKAGE_BUILD)
  add_compile_definitions(ORT_CLIENT_PACKAGE_BUILD)
endif()

if (onnxruntime_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_enabled OUTPUT ipo_output)
    if (NOT ipo_enabled)
      message(WARNING "Interprocedural optimization (IPO) is not supported by this compiler. ${ipo_output}")
      set(onnxruntime_ENABLE_LTO OFF)
    else()
      set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

      # See https://cmake.org/cmake/help/latest/policy/CMP0069.html.
      #
      # "The OLD behavior for this policy is to add IPO flags only for Intel compiler on Linux.
      # The NEW behavior for this policy is to add IPO flags for the current compiler or produce an error if CMake does
      # not know the flags."
      #
      # CMake versions 3.8 and lower use the OLD behavior. This project requires CMake version > 3.8.
      # However, some dependencies may specify a lower required CMake version and default to the OLD behavior.
      # Ensure that CMake also uses the NEW behavior for such dependencies.
      set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
    endif()
endif()

if (onnxruntime_DISABLE_RTTI)
  add_compile_definitions(ORT_NO_RTTI)
  if (MSVC)
    # Disable RTTI and turn usage of dynamic_cast and typeid into errors
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/GR->" "$<$<COMPILE_LANGUAGE:CXX>:/we4541>")
  else()
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>")
    if (onnxruntime_USE_WEBNN)
      # Avoid unboundTypeError for WebNN EP since unbound type names are illegal with RTTI disabled
      # in Embind API, relevant issue: https://github.com/emscripten-core/emscripten/issues/7001
      add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-DEMSCRIPTEN_HAS_UNBOUND_TYPE_NAMES=0>")
    endif()
  endif()
else()
  #MSVC RTTI flag /GR is not added to CMAKE_CXX_FLAGS by default. But, anyway VC++2019 treats "/GR" default on.
  #So we don't need the following three lines. But it's better to make it more explicit.
  if (MSVC)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/GR>")
  endif()
endif()

# If this is only enabled in an onnxruntime_ORT_MODEL_FORMAT_ONLY build we don't need ONNX changes
# as we (currently) only pull in data_type_utils.cc/h which doesn't throw
if (onnxruntime_DISABLE_EXCEPTIONS)
  if (NOT onnxruntime_MINIMAL_BUILD)
    message(FATAL_ERROR "onnxruntime_MINIMAL_BUILD required for onnxruntime_DISABLE_EXCEPTIONS")
  endif()

  if (onnxruntime_ENABLE_PYTHON)
    # pybind11 highly depends on C++ exceptions.
    message(FATAL_ERROR "onnxruntime_ENABLE_PYTHON must be disabled for onnxruntime_DISABLE_EXCEPTIONS")
  endif()
  add_compile_definitions("ORT_NO_EXCEPTIONS")
  add_compile_definitions("MLAS_NO_EXCEPTION")
  add_compile_definitions("ONNX_NO_EXCEPTIONS")
  # The following line of code probably is not needed because the same effect can be achieved by setting the compiler flag -fno-exceptions.
  add_compile_definitions("JSON_NOEXCEPTION")  # https://json.nlohmann.me/api/macros/json_noexception/

  if (MSVC)
    string(REGEX REPLACE "/EHsc" "/EHs-c-" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    # Eigen throw_std_bad_alloc calls 'new' instead of throwing which results in a nodiscard warning.
    # It also has unreachable code as there's no good way to avoid EIGEN_EXCEPTIONS being set in macros.h
    # TODO: see if we can limit the code this is disabled for.
    string(APPEND CMAKE_CXX_FLAGS " /wd4834 /wd4702")
    add_compile_definitions("_HAS_EXCEPTIONS=0")
  else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables")
  endif()
endif()


# Guarantee that the Eigen code that you are #including is licensed
# under the MPL2 and possibly more permissive licenses (like BSD).
add_definitions(-DEIGEN_MPL2_ONLY)
if (MSVC)
  add_definitions(-DEIGEN_HAS_CONSTEXPR -DEIGEN_HAS_VARIADIC_TEMPLATES -DEIGEN_HAS_CXX11_MATH -DEIGEN_HAS_CXX11_ATOMIC
          -DEIGEN_STRONG_INLINE=inline)
endif()

if ( onnxruntime_DONT_VECTORIZE )
  add_definitions(-DEIGEN_DONT_VECTORIZE=1)
endif()

if (onnxruntime_CROSS_COMPILING)
  set(CMAKE_CROSSCOMPILING ON)
  check_cxx_compiler_flag(-Wno-error HAS_NOERROR)
  if (HAS_NOERROR)
    string(APPEND CMAKE_CXX_FLAGS " -Wno-error=attributes")
    string(APPEND CMAKE_C_FLAGS " -Wno-error=attributes")
  endif()
endif()

# Mark symbols to be invisible, for macOS/iOS/visionOS/tvOS target only
# Due to many dependencies have different symbol visibility settings, set global compile flags here.
if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin|iOS|visionOS|tvOS")
  foreach(flags CMAKE_CXX_FLAGS CMAKE_OBJC_FLAGS CMAKE_OBJCXX_FLAGS)
    string(APPEND ${flags} " -fvisibility=hidden -fvisibility-inlines-hidden")
  endforeach()
endif()


macro(check_nvcc_compiler_flag _FLAG _RESULT)
    execute_process(COMMAND ${CUDAToolkit_BIN_DIR}/nvcc "${_FLAG}" RESULT_VARIABLE NVCC_OUT ERROR_VARIABLE NVCC_ERROR)
    message("NVCC_ERROR = ${NVCC_ERROR}")
    message("NVCC_OUT = ${NVCC_OUT}")
    if ("${NVCC_OUT}" MATCHES "0")
        set(${_RESULT} 1)
    else()
        set(${_RESULT} 0)
    endif()
endmacro()

#Set global compile flags for all the source code(including third_party code like protobuf)
#This section must be before any add_subdirectory, otherwise build may fail because /MD,/MT mismatch
if (MSVC)
  if (CMAKE_VS_PLATFORM_NAME)
    # Multi-platform generator
    set(onnxruntime_target_platform ${CMAKE_VS_PLATFORM_NAME})
  else()
    set(onnxruntime_target_platform ${CMAKE_SYSTEM_PROCESSOR})
  endif()
  if (onnxruntime_target_platform STREQUAL "ARM64")
    set(onnxruntime_target_platform "ARM64")
    enable_language(ASM_MARMASM)
  elseif (onnxruntime_target_platform STREQUAL "ARM64EC")
    enable_language(ASM_MARMASM)
  elseif (onnxruntime_target_platform STREQUAL "ARM" OR CMAKE_GENERATOR MATCHES "ARM")
    set(onnxruntime_target_platform "ARM")
    enable_language(ASM_MARMASM)
  elseif (onnxruntime_target_platform STREQUAL "x64" OR onnxruntime_target_platform STREQUAL "x86_64" OR onnxruntime_target_platform STREQUAL "AMD64" OR CMAKE_GENERATOR MATCHES "Win64")
    set(onnxruntime_target_platform "x64")
    enable_language(ASM_MASM)
  elseif (onnxruntime_target_platform STREQUAL "Win32" OR onnxruntime_target_platform STREQUAL "x86" OR onnxruntime_target_platform STREQUAL "i386" OR onnxruntime_target_platform STREQUAL "i686")
    set(onnxruntime_target_platform "x86")
    enable_language(ASM_MASM)
    message("Enabling SAFESEH for x86 build")
    set(CMAKE_ASM_MASM_FLAGS "${CMAKE_ASM_MASM_FLAGS} /safeseh")
  else()
    message(FATAL_ERROR "Unknown CMAKE_SYSTEM_PROCESSOR: ${CMAKE_SYSTEM_PROCESSOR}")
  endif()

  #Always enable exception handling, even for Windows ARM
  if (NOT onnxruntime_DISABLE_EXCEPTIONS)
    string(APPEND CMAKE_CXX_FLAGS " /EHsc")
    string(APPEND CMAKE_C_FLAGS " /EHsc")

    string(APPEND CMAKE_CXX_FLAGS " /wd26812")
    string(APPEND CMAKE_C_FLAGS " /wd26812")
  endif()

  if (onnxruntime_USE_AVX)
    string(APPEND CMAKE_CXX_FLAGS " /arch:AVX")
    string(APPEND CMAKE_C_FLAGS " /arch:AVX")
  elseif (onnxruntime_USE_AVX2)
    string(APPEND CMAKE_CXX_FLAGS " /arch:AVX2")
    string(APPEND CMAKE_C_FLAGS " /arch:AVX2")
  elseif (onnxruntime_USE_AVX512)
    string(APPEND CMAKE_CXX_FLAGS " /arch:AVX512")
    string(APPEND CMAKE_C_FLAGS " /arch:AVX512")
  endif()

  if (onnxruntime_ENABLE_LTO AND NOT onnxruntime_USE_CUDA)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Gw /GL")
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} /Gw /GL")
    set(CMAKE_CXX_FLAGS_MINSIZEREL "${CMAKE_CXX_FLAGS_MINSIZEREL} /Gw /GL")
  endif()
else()
  if (NOT APPLE)
    #XXX: Sometimes the value of CMAKE_SYSTEM_PROCESSOR is set but it's wrong. For example, if you run an armv7 docker
    #image on an aarch64 machine with an aarch64 Ubuntu host OS, in the docker instance cmake may still report
    # CMAKE_SYSTEM_PROCESSOR as aarch64 by default. Given compiling this code may need more than 2GB memory, we do not
    # support compiling for ARM32 natively(only support cross-compiling), we will ignore this issue for now.
    if(NOT CMAKE_SYSTEM_PROCESSOR)
      message(WARNING "CMAKE_SYSTEM_PROCESSOR is not set. Please set it in your toolchain cmake file.")
      # Try to detect it
      if("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU" OR "${CMAKE_C_COMPILER_ID}" STREQUAL "Clang")
        execute_process(
		COMMAND "${CMAKE_C_COMPILER}" -dumpmachine
		OUTPUT_VARIABLE GCC_DUMP_MACHINE_OUT OUTPUT_STRIP_TRAILING_WHITESPACE
		ERROR_VARIABLE _err
		RESULT_VARIABLE _res
		)
		if(NOT _res EQUAL 0)
			message(SEND_ERROR "Failed to run 'gcc -dumpmachine':\n ${_res}")
		endif()
		string(REPLACE "-" ";" GCC_DUMP_MACHINE_OUT_LIST "${GCC_DUMP_MACHINE_OUT}")
		list(LENGTH GCC_DUMP_MACHINE_OUT_LIST GCC_TRIPLET_LEN)
		if(GCC_TRIPLET_LEN EQUAL 4)
		  list(GET GCC_DUMP_MACHINE_OUT_LIST 0 CMAKE_SYSTEM_PROCESSOR)
          message("Setting CMAKE_SYSTEM_PROCESSOR to ${CMAKE_SYSTEM_PROCESSOR}")
        endif()
      endif()
    endif()
    set(onnxruntime_target_platform ${CMAKE_SYSTEM_PROCESSOR})
  endif()
  if (onnxruntime_BUILD_FOR_NATIVE_MACHINE)
    string(APPEND CMAKE_CXX_FLAGS " -march=native -mtune=native")
    string(APPEND CMAKE_C_FLAGS " -march=native -mtune=native")
  elseif (onnxruntime_USE_AVX)
    string(APPEND CMAKE_CXX_FLAGS " -mavx")
    string(APPEND CMAKE_C_FLAGS " -mavx")
  elseif (onnxruntime_USE_AVX2)
    string(APPEND CMAKE_CXX_FLAGS " -mavx2")
    string(APPEND CMAKE_C_FLAGS " -mavx2")
  elseif (onnxruntime_USE_AVX512)
    string(APPEND CMAKE_CXX_FLAGS " -mavx512f -mavx512cd -mavx512bw -mavx512dq -mavx512vl")
    string(APPEND CMAKE_C_FLAGS " -mavx512f -mavx512cd -mavx512bw -mavx512dq -mavx512vl")
  endif()
  if (CMAKE_SYSTEM_NAME STREQUAL "Android" AND onnxruntime_GCOV_COVERAGE)
    string(APPEND CMAKE_CXX_FLAGS " -g -O0 --coverage ")
    string(APPEND CMAKE_C_FLAGS   " -g -O0 --coverage ")
  endif()
  if("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
    if(onnxruntime_USE_XNNPACK)
      # https://github.com/google/XNNPACK/issues/7650
      string(APPEND CMAKE_C_FLAGS   " -Wno-incompatible-pointer-types ")
    endif()
  endif()
  # Check support for AVX and f16c.
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag("-mf16c" COMPILER_SUPPORT_MF16C)
  if (NOT COMPILER_SUPPORT_MF16C)
    message("F16C instruction set is not supported.")
  endif()

  check_cxx_compiler_flag("-mfma" COMPILER_SUPPORT_FMA)
  if (NOT COMPILER_SUPPORT_FMA)
    message("FMA instruction set is not supported.")
  endif()

  check_cxx_compiler_flag("-mavx" COMPILER_SUPPORT_AVX)
  if (NOT COMPILER_SUPPORT_AVX)
    message("AVX instruction set is not supported.")
  endif()

  if (CMAKE_SYSTEM_NAME STREQUAL "Android" AND onnxruntime_ENABLE_TRAINING_APIS)
    message("F16C, FMA and AVX flags are not supported on Android for ort training.")
  endif()

  if (NOT (COMPILER_SUPPORT_MF16C AND COMPILER_SUPPORT_FMA AND COMPILER_SUPPORT_AVX) OR
    (CMAKE_SYSTEM_NAME STREQUAL "Android" AND onnxruntime_ENABLE_TRAINING_APIS))
    message("One or more AVX/F16C instruction flags are not supported. ")
    set(onnxruntime_ENABLE_CPU_FP16_OPS FALSE)
  endif()

endif()

if (WIN32)
    # required to be set explicitly to enable Eigen-Unsupported SpecialFunctions
    string(APPEND CMAKE_CXX_FLAGS " -DEIGEN_HAS_C99_MATH")
elseif(LINUX)
    add_compile_definitions("_GNU_SOURCE")
endif()

if (onnxruntime_USE_EXTENSIONS)
    include_directories(${REPO_ROOT}/include/onnxruntime/core/session)
endif()
