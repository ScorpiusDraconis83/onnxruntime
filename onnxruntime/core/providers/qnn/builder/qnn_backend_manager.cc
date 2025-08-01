//
// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "qnn_backend_manager.h"
#include "qnn_model.h"
#include <filesystem>
#include <fstream>
#include <string>
#include "QnnOpDef.h"
#include "CPU/QnnCpuCommon.h"
#include "GPU/QnnGpuCommon.h"
#include "DSP/QnnDspCommon.h"
#include "HTP/QnnHtpCommon.h"
#include "HTP/QnnHtpContext.h"
#include "HTP/QnnHtpPerfInfrastructure.h"
#include "HTP/QnnHtpSystemContext.h"
#include "IR/QnnIrCommon.h"
#include "IR/QnnIrGraph.h"
#include "Saver/QnnSaver.h"
#include "Saver/QnnSaverCommon.h"
#include <gsl/gsl>

#include "core/providers/qnn/ort_api.h"
#include "core/providers/qnn/qnn_allocator.h"
#include "core/providers/qnn/qnn_telemetry.h"
#include "core/providers/qnn/shared_context.h"
#include "core/providers/qnn/builder/onnx_ctx_model_helper.h"
#include "core/providers/qnn/builder/qnn_configs_helper.h"
#include "core/providers/qnn/builder/qnn_utils.h"

// Flag to determine if Backend should do node validation for each opNode added
#define DO_GRAPH_NODE_VALIDATIONS 1

namespace onnxruntime {
namespace qnn {

typedef Qnn_ErrorHandle_t (*QnnInterfaceGetProvidersFn_t)(const QnnInterface_t*** providerList,
                                                          uint32_t* numProviders);
typedef Qnn_ErrorHandle_t (*QnnSystemInterfaceGetProvidersFn_t)(const QnnSystemInterface_t*** providerList,
                                                                uint32_t* numProviders);

static Qnn_Version_t GetQnnInterfaceApiVersion(const QnnInterface_t* qnn_interface) {
  return qnn_interface->apiVersion.coreApiVersion;
}

static Qnn_Version_t GetQnnInterfaceApiVersion(const QnnSystemInterface_t* qnn_interface) {
  return qnn_interface->systemApiVersion;
}

static const char* DlError() {
#ifdef _WIN32
  return "";
#else
  return ::dlerror();
#endif
}

// Workaround for a missing comma in QNN_IR_GRAPH_CUSTOM_CONFIG_INIT.
static QnnIrGraph_CustomConfig_t EmptyIrGraphConfig() {
  return {
      QNN_IR_GRAPH_CONFIG_OPTION_SERIALIZATION, {QNN_IR_GRAPH_SERIALIZATION_TYPE_FLAT_BUFFER, ""}};
}

class QnnIrConfig : public QnnSerializerConfig {
 public:
  QnnIrConfig(std::string backend_path, std::string dlc_dir)
      : QnnSerializerConfig(std::move(backend_path)), dlc_dir_(std::move(dlc_dir)), configs_builder_(MakeConfigsBuilder()) {
  }

  const QnnGraph_Config_t** Configure() override {
    auto configs_builder = MakeConfigsBuilder();

    std::filesystem::path dlc_path = (dlc_dir_ / (GetGraphName() + ".dlc"));
    std::string dlc_path_str = dlc_path.string();
    gsl::not_null<QnnIrGraph_CustomConfig_t*> dlc_path_config = configs_builder.PushCustomConfig();
    dlc_path_config->option = QNN_IR_GRAPH_CONFIG_OPTION_SERIALIZATION;
    dlc_path_config->serializationOption.serializationType = QNN_IR_GRAPH_SERIALIZATION_TYPE_FLAT_BUFFER;
    dlc_path_config->serializationOption.outputPath = dlc_path_str.c_str();

    gsl::not_null<QnnGraph_Config_t*> dlc_path_custom_config = configs_builder.PushConfig();
    dlc_path_custom_config->option = QNN_GRAPH_CONFIG_OPTION_CUSTOM;
    dlc_path_custom_config->customConfig = dlc_path_config;

    std::filesystem::create_directories(dlc_path);

    // Keep the pointer to dlc_path_str's null-terminated string alive.
    std::swap(dlc_path_str, dlc_path_str_);

    std::swap(configs_builder, configs_builder_);
    return configs_builder_.GetQnnConfigs();
  }

  bool SupportsArbitraryGraphConfigs() const override {
    return false;
  }

 private:
  static QnnConfigsBuilder<QnnGraph_Config_t, QnnIrGraph_CustomConfig_t> MakeConfigsBuilder() {
    return QnnConfigsBuilder<QnnGraph_Config_t, QnnIrGraph_CustomConfig_t>(QNN_GRAPH_CONFIG_INIT, EmptyIrGraphConfig());
  }

  std::filesystem::path dlc_dir_;
  std::string dlc_path_str_;
  QnnConfigsBuilder<QnnGraph_Config_t, QnnIrGraph_CustomConfig_t> configs_builder_;
};

class QnnSaverConfig : public QnnSerializerConfig {
 public:
  QnnSaverConfig(std::string backend_path) : QnnSerializerConfig(std::move(backend_path)) {}

  const QnnGraph_Config_t** Configure() override {
    return nullptr;
  }

  bool SupportsArbitraryGraphConfigs() const override {
    return true;
  }
};

QnnSerializerConfig::~QnnSerializerConfig() = default;

QnnSerializerConfig::QnnSerializerConfig(std::string backend_path)
    : backend_path_(std::move(backend_path)) {}

std::unique_ptr<QnnSerializerConfig> QnnSerializerConfig::CreateIr(std::string backend_path, std::string dlc_dir) {
  return std::make_unique<QnnIrConfig>(std::move(backend_path), std::move(dlc_dir));
}

std::unique_ptr<QnnSerializerConfig> QnnSerializerConfig::CreateSaver(std::string backend_path) {
  return std::make_unique<QnnSaverConfig>(std::move(backend_path));
}

const std::string& QnnSerializerConfig::GetBackendPath() const {
  return backend_path_;
}

const std::string& QnnSerializerConfig::GetGraphName() const {
  return graph_name_;
}

void QnnSerializerConfig::SetGraphName(std::string graph_name) {
  graph_name_ = std::move(graph_name);
}

Status ReadBinaryFromFile(const std::string& file_path, uint8_t* buffer, size_t buffer_size) {
  ORT_RETURN_IF(nullptr == buffer, "Binary buffer is nullptr");
  std::ifstream in(file_path, std::ifstream::binary);
  ORT_RETURN_IF(!in, "Failed to open input file: ", file_path.c_str());
  ORT_RETURN_IF(!in.read(reinterpret_cast<char*>(buffer), buffer_size), "Failed to read the contents of: ", file_path.c_str());
  return Status::OK();
}

Status QnnBackendManager::ParseLoraConfig(std::string lora_config_path) {
  LOGS_DEFAULT(INFO) << "Acquiring the QnnInterface " << lora_config_path;

  // QNN Lora Config file format should be a single line, with the graph name first,
  // followed by the qnn lora context binary path, separated by a semicolon (;)
  // Example: <graph_name>;<binary_path>
  LOGS_DEFAULT(INFO) << "Loading Lora Config " << lora_config_path;
  std::ifstream file(lora_config_path);
  std::string line;

  if (file.is_open()) {
    if (std::getline(file, line)) {
      std::istringstream ss(line);
      std::string graph_name;
      std::string lora_adapter_bin_path;

      if (std::getline(ss, graph_name, ';') && std::getline(ss, lora_adapter_bin_path)) {
        size_t buffer_size = std::filesystem::file_size(lora_adapter_bin_path.c_str());

        ORT_RETURN_IF(0 == buffer_size, "Received path to an empty file. Nothing to deserialize.");
        std::unique_ptr<uint8_t[]> buffer = std::make_unique<uint8_t[]>(buffer_size);
        void* voidBufferPtr = static_cast<void*>(buffer.get());
        QnnContext_Buffer_t contextBuffer{QNN_CONTEXT_BUFFER_VERSION_1,
                                          {QNN_CONTEXTMEMTYPE_RAW, {{voidBufferPtr, buffer_size}}}};

        auto status = ReadBinaryFromFile(lora_adapter_bin_path,
                                         reinterpret_cast<uint8_t*>(buffer.get()),
                                         buffer_size);

        ORT_RETURN_IF(status != Status::OK(), "Failed to read binary data.");
        Qnn_GraphHandle_t graph;
        bool graph_retrieve_success = false;
        for (size_t cIdx = 0; cIdx < contexts_.size(); cIdx++) {
          auto graph_retrieve_rt = qnn_interface_.graphRetrieve(contexts_[cIdx], graph_name.c_str(), &graph);
          if (QNN_SUCCESS != graph_retrieve_rt) {
            continue;
          }

          graph_retrieve_success = true;

          auto context_apply_binary_section_rt = qnn_interface_.contextApplyBinarySection(
              contexts_[cIdx], graph, QNN_CONTEXT_SECTION_UPDATABLE, &contextBuffer, profile_backend_handle_, nullptr);
          ORT_RETURN_IF(QNN_SUCCESS != context_apply_binary_section_rt, "Failed to apply binary section.");
          break;
        }
        ORT_RETURN_IF_NOT(graph_retrieve_success, "Failed to retrieve graph: ", graph_name, " and apply binary section.");
      }
    }
    file.close();
  } else {
    LOGS_DEFAULT(ERROR) << "Unable to load Lora Config " << lora_config_path;
  }

  return Status::OK();
}

template <typename F, class T>
Status QnnBackendManager::GetQnnInterfaceProvider(const char* lib_path,
                                                  const char* interface_provider_name,
                                                  void** backend_lib_handle,
                                                  Qnn_Version_t req_version,
                                                  T** interface_provider) {
  std::string error_msg;
  *backend_lib_handle = LoadLib(lib_path,
                                static_cast<int>(DlOpenFlag::DL_NOW) | static_cast<int>(DlOpenFlag::DL_GLOBAL),
                                error_msg);
  ORT_RETURN_IF(nullptr == *backend_lib_handle, "Unable to load backend, error: ", error_msg, " ", DlError());

  // Get QNN Interface providers
  F GetInterfaceProviders{nullptr};
  GetInterfaceProviders = ResolveSymbol<F>(*backend_lib_handle, interface_provider_name, *logger_);
  ORT_RETURN_IF(nullptr == GetInterfaceProviders, "Failed to get QNN providers!");

  T** interface_providers{nullptr};
  uint32_t num_providers{0};

  auto result = GetInterfaceProviders((const T***)&interface_providers, &num_providers);
  ORT_RETURN_IF((QNN_SUCCESS != result || nullptr == *interface_providers || 0 == num_providers),
                "Failed to get QNN providers.");

  bool found_valid_interface{false};
  for (size_t pIdx = 0; pIdx < num_providers; pIdx++) {
    Qnn_Version_t interface_version = GetQnnInterfaceApiVersion(interface_providers[pIdx]);

    LOGS_DEFAULT(VERBOSE) << lib_path << " interface version: " << interface_version.major << "."
                          << interface_version.minor << "." << interface_version.patch;

    // Check the interface's API version against the required version.
    // Major versions must match. The interface's minor version must be greater OR equal with a suitable patch version.
    if (interface_version.major == req_version.major) {
      bool minor_and_patch_version_ok = (interface_version.minor > req_version.minor) ||
                                        (interface_version.minor == req_version.minor &&
                                         interface_version.patch >= req_version.patch);
      if (minor_and_patch_version_ok) {
        found_valid_interface = true;
        *interface_provider = interface_providers[pIdx];
        break;
      }
    }
  }

  ORT_RETURN_IF_NOT(found_valid_interface, "Unable to find a valid interface for ", lib_path);

  return Status::OK();
}

void QnnBackendManager::SetQnnBackendType(uint32_t backend_id) {
  switch (backend_id) {
    case QNN_BACKEND_ID_CPU:
      qnn_backend_type_ = QnnBackendType::CPU;
      break;
    case QNN_BACKEND_ID_GPU:
      qnn_backend_type_ = QnnBackendType::GPU;
      break;
    case QNN_BACKEND_ID_DSP:
      qnn_backend_type_ = QnnBackendType::DSP;
      break;
    case QNN_BACKEND_ID_HTP:
      qnn_backend_type_ = QnnBackendType::HTP;
      break;
    case QNN_BACKEND_ID_IR:
    case QNN_BACKEND_ID_SAVER:
      qnn_backend_type_ = QnnBackendType::SERIALIZER;
      break;
    default:
      qnn_backend_type_ = QnnBackendType::CPU;
      break;
  }
}

Status QnnBackendManager::LoadBackend() {
  QnnInterface_t* backend_interface_provider{nullptr};
  auto rt = GetQnnInterfaceProvider<QnnInterfaceGetProvidersFn_t,
                                    QnnInterface_t>(backend_path_.c_str(),
                                                    "QnnInterface_getProviders",
                                                    &backend_lib_handle_,
                                                    {QNN_API_VERSION_MAJOR,
                                                     QNN_API_VERSION_MINOR,
                                                     QNN_API_VERSION_PATCH},
                                                    &backend_interface_provider);
  ORT_RETURN_IF_ERROR(rt);
  qnn_interface_ = backend_interface_provider->QNN_INTERFACE_VER_NAME;
  auto backend_id = backend_interface_provider->backendId;
  SetQnnBackendType(backend_id);

  Qnn_Version_t backend_interface_version = GetQnnInterfaceApiVersion(backend_interface_provider);
  LOGS_DEFAULT(INFO) << "Found valid interface, version: " << backend_interface_version.major
                     << "." << backend_interface_version.minor << "." << backend_interface_version.patch
                     << " backend provider name: " << backend_interface_provider->providerName
                     << " backend id: " << backend_id;

  return Status::OK();
}

QnnSerializerConfig* QnnBackendManager::GetQnnSerializerConfig() {
  return qnn_serializer_config_.get();
}

// Loads the intended backend (e.g., HTP, CPU, etc) to get its type, and then
// sets QnnSaver or QnnIr as the active backend. QNN op builders will still see the intended backend
// (e.g., HTP) as the backend type to ensure they emit the expected QNN API calls. Note, however, that
// calls to QnnBackend_validateOpConfig will be to the saver backend, not the "intended" one.
//
// QnnSaver and QnnIr are "debugging" backends that serializes all QNN API calls (and weights) into
// local files: Saver dumps to C++ sources and Ir to .dlc archives.
// This information can be used to debug issues by replaying QNN API calls with another backend.
Status QnnBackendManager::LoadQnnSerializerBackend() {
  void* backend_lib_handle = nullptr;

  // Helper that unloads the intended backend library handle when the `unload_backend_lib` variable
  // goes out of scope. Similar to `defer` in other languages.
  auto unload_backend_lib = gsl::finally([&] {
    if (backend_lib_handle != nullptr) {
      auto result = UnloadLib(backend_lib_handle);
      if (Status::OK() != result) {
        ORT_THROW("Failed to unload backend library.");
      }
    }
  });

  // Load the intended backend (e.g., HTP, CPU) to ensure it is valid and to get its type.
  QnnInterface_t* backend_interface_provider{nullptr};
  auto rt = GetQnnInterfaceProvider<QnnInterfaceGetProvidersFn_t,
                                    QnnInterface_t>(backend_path_.c_str(),
                                                    "QnnInterface_getProviders",
                                                    &backend_lib_handle,
                                                    {QNN_API_VERSION_MAJOR,
                                                     QNN_API_VERSION_MINOR,
                                                     QNN_API_VERSION_PATCH},
                                                    &backend_interface_provider);
  ORT_RETURN_IF_ERROR(rt);

  // Set the "intended" backend type so that QNN builders still make the expected QNN API calls.
  auto backend_id = backend_interface_provider->backendId;
  SetQnnBackendType(backend_id);

  // Load the serializer backend and set it as the activate backend.
  QnnInterface_t* serializer_interface_provider{nullptr};
  auto saver_rt = GetQnnInterfaceProvider<QnnInterfaceGetProvidersFn_t,
                                          QnnInterface_t>(qnn_serializer_config_->GetBackendPath().c_str(),
                                                          "QnnInterface_getProviders",
                                                          &backend_lib_handle_,  // NOTE: QnnSaver/Ir library handle is set
                                                          {QNN_API_VERSION_MAJOR,
                                                           QNN_API_VERSION_MINOR,
                                                           QNN_API_VERSION_PATCH},
                                                          &serializer_interface_provider);
  ORT_RETURN_IF_ERROR(saver_rt);
  qnn_interface_ = serializer_interface_provider->QNN_INTERFACE_VER_NAME;  // NOTE: QnnSaver/Ir will provide the interfaces

  Qnn_Version_t backend_interface_version = GetQnnInterfaceApiVersion(backend_interface_provider);
  Qnn_Version_t serializer_interface_version = GetQnnInterfaceApiVersion(serializer_interface_provider);

  LOGS_DEFAULT(INFO) << "Using QnnSaver/Ir version: " << serializer_interface_version.major << "."
                     << serializer_interface_version.minor << "." << serializer_interface_version.patch
                     << " provider name : " << serializer_interface_provider->providerName;

  LOGS_DEFAULT(INFO) << "Intended backend provider name: " << backend_interface_provider->providerName
                     << " backend id: " << backend_id
                     << " interface version: " << backend_interface_version.major
                     << "." << backend_interface_version.minor << "." << backend_interface_version.patch;

  return Status::OK();
}

Status QnnBackendManager::LoadQnnSystemLib() {
#ifdef _WIN32
  std::string system_lib_file = "QnnSystem.dll";
#else
  std::string system_lib_file = "libQnnSystem.so";
#endif  // #ifdef _WIN32
  LOGS_DEFAULT(INFO) << "Loading QnnSystem lib";
  std::filesystem::path lib_file_path(backend_path_.c_str());
  std::string sys_file_path(lib_file_path.remove_filename().string() + system_lib_file);
  QnnSystemInterface_t* system_interface_provider{nullptr};
  auto rt = GetQnnInterfaceProvider<QnnSystemInterfaceGetProvidersFn_t,
                                    QnnSystemInterface_t>(sys_file_path.c_str(),
                                                          "QnnSystemInterface_getProviders",
                                                          &system_lib_handle_,
                                                          {QNN_SYSTEM_API_VERSION_MAJOR,
                                                           QNN_SYSTEM_API_VERSION_MINOR,
                                                           QNN_SYSTEM_API_VERSION_PATCH},
                                                          &system_interface_provider);
  ORT_RETURN_IF_ERROR(rt);
  Qnn_Version_t system_interface_version = GetQnnInterfaceApiVersion(system_interface_provider);
  qnn_sys_interface_ = system_interface_provider->QNN_SYSTEM_INTERFACE_VER_NAME;

  LOGS_DEFAULT(INFO) << "Found valid system interface, version: " << system_interface_version.major
                     << "." << system_interface_version.minor
                     << " backend provider name: " << system_interface_provider->providerName;

  return Status::OK();
}

void QnnLogging(const char* format,
                QnnLog_Level_t level,
                uint64_t timestamp,
                va_list argument_parameter) {
  ORT_UNUSED_PARAMETER(level);
  ORT_UNUSED_PARAMETER(timestamp);

  if (!::onnxruntime::logging::LoggingManager::HasDefaultLogger()) {
    // QNN may call this logging callback at any point, which means that we need to explicitly check
    // that the default logger has been initialized before trying to use it (otherwise get segfault).
    return;
  }

  const auto& logger = ::onnxruntime::logging::LoggingManager::DefaultLogger();
  const auto severity = ::onnxruntime::logging::Severity::kVERBOSE;
  const auto data_type = ::onnxruntime::logging::DataType::SYSTEM;

  if (logger.OutputIsEnabled(severity, data_type)) {
    auto log_capture = Factory<logging::Capture>::Create(logger,
                                                         severity,
                                                         logging::Category::onnxruntime,
                                                         data_type,
                                                         ORT_WHERE);
    log_capture->ProcessPrintf(format, argument_parameter);
  }
}

Status QnnBackendManager::InitializeQnnLog(const logging::Logger& logger) {
  logger_ = &logger;

  // Set Qnn log level align with Ort log level
  auto ort_log_level = logger_->GetSeverity();
  QnnLog_Level_t qnn_log_level = MapOrtSeverityToQNNLogLevel(ort_log_level);
  LOGS(*logger_, VERBOSE) << "Set Qnn log level: " << qnn_log_level;

  // NOTE: Even if logCreate() fails and QNN does not return a valid log_handle_, QNN may still
  // call the QnnLogging() callback. So, we have to make sure that QnnLogging() can handle calls
  // in which ORT logging is not available.
  Qnn_ErrorHandle_t result = qnn_interface_.logCreate(QnnLogging, qnn_log_level, &log_handle_);

  if (result != QNN_SUCCESS) {
    switch (result) {
      case QNN_COMMON_ERROR_NOT_SUPPORTED:
        LOGS(*logger_, ERROR) << "Logging not supported in the QNN backend.";
        break;
      case QNN_LOG_ERROR_INVALID_ARGUMENT:
        LOGS(*logger_, ERROR) << "Invalid argument provided to QnnLog_create.";
        break;
      case QNN_LOG_ERROR_MEM_ALLOC:
        LOGS(*logger_, ERROR) << "Memory allocation error during QNN logging initialization.";
        break;
      case QNN_LOG_ERROR_INITIALIZATION:
        LOGS(*logger_, ERROR) << "Initialization of logging failed in the QNN backend.";
        break;
      default:
        LOGS(*logger_, WARNING) << "Unknown error occurred while initializing logging in the QNN backend.";
        break;
    }
  }

  ORT_RETURN_IF(QNN_BACKEND_NO_ERROR != result, "Failed to initialize logging in the QNN backend. Error: ", QnnErrorHandleToString(result));
  return Status::OK();
}

QnnLog_Level_t QnnBackendManager::MapOrtSeverityToQNNLogLevel(logging::Severity ort_log_level) {
  // Map ORT log severity to Qnn log level
  switch (ort_log_level) {
    case logging::Severity::kVERBOSE: {
      switch ((GetQnnBackendType())) {
        case QnnBackendType::GPU:
          // Currently GPU needs this log level to work.
          // This switch will be removed once this is resolved.
          return QNN_LOG_LEVEL_DEBUG;
        default:
          return QNN_LOG_LEVEL_VERBOSE;
      }
    }
    case logging::Severity::kINFO:
      return QNN_LOG_LEVEL_INFO;
    case logging::Severity::kWARNING:
      return QNN_LOG_LEVEL_WARN;
    case logging::Severity::kERROR:
    case logging::Severity::kFATAL:
    default:
      return QNN_LOG_LEVEL_ERROR;
  }
}

Status QnnBackendManager::ResetQnnLogLevel(std::optional<logging::Severity> ort_log_level) {
  std::lock_guard<std::recursive_mutex> lock(logger_recursive_mutex_);
  if (!backend_setup_completed_ || logger_ == nullptr) {
    return Status::OK();
  }
  ORT_RETURN_IF(nullptr == log_handle_, "Unable to update QNN Log Level. Invalid QNN log handle.");

  logging::Severity actual_log_level = ort_log_level.has_value() ? *ort_log_level : logger_->GetSeverity();
  QnnLog_Level_t qnn_log_level = MapOrtSeverityToQNNLogLevel(actual_log_level);

  LOGS(*logger_, INFO) << "Updating Qnn log level to: " << qnn_log_level;

  // Use the QnnLog_setLogLevel API to set the new log level
  Qnn_ErrorHandle_t result = qnn_interface_.logSetLogLevel(log_handle_, qnn_log_level);
  if (QNN_SUCCESS != result) {
    if (result == QNN_LOG_ERROR_INVALID_ARGUMENT) {
      LOGS(*logger_, ERROR) << "Invalid log level argument provided to QnnLog_setLogLevel.";
    } else if (result == QNN_LOG_ERROR_INVALID_HANDLE) {
      LOGS(*logger_, ERROR) << "Invalid log handle provided to QnnLog_setLogLevel.";
    }
  }
  ORT_RETURN_IF(QNN_BACKEND_NO_ERROR != result,
                "Failed to set log level in Qnn backend. Error: ", QnnErrorHandleToString(result));
  return Status::OK();
}

Status QnnBackendManager::InitializeBackend() {
  if (true == backend_initialized_) {
    LOGS_DEFAULT(INFO) << "Backend initialized already.";
    return Status::OK();
  }

  Qnn_ErrorHandle_t result = qnn_interface_.backendCreate(log_handle_, (const QnnBackend_Config_t**)backend_config_, &backend_handle_);
  ORT_RETURN_IF(QNN_BACKEND_NO_ERROR != result, "Failed to initialize backend. Error: ", QnnErrorHandleToString(result));

  backend_initialized_ = true;
  return Status::OK();
}

Status QnnBackendManager::ShutdownBackend() {
  if (false == backend_initialized_) {
    return Status::OK();
  }

  if (nullptr != qnn_interface_.backendFree) {
    ORT_RETURN_IF(QNN_BACKEND_NO_ERROR != qnn_interface_.backendFree(backend_handle_),
                  "Failed to shutdown backend!");
  }

  backend_initialized_ = false;

  return Status::OK();
}

bool QnnBackendManager::IsDevicePropertySupported() {
  if (nullptr != qnn_interface_.propertyHasCapability) {
    auto rt = qnn_interface_.propertyHasCapability(QNN_PROPERTY_GROUP_DEVICE);
    if (QNN_PROPERTY_NOT_SUPPORTED == rt || QNN_PROPERTY_ERROR_UNKNOWN_KEY == rt) {
      LOGS_DEFAULT(INFO) << "Device property not supported or unknown to backend.";
      return false;
    }
  }

  return true;
}

Status QnnBackendManager::CreateDevice() {
  if (true == device_created_) {
    LOGS_DEFAULT(INFO) << "Device initialized already.";
    return Status::OK();
  }

  // Create device if its property supported
  if (!IsDevicePropertySupported()) {
    LOGS_DEFAULT(INFO) << "Skip to create device.";
    return Status::OK();
  }

  qnn::QnnConfigsBuilder<QnnDevice_Config_t, QnnHtpDevice_CustomConfig_t> device_configs_builder(QNN_DEVICE_CONFIG_INIT,
                                                                                                 {});
  if (qnn_backend_type_ == QnnBackendType::HTP) {
    // Set SoC Model. The *enum* Qnn_SocModel_t is deprecated and will not be updated in the future. Therefore,
    // must use the latest SDK documentation to get the SoC model of the latest HW.
    if (soc_model_ != QNN_SOC_MODEL_UNKNOWN) {
      gsl::not_null<QnnHtpDevice_CustomConfig_t*> custom_config = device_configs_builder.PushCustomConfig();
      custom_config->option = QNN_HTP_DEVICE_CONFIG_OPTION_SOC;
      custom_config->socModel = soc_model_;

      gsl::not_null<QnnDevice_Config_t*> device_config = device_configs_builder.PushConfig();
      device_config->option = QNN_DEVICE_CONFIG_OPTION_CUSTOM;
      device_config->customConfig = custom_config;
    }

    // Set the minimum HTP architecture. The driver will use ops that are compatible with this minimum architecture.
    if (htp_arch_ != QNN_HTP_DEVICE_ARCH_NONE) {
      gsl::not_null<QnnHtpDevice_CustomConfig_t*> custom_config = device_configs_builder.PushCustomConfig();
      custom_config->option = QNN_HTP_DEVICE_CONFIG_OPTION_ARCH;
      custom_config->arch.arch = htp_arch_;
      custom_config->arch.deviceId = device_id_;

      gsl::not_null<QnnDevice_Config_t*> device_config = device_configs_builder.PushConfig();
      device_config->option = QNN_DEVICE_CONFIG_OPTION_CUSTOM;
      device_config->customConfig = custom_config;
    }
  }

  LOGS_DEFAULT(INFO) << "Create device.";
  if (nullptr != qnn_interface_.deviceCreate) {
    Qnn_ErrorHandle_t result = qnn_interface_.deviceCreate(log_handle_, device_configs_builder.GetQnnConfigs(), &device_handle_);
    if (QNN_SUCCESS != result) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Failed to create device. Error: ", QnnErrorHandleToString(result));
    }
  }
  device_created_ = true;

  return Status::OK();
}

Status QnnBackendManager::ReleaseDevice() {
  if (false == device_created_) {
    return Status::OK();
  }

  if (nullptr != qnn_interface_.deviceFree) {
    Qnn_ErrorHandle_t result = qnn_interface_.deviceFree(device_handle_);
    if (QNN_SUCCESS != result) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Failed to release device. Error: ", QnnErrorHandleToString(result));
    }
  }

  device_created_ = false;

  return Status::OK();
}

Status QnnBackendManager::InitializeProfiling() {
  profiling_level_merge_ = profiling_level_;
  // use profiling level from ETW if ETW is enabled
  if (profiling_level_etw_ != ProfilingLevel::INVALID) {
    profiling_level_merge_ = profiling_level_etw_;
  }

  if (ProfilingLevel::OFF == profiling_level_merge_ || ProfilingLevel::INVALID == profiling_level_merge_) {
    LOGS_DEFAULT(INFO) << "Profiling turned off.";
    return Status::OK();
  }

  QnnProfile_Level_t qnn_profile_level = QNN_PROFILE_LEVEL_BASIC;
  if (ProfilingLevel::BASIC == profiling_level_merge_) {
    qnn_profile_level = QNN_PROFILE_LEVEL_BASIC;
    LOGS_DEFAULT(VERBOSE) << "Profiling level set to basic.";
  } else if (ProfilingLevel::DETAILED == profiling_level_merge_) {
    qnn_profile_level = QNN_PROFILE_LEVEL_DETAILED;
    LOGS_DEFAULT(VERBOSE) << "Profiling level set to detailed.";
  }
  Qnn_ErrorHandle_t result = qnn_interface_.profileCreate(backend_handle_, qnn_profile_level, &profile_backend_handle_);
  ORT_RETURN_IF(QNN_PROFILE_NO_ERROR != result, "Failed to create QNN profile! Error: ", QnnErrorHandleToString(result));

  return Status::OK();
}

Status QnnBackendManager::ReleaseProfilehandle() {
  // Free Profiling object if it was created
  if (nullptr != profile_backend_handle_) {
    ORT_RETURN_IF(QNN_PROFILE_NO_ERROR != qnn_interface_.profileFree(profile_backend_handle_),
                  "Could not free backend profile handle!");
  }
  profile_backend_handle_ = nullptr;

  return Status::OK();
}

Status QnnBackendManager::SetProfilingLevelETW(ProfilingLevel profiling_level_etw_param) {
  if (profiling_level_etw_ != profiling_level_etw_param) {
    profiling_level_etw_ = profiling_level_etw_param;

    auto result = ReleaseProfilehandle();
    if (Status::OK() != result) {
      ORT_THROW("Failed to ReleaseProfilehandle for previous QNN profiling");
    }

    result = InitializeProfiling();
    if (Status::OK() != result) {
      ORT_THROW("Failed to Re-InitializeProfiling for QNN ETW profiling");
    }
  }
  return Status::OK();
}

Status SetQnnContextConfig(ContextPriority context_priority, QnnContext_Config_t& qnn_context_config) {
  qnn_context_config.option = QNN_CONTEXT_CONFIG_OPTION_PRIORITY;
  switch (context_priority) {
    case ContextPriority::LOW: {
      qnn_context_config.priority = QNN_PRIORITY_LOW;
      break;
    }
    case ContextPriority::NORMAL: {
      qnn_context_config.priority = QNN_PRIORITY_NORMAL;
      break;
    }
    case ContextPriority::NORMAL_HIGH: {
      qnn_context_config.priority = QNN_PRIORITY_NORMAL_HIGH;
      break;
    }
    case ContextPriority::HIGH: {
      qnn_context_config.priority = QNN_PRIORITY_HIGH;
      break;
    }
    case ContextPriority::UNDEFINED: {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Invalid Qnn context priority.");
    }
    default:
      qnn_context_config.priority = QNN_PRIORITY_NORMAL;
  }  // switch

  return Status::OK();
}

// callback required to add context handles to class list
// when using contextCreateFromBinaryListAsync()
void ContextCreateAsyncCallback(Qnn_ContextHandle_t context,
                                Qnn_GraphHandle_t graph,
                                const char* graphName,
                                QnnContext_createFromBinaryAsyncNotifyType_t notifyType,
                                void* notifyParam,
                                Qnn_ErrorHandle_t status) {
  auto qnn_backend_manager = SharedContext::GetInstance().GetSharedQnnBackendManager();

  if (context) {
    qnn_backend_manager->ProcessContextFromBinListAsync(context, notifyParam);
  }

  if (nullptr == graphName || graph || notifyType || status) {
    // Avoid compilation unused var warning error
  }
}

void QnnBackendManager::ProcessContextFromBinListAsync(Qnn_ContextHandle_t context, void* notifyParam) {
  std::lock_guard<std::mutex> guard(ep_context_handle_map_mutex_);
  if (!notifyParam) {
    LOGS(*logger_, WARNING) << "No known node names associated with context handle: " << context;
    return;
  }

  std::vector<std::string>* ep_node_names = reinterpret_cast<std::vector<std::string>*>(notifyParam);
  for (const auto& node_name : *ep_node_names) {
    if (!(ep_context_handle_map_.emplace(node_name, context).second)) {
      LOGS(*logger_, VERBOSE) << "Unable to map " << context << " to " << node_name;
    }
  }

  auto s = AddQnnContextHandle(context);
  if (s != Status::OK()) {
    LOGS(*logger_, WARNING) << "Unable to add context " << context;
  }
}

Status QnnBackendManager::CreateContextVtcmBackupBufferSharingEnabled(std::unordered_map<std::string, std::unique_ptr<std::vector<std::string>>>& context_bin_map) {
#if QNN_API_VERSION_MAJOR == 2 && (QNN_API_VERSION_MINOR >= 26)
  QnnContext_Config_t context_config_resource_sharing = QNN_CONTEXT_CONFIG_INIT;
  QnnHtpContext_CustomConfig_t resource_sharing_custom_config;
  resource_sharing_custom_config.option = QNN_HTP_CONTEXT_CONFIG_OPTION_SHARE_RESOURCES;
  resource_sharing_custom_config.shareResources = true;
  context_config_resource_sharing.option = QNN_CONTEXT_CONFIG_OPTION_CUSTOM;
  context_config_resource_sharing.customConfig = &resource_sharing_custom_config;

  QnnHtpContext_CustomConfig_t context_config_resource_sharing_opt_type;
  context_config_resource_sharing_opt_type.option = QNN_HTP_CONTEXT_CONFIG_OPTION_SHARE_RESOURCES_OPTIMIZATION_TYPE;
  context_config_resource_sharing_opt_type.shareResOptType = SEQUENTIAL_WITHOUT_VA_OPTIMIZATION;
  QnnContext_Config_t resource_sharing_opt_type_config;
  resource_sharing_opt_type_config.option = QNN_CONTEXT_CONFIG_OPTION_CUSTOM;
  resource_sharing_opt_type_config.customConfig = &context_config_resource_sharing_opt_type;

  QnnContext_Config_t context_config_weight_sharing = QNN_CONTEXT_CONFIG_INIT;
  QnnHtpContext_CustomConfig_t custom_config;
  custom_config.option = QNN_HTP_CONTEXT_CONFIG_OPTION_WEIGHT_SHARING_ENABLED;
  custom_config.weightSharingEnabled = true;
  context_config_weight_sharing.option = QNN_CONTEXT_CONFIG_OPTION_CUSTOM;
  context_config_weight_sharing.customConfig = &custom_config;
#else
  LOGS(*logger_, WARNING) << "Called CreateContextVtcmBackupBufferSharingEnabled() but QNN API version is older than 2.26!";
#endif
  QnnContext_Config_t context_priority_config = QNN_CONTEXT_CONFIG_INIT;
  ORT_RETURN_IF_ERROR(SetQnnContextConfig(context_priority_, context_priority_config));

  const QnnContext_Config_t* configs[] = {&context_priority_config,
#if QNN_API_VERSION_MAJOR == 2 && (QNN_API_VERSION_MINOR >= 26)
                                          &context_config_resource_sharing,
                                          &resource_sharing_opt_type_config,
                                          &context_config_weight_sharing,
#endif
                                          nullptr};

  std::vector<QnnContext_Params_t> context_params_list;
  std::vector<QnnContext_ParamsV1_t> context_paramsv1_list;
  std::vector<const QnnContext_Params_t*> context_params_ptr_list;
  std::vector<std::unique_ptr<char[]>> buffer_list;

  context_params_list.reserve(context_bin_map.size());
  context_params_ptr_list.reserve(context_bin_map.size() + 1);

  for (auto& it : context_bin_map) {
    auto context_bin_filepath = it.first;

    std::ifstream cache_file(context_bin_filepath.c_str(), std::ifstream::binary);
    ORT_RETURN_IF(!cache_file || !cache_file.good(), "Failed to retrieve context binary from: ", context_bin_filepath);

    cache_file.seekg(0, cache_file.end);
    size_t buffer_size = static_cast<size_t>(cache_file.tellg());
    ORT_RETURN_IF(0 == buffer_size, "Empty cache file encountered.");

    cache_file.seekg(0, cache_file.beg);
    std::unique_ptr<char[]> buffer = std::make_unique<char[]>(buffer_size);
    ORT_RETURN_IF(nullptr == buffer, "Failed to allocate memory for cache file.");
    const auto& read_result = cache_file.read(buffer.get(), buffer_size);
    ORT_RETURN_IF(!read_result, "Failed to read contents from cached context file.");

    cache_file.close();
    QnnContext_ParamsV1_t context_params_v1 = {nullptr,
                                               buffer.get(),
                                               buffer_size,
                                               nullptr,
                                               ContextCreateAsyncCallback,
                                               it.second.get()};

    QnnContext_Params_t context_params = {QnnContext_ParamsVersion_t::QNN_CONTEXT_PARAMS_VERSION_1,
                                          {context_params_v1}};

    buffer_list.push_back(std::move(buffer));
    context_params_list.push_back(std::move(context_params));
    context_paramsv1_list.push_back(std::move(context_params_v1));
    context_params_ptr_list.push_back(&context_params_list.back());
  }
  context_params_ptr_list.push_back(nullptr);
  auto result = qnn_interface_.contextCreateFromBinaryListAsync(backend_handle_,
                                                                device_handle_,
                                                                context_params_ptr_list.data(),
                                                                configs,
                                                                nullptr);

  context_params_ptr_list.clear();
  context_paramsv1_list.clear();
  context_params_list.clear();
  buffer_list.clear();

  ORT_RETURN_IF(QNN_CONTEXT_NO_ERROR != result, "Failed to create context. Error: ", QnnErrorHandleToString(result), ", Code:", result);
  return Status::OK();
}

Status QnnBackendManager::SetContextPriority(ContextPriority context_priority) {
  QnnContext_Config_t context_priority_config = QNN_CONTEXT_CONFIG_INIT;
  ORT_RETURN_IF_ERROR(SetQnnContextConfig(context_priority, context_priority_config));

  QnnContext_Config_t* configs[] = {&context_priority_config, nullptr};
  for (const auto& context_handle : contexts_) {
    auto result = qnn_interface_.contextSetConfig(context_handle, (const QnnContext_Config_t**)configs);
    ORT_RETURN_IF(QNN_CONTEXT_NO_ERROR != result, "Failed to set context priority for context handle: ", context_handle);
  }

  return Status::OK();
}

Status QnnBackendManager::ResetContextPriority() {
  return SetContextPriority(context_priority_);
}

Status QnnBackendManager::CreateContext(bool enable_htp_weight_sharing) {
  if (true == context_created_) {
    LOGS_DEFAULT(INFO) << "Context created already.";
    return Status::OK();
  }

  QnnContext_Config_t context_config_weight_sharing = QNN_CONTEXT_CONFIG_INIT;
  QnnHtpContext_CustomConfig_t custom_config;
  custom_config.option = QNN_HTP_CONTEXT_CONFIG_OPTION_WEIGHT_SHARING_ENABLED;
  custom_config.weightSharingEnabled = enable_htp_weight_sharing;
  context_config_weight_sharing.option = QNN_CONTEXT_CONFIG_OPTION_CUSTOM;
  context_config_weight_sharing.customConfig = &custom_config;

  QnnContext_Config_t context_priority_config = QNN_CONTEXT_CONFIG_INIT;
  ORT_RETURN_IF_ERROR(SetQnnContextConfig(context_priority_, context_priority_config));

  const QnnContext_Config_t* npu_context_configs[] = {&context_priority_config,
                                                      &context_config_weight_sharing,
                                                      nullptr};

  const QnnContext_Config_t* empty_context_configs[] = {nullptr};

  const QnnContext_Config_t** configs = nullptr;
  switch (GetQnnBackendType()) {
    case QnnBackendType::HTP:
    case QnnBackendType::DSP:
      configs = npu_context_configs;
      break;
    case QnnBackendType::GPU:
    case QnnBackendType::SERIALIZER:
      configs = nullptr;
      break;
    default:
      configs = empty_context_configs;
      break;
  }

  // Not all serialization backends allow for hardware configs to be applied.
  if (qnn_serializer_config_ && !qnn_serializer_config_->SupportsArbitraryGraphConfigs()) {
    configs = nullptr;
  }

  Qnn_ContextHandle_t context = nullptr;
  Qnn_ErrorHandle_t result = 0;

  result = qnn_interface_.contextCreate(backend_handle_,
                                        device_handle_,
                                        configs,
                                        &context);

  ORT_RETURN_IF(QNN_CONTEXT_NO_ERROR != result, "Failed to create context. Error: ", QnnErrorHandleToString(result), ", Code:", result);

  ORT_RETURN_IF_ERROR(AddQnnContextHandle(context));

  context_created_ = true;
  return Status::OK();
}

Status QnnBackendManager::ReleaseContext() {
  if (false == context_created_) {
    return Status::OK();
  }

  // release QNN context handles
  contexts_.clear();
  context_map_.clear();

  context_created_ = false;
  return Status::OK();
}

std::unique_ptr<unsigned char[]> QnnBackendManager::GetContextBinaryBuffer(uint64_t& written_buffer_size) {
  if (nullptr == qnn_interface_.contextGetBinarySize ||
      nullptr == qnn_interface_.contextGetBinary) {
    LOGS(*logger_, ERROR) << "Failed to get valid function pointer.";
    return nullptr;
  }
  ORT_ENFORCE(contexts_.size() > 0, "No valid QNN context!");
  uint64_t required_buffer_size(0);
  // Generate all graphs in one single context
  Qnn_ErrorHandle_t rt = qnn_interface_.contextGetBinarySize(contexts_[0], &required_buffer_size);
  if (QNN_CONTEXT_NO_ERROR != rt) {
    LOGS(*logger_, ERROR) << "Failed to get QNN context binary size. Error: " << QnnErrorHandleToString(rt);
    return nullptr;
  }

  std::unique_ptr<unsigned char[]> context_buffer = std::make_unique<unsigned char[]>(required_buffer_size);
  if (nullptr == context_buffer) {
    LOGS(*logger_, ERROR) << "Failed to allocate buffer for context cache.";
    return nullptr;
  }

  rt = qnn_interface_.contextGetBinary(contexts_[0],
                                       reinterpret_cast<void*>(context_buffer.get()),
                                       required_buffer_size,
                                       &written_buffer_size);
  if (QNN_CONTEXT_NO_ERROR != rt) {
    LOGS(*logger_, ERROR) << "Failed to get context binary. Error: " << QnnErrorHandleToString(rt);
    return nullptr;
  }

  if (required_buffer_size < written_buffer_size) {
    LOGS(*logger_, ERROR) << "Context written buffer size: " << written_buffer_size
                          << " exceeds allocated buffer size: " << required_buffer_size;
    return nullptr;
  }

  LOGS(*logger_, VERBOSE) << "Get context binary buffer succeed.";
  return context_buffer;
}

Status QnnBackendManager::GetMaxSpillFillBufferSize(unsigned char* buffer,
                                                    uint64_t buffer_length,
                                                    uint64_t& max_spill_fill_buffer_size) {
  max_spill_fill_buffer_size = 0;
  // spill fill starts from 2.28
#if QNN_API_VERSION_MAJOR == 2 && (QNN_API_VERSION_MINOR >= 21)
  bool result = nullptr == qnn_sys_interface_.systemContextCreate ||
                nullptr == qnn_sys_interface_.systemContextGetBinaryInfo ||
                nullptr == qnn_sys_interface_.systemContextFree;
  ORT_RETURN_IF(result, "Failed to get valid function pointer.");

  QnnSystemContext_Handle_t sys_ctx_handle = nullptr;
  auto rt = qnn_sys_interface_.systemContextCreate(&sys_ctx_handle);
  ORT_RETURN_IF(QNN_SUCCESS != rt, "Failed to create system handle.");

  const QnnSystemContext_BinaryInfo_t* binary_info = nullptr;
  Qnn_ContextBinarySize_t binary_info_size{0};
  rt = qnn_sys_interface_.systemContextGetBinaryInfo(sys_ctx_handle,
                                                     static_cast<void*>(buffer),
                                                     buffer_length,
                                                     &binary_info,
                                                     &binary_info_size);
  ORT_RETURN_IF(QNN_SUCCESS != rt, "Failed to get context binary info.");

  // binary_info life cycle is here
  // Binary info to graph info
  // retrieve Qnn graph info from binary info
  ORT_RETURN_IF(nullptr == binary_info, "Qnn cached binary info is nullptr.");
  uint32_t graph_count = 0;
  QnnSystemContext_GraphInfo_t* graphs_info = nullptr;
  if (binary_info->version == QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_3) {
    graph_count = binary_info->contextBinaryInfoV3.numGraphs;
    graphs_info = binary_info->contextBinaryInfoV3.graphs;
  } else if (binary_info->version == QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_2) {
    graph_count = binary_info->contextBinaryInfoV2.numGraphs;
    graphs_info = binary_info->contextBinaryInfoV2.graphs;
  } else if (binary_info->version == QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_1) {
    graph_count = binary_info->contextBinaryInfoV1.numGraphs;
    graphs_info = binary_info->contextBinaryInfoV1.graphs;
  } else {
    return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Unsupported context binary info version.");
  }

  for (uint32_t i = 0; i < graph_count; ++i) {
    if (graphs_info[i].version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_3) {
      auto htp_graph_info = reinterpret_cast<QnnHtpSystemContext_GraphBlobInfo_t*>(graphs_info[i].graphInfoV3.graphBlobInfo);
      if (htp_graph_info->version == QNN_SYSTEM_CONTEXT_HTP_GRAPH_INFO_BLOB_VERSION_V1) {
        auto spill_fill_buffer_size = htp_graph_info->contextBinaryGraphBlobInfoV1.spillFillBufferSize;
        max_spill_fill_buffer_size = spill_fill_buffer_size > max_spill_fill_buffer_size ? spill_fill_buffer_size : max_spill_fill_buffer_size;
      } else {
        LOGS(*logger_, VERBOSE) << "Unknown context binary graph info blob version.";
      }
    } else if (graphs_info[i].version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_2 ||
               graphs_info[i].version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_1) {
      LOGS(*logger_, VERBOSE) << "Skip retrieve spill file buffer size, it is not supported with graph info v1 & v2.";
    } else {
      LOGS(*logger_, VERBOSE) << "Unknown context binary graph info version.";
    }
  }
#else
  ORT_UNUSED_PARAMETER(buffer);
  ORT_UNUSED_PARAMETER(buffer_length);
#endif

  LOGS(*logger_, VERBOSE) << "Get max spill fill buffer size completed.";
  return Status::OK();
}

Status QnnBackendManager::LoadCachedQnnContextFromBuffer(char* buffer, uint64_t buffer_length,
                                                         std::string node_name,
                                                         QnnModelLookupTable& qnn_models,
                                                         int64_t max_spill_fill_size) {
  bool result = nullptr == qnn_sys_interface_.systemContextCreate ||
                nullptr == qnn_sys_interface_.systemContextGetBinaryInfo ||
                nullptr == qnn_sys_interface_.systemContextFree;
  ORT_RETURN_IF(result, "Failed to get valid function pointer.");

  QnnSystemContext_Handle_t sys_ctx_handle = nullptr;
  auto rt = qnn_sys_interface_.systemContextCreate(&sys_ctx_handle);
  ORT_RETURN_IF(QNN_SUCCESS != rt, "Failed to create system handle.");

  const QnnSystemContext_BinaryInfo_t* binary_info = nullptr;
  Qnn_ContextBinarySize_t binary_info_size{0};
  rt = qnn_sys_interface_.systemContextGetBinaryInfo(sys_ctx_handle,
                                                     static_cast<void*>(buffer),
                                                     buffer_length,
                                                     &binary_info,
                                                     &binary_info_size);
  ORT_RETURN_IF(QNN_SUCCESS != rt, "Failed to get context binary info.");

  // binary_info life cycle is here
  // Binary info to graph info
  // retrieve Qnn graph info from binary info
  ORT_RETURN_IF(nullptr == binary_info, "Qnn cached binary info is nullptr.");
  uint32_t graph_count = 0;
  QnnSystemContext_GraphInfo_t* graphs_info = nullptr;
  if (binary_info->version == QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_1) {
    graph_count = binary_info->contextBinaryInfoV1.numGraphs;
    graphs_info = binary_info->contextBinaryInfoV1.graphs;
  }
#if QNN_API_VERSION_MAJOR == 2 && (QNN_API_VERSION_MINOR >= 15)  // starts from 2.22
  else if (binary_info->version == QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_2) {
    graph_count = binary_info->contextBinaryInfoV2.numGraphs;
    graphs_info = binary_info->contextBinaryInfoV2.graphs;
  }
#endif
#if QNN_API_VERSION_MAJOR == 2 && (QNN_API_VERSION_MINOR >= 21)  // starts from 2.28
  else if (binary_info->version == QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_3) {
    graph_count = binary_info->contextBinaryInfoV3.numGraphs;
    graphs_info = binary_info->contextBinaryInfoV3.graphs;
  }
#endif
  else {
    return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Unsupported context binary info version.");
  }

  ORT_RETURN_IF(graph_count < 1 || graphs_info == nullptr, "Failed to get graph info from Qnn cached context.");
  LOGS(*logger_, VERBOSE) << "Graph count from QNN context: " << graph_count;

  Qnn_ContextHandle_t context = nullptr;
#if QNN_API_VERSION_MAJOR == 2 && (QNN_API_VERSION_MINOR >= 26)
  if (vtcm_backup_buffer_sharing_enabled_) {
    if (ep_context_handle_map_.find(node_name) != ep_context_handle_map_.end()) {
      context = ep_context_handle_map_.at(node_name);
    }
    ORT_RETURN_IF(nullptr == context, "Failed to retrieve context for ", node_name);

  } else {
#endif
    QnnContext_Config_t qnn_context_config = QNN_CONTEXT_CONFIG_INIT;
    ORT_RETURN_IF_ERROR(SetQnnContextConfig(context_priority_, qnn_context_config));

    // Register spill fill buffer for multi context
    QnnContext_Config_t spill_fill_config = QNN_CONTEXT_CONFIG_INIT;

    // The spill fill buffer is available since 2.28, API version starts from 2.21
#if QNN_API_VERSION_MAJOR == 2 && (QNN_API_VERSION_MINOR >= 21)
    QnnHtpContext_CustomConfig_t custom_config;
    custom_config.option = QNN_HTP_CONTEXT_CONFIG_OPTION_REGISTER_MULTI_CONTEXTS;
    QnnHtpContext_GroupRegistration_t group_info;
    size_t current_contexts_size = GetQnnContextSize();
    // set to 0x0 (new group) if this is the first context, otherwise point to the first context handle
    // note that we already move the context with max spill fill size to the beginning of the list
    group_info.firstGroupHandle = (max_spill_fill_size > 0 && current_contexts_size > 0) ? GetQnnContext(0) : 0x0;
    group_info.maxSpillFillBuffer = max_spill_fill_size;  // Max spill-fill buffer across contexts. Must be >0
    custom_config.groupRegistration = group_info;
    spill_fill_config.option = QNN_CONTEXT_CONFIG_OPTION_CUSTOM;
    spill_fill_config.customConfig = &custom_config;

#endif

    QnnContext_Config_t* spill_fill_config_pointer = max_spill_fill_size > 0 ? &spill_fill_config : nullptr;
    LOGS(*logger_, VERBOSE) << "Max spill fill buffer size:" << max_spill_fill_size;

    const QnnContext_Config_t* context_configs[] = {&qnn_context_config, spill_fill_config_pointer, nullptr};

    ORT_RETURN_IF(nullptr == qnn_interface_.contextCreateFromBinary,
                  "Invalid function pointer for contextCreateFromBinary.");

    rt = qnn_interface_.contextCreateFromBinary(backend_handle_,
                                                device_handle_,
                                                context_configs,
                                                static_cast<void*>(buffer),
                                                buffer_length,
                                                &context,
                                                profile_backend_handle_);
    ORT_RETURN_IF(QNN_SUCCESS != rt, "Failed to create context from binary. Error code: ", rt);
    ORT_RETURN_IF_ERROR(AddQnnContextHandle(context));

#if QNN_API_VERSION_MAJOR == 2 && (QNN_API_VERSION_MINOR >= 26)
  }
#endif

  if (1 == graph_count) {
    // in case the EPContext node is generated from script
    // the graph name from the context binary may not match the EPContext node name
    auto qnn_model = std::make_unique<qnn::QnnModel>(this);
    ORT_RETURN_IF_ERROR(qnn_model->DeserializeGraphInfoFromBinaryInfo(graphs_info[0], context));
    qnn_models.emplace(node_name, std::move(qnn_model));
  } else {
    for (uint32_t i = 0; i < graph_count; ++i) {
      auto qnn_model = std::make_unique<qnn::QnnModel>(this);
      ORT_RETURN_IF_ERROR(qnn_model->DeserializeGraphInfoFromBinaryInfo(graphs_info[i], context));
      qnn_models.emplace(graphs_info[i].graphInfoV1.graphName, std::move(qnn_model));
    }
  }

  qnn_sys_interface_.systemContextFree(sys_ctx_handle);
  sys_ctx_handle = nullptr;

  ORT_RETURN_IF_ERROR(ExtractBackendProfilingInfo());
  context_created_ = true;

  LOGS(*logger_, VERBOSE) << "Load from cached QNN Context completed.";
  return Status::OK();
}

// need to load system lib if load from Qnn context binary
// or generate Qnn context binary is enabled -- to get the max spill fill buffer size
Status QnnBackendManager::SetupBackend(const logging::Logger& logger,
                                       bool load_from_cached_context,
                                       bool need_load_system_lib,
                                       bool share_ep_contexts,
                                       bool enable_vtcm_backup_buffer_sharing,
                                       std::unordered_map<std::string, std::unique_ptr<std::vector<std::string>>>& context_bin_map) {
  std::lock_guard<std::recursive_mutex> lock(logger_recursive_mutex_);
  if (backend_setup_completed_) {
    LOGS(logger, VERBOSE) << "Backend setup already!";

#if QNN_API_VERSION_MAJOR == 2 && (QNN_API_VERSION_MINOR >= 26)
    if (vtcm_backup_buffer_sharing_enabled_) {
      // If a context bin filepath has not been processed yet,
      // then a new context must be created for the set of context bins
      auto first_mapping_it = ep_context_handle_map_.find(context_bin_map.begin()->first);
      if (first_mapping_it == ep_context_handle_map_.end()) {
        LOGS(logger, VERBOSE) << "Creating context for new set of context binaries";
        return CreateContextVtcmBackupBufferSharingEnabled(context_bin_map);
      }

      LOGS(logger, VERBOSE) << "Mapping contexts to new EP main context nodes";

      for (auto& it : context_bin_map) {
        auto context_bin_filepath = it.first;
        auto ep_node_names = *(it.second);

        auto context = ep_context_handle_map_.at(context_bin_filepath);
        for (auto node_name : ep_node_names) {
          ep_context_handle_map_.emplace(node_name, context);
        }
      }
    }
#endif
    return Status::OK();
  }

  vtcm_backup_buffer_sharing_enabled_ = enable_vtcm_backup_buffer_sharing;

  Status status = Status::OK();
  if (!qnn_serializer_config_) {
    status = LoadBackend();
  } else {
    status = LoadQnnSerializerBackend();
  }
  if (status.IsOK()) {
    LOGS(logger, VERBOSE) << "LoadBackend succeed.";
  }

  if (status.IsOK() && (load_from_cached_context || need_load_system_lib)) {
    status = LoadQnnSystemLib();
  }

  if (status.IsOK()) {
    sdk_build_version_ = GetBackendBuildId();
    LOGS(logger, VERBOSE) << "Backend build version: "
                          << sdk_build_version_;
  }

  if (status.IsOK()) {
    status = InitializeQnnLog(logger);
  }
  if (status.IsOK()) {
    LOGS(logger, VERBOSE) << "SetLogger succeed.";
  }

  if (status.IsOK()) {
    status = InitializeBackend();
  }
  if (status.IsOK()) {
    LOGS(logger, VERBOSE) << "InitializeBackend succeed.";
  }

  if (status.IsOK()) {
    status = CreateDevice();
  }
  if (status.IsOK()) {
    LOGS(logger, VERBOSE) << "CreateDevice succeed.";
  }

  if (status.IsOK()) {
    status = InitializeProfiling();
  }
  if (status.IsOK()) {
    LOGS(logger, VERBOSE) << "InitializeProfiling succeed.";
  }

  if (status.IsOK()) {
    ORT_RETURN_IF_ERROR(LoadOpPackage());
    LOGS(logger, VERBOSE) << "LoadOpPackage succeed.";
  }

  bool enable_htp_weight_sharing = false;
  if (share_ep_contexts && !load_from_cached_context) {
#if defined(__aarch64__) || defined(_M_ARM64)
    LOGS(logger, WARNING) << "Weight sharing only available with offline generation on x64 platform, not work on real device.";
#else
    enable_htp_weight_sharing = true;
#endif
  }

  if (status.IsOK() && (vtcm_backup_buffer_sharing_enabled_ || !load_from_cached_context)) {
    status = vtcm_backup_buffer_sharing_enabled_ ? CreateContextVtcmBackupBufferSharingEnabled(context_bin_map)
                                                 : CreateContext(enable_htp_weight_sharing);

    if (status.IsOK()) {
      LOGS(logger, VERBOSE) << "CreateContext succeed.";
    }
  }

  if (status.IsOK()) {
    LOGS(logger, VERBOSE) << "QNN SetupBackend succeed";
    backend_setup_completed_ = true;
  } else {
    LOGS_DEFAULT(WARNING) << "Failed to setup so cleaning up";
    ReleaseResources();
  }

  return status;
}

Status QnnBackendManager::CreateHtpPowerCfgId(uint32_t device_id, uint32_t core_id, uint32_t& htp_power_config_id) {
  // This function is called in QNN EP's OnRunStart() even if QNN backend setup failed and the model is assigned
  // to a different EP. Therefore, we have to check that backend setup actually completed before trying to
  // create an HTP power config ID. Otherwise, this causes a segfault because the QNN backend lib is unloaded.
  ORT_RETURN_IF_NOT(backend_setup_completed_, "Cannot create HTP power config ID if backend setup is not complete.");
  QnnDevice_Infrastructure_t qnn_device_infra = nullptr;
  auto status = qnn_interface_.deviceGetInfrastructure(&qnn_device_infra);
  ORT_RETURN_IF(QNN_SUCCESS != status, "backendGetPerfInfrastructure failed.");

  auto* htp_infra = static_cast<QnnHtpDevice_Infrastructure_t*>(qnn_device_infra);
  ORT_RETURN_IF(QNN_HTP_DEVICE_INFRASTRUCTURE_TYPE_PERF != htp_infra->infraType,
                "HTP infra type = ", htp_infra->infraType, ", which is not perf infra type.");
  QnnHtpDevice_PerfInfrastructure_t& htp_perf_infra = htp_infra->perfInfra;
  // Get power client id
  status = htp_perf_infra.createPowerConfigId(device_id, core_id, &htp_power_config_id);
  ORT_RETURN_IF(QNN_SUCCESS != status, "createPowerConfigId failed.");

  return Status::OK();
}

Status QnnBackendManager::SetHtpPowerConfig(uint32_t htp_power_config_client_id,
                                            HtpPerformanceMode htp_performance_mode) {
  // This function is called in QNN EP's OnRunStart() even if QNN backend setup failed and the model is assigned
  // to a different EP. Therefore, we have to check that backend setup actually completed before trying to
  // set an HTP power config ID. Otherwise, this causes a segfault because the QNN backend lib is unloaded.
  ORT_RETURN_IF_NOT(backend_setup_completed_, "Cannot set HTP power config ID if backend setup is not complete.");
  QnnDevice_Infrastructure_t qnn_device_infra = nullptr;
  auto status = qnn_interface_.deviceGetInfrastructure(&qnn_device_infra);
  ORT_RETURN_IF(QNN_SUCCESS != status, "backendGetPerfInfrastructure failed.");

  auto* htp_infra = static_cast<QnnHtpDevice_Infrastructure_t*>(qnn_device_infra);
  ORT_RETURN_IF(QNN_HTP_DEVICE_INFRASTRUCTURE_TYPE_PERF != htp_infra->infraType,
                "HTP infra type = ", htp_infra->infraType, ", which is not perf infra type.");
  QnnHtpDevice_PerfInfrastructure_t& htp_perf_infra = htp_infra->perfInfra;

  constexpr const int kNumConfigs = 1;
  std::vector<QnnHtpPerfInfrastructure_PowerConfig_t> power_configs(
      kNumConfigs);
  QnnHtpPerfInfrastructure_PowerConfig_t& dcvs_config = power_configs[0];
  dcvs_config.option = QNN_HTP_PERF_INFRASTRUCTURE_POWER_CONFIGOPTION_DCVS_V3;
  QnnHtpPerfInfrastructure_DcvsV3_t& dcvs_v3 = dcvs_config.dcvsV3Config;
  dcvs_v3.contextId = htp_power_config_client_id;
  dcvs_v3.setSleepDisable = 0;
  dcvs_v3.sleepDisable = 0;
  dcvs_v3.setDcvsEnable = 1;
  dcvs_v3.powerMode = QNN_HTP_PERF_INFRASTRUCTURE_POWERMODE_PERFORMANCE_MODE;
  // choose performance mode
  switch (htp_performance_mode) {
    case HtpPerformanceMode::kHtpBurst:
      dcvs_v3.setSleepLatency = 1;  // true
      dcvs_v3.sleepLatency = kSleepMinLatency;
      dcvs_v3.dcvsEnable = kDcvsDisable;
      dcvs_v3.setBusParams = 1;
      dcvs_v3.busVoltageCornerMin = DCVS_VOLTAGE_VCORNER_MAX_VOLTAGE_CORNER;
      dcvs_v3.busVoltageCornerTarget = DCVS_VOLTAGE_VCORNER_MAX_VOLTAGE_CORNER;
      dcvs_v3.busVoltageCornerMax = DCVS_VOLTAGE_VCORNER_MAX_VOLTAGE_CORNER;
      dcvs_v3.setCoreParams = 1;
      dcvs_v3.coreVoltageCornerMin = DCVS_VOLTAGE_VCORNER_MAX_VOLTAGE_CORNER;
      dcvs_v3.coreVoltageCornerTarget = DCVS_VOLTAGE_VCORNER_MAX_VOLTAGE_CORNER;
      dcvs_v3.coreVoltageCornerMax = DCVS_VOLTAGE_VCORNER_MAX_VOLTAGE_CORNER;
      break;
    case HtpPerformanceMode::kHtpSustainedHighPerformance:
    case HtpPerformanceMode::kHtpHighPerformance:
      dcvs_v3.setSleepLatency = 1;  // true
      dcvs_v3.sleepLatency = kSleepLowLatency;
      dcvs_v3.dcvsEnable = kDcvsDisable;
      dcvs_v3.setBusParams = 1;
      dcvs_v3.busVoltageCornerMin = DCVS_VOLTAGE_VCORNER_TURBO;
      dcvs_v3.busVoltageCornerTarget = DCVS_VOLTAGE_VCORNER_TURBO;
      dcvs_v3.busVoltageCornerMax = DCVS_VOLTAGE_VCORNER_TURBO;
      dcvs_v3.setCoreParams = 1;
      dcvs_v3.coreVoltageCornerMin = DCVS_VOLTAGE_VCORNER_TURBO;
      dcvs_v3.coreVoltageCornerTarget = DCVS_VOLTAGE_VCORNER_TURBO;
      dcvs_v3.coreVoltageCornerMax = DCVS_VOLTAGE_VCORNER_TURBO;
      break;
    case HtpPerformanceMode::kHtpBalanced:
      dcvs_v3.setSleepLatency = 1;  // true
      dcvs_v3.sleepLatency = kSleepMediumLatency;
      dcvs_v3.dcvsEnable = kDcvsEnable;
      dcvs_v3.setBusParams = 1;
      dcvs_v3.busVoltageCornerMin = DCVS_VOLTAGE_VCORNER_NOM_PLUS;
      dcvs_v3.busVoltageCornerTarget = DCVS_VOLTAGE_VCORNER_NOM_PLUS;
      dcvs_v3.busVoltageCornerMax = DCVS_VOLTAGE_VCORNER_NOM_PLUS;
      dcvs_v3.setCoreParams = 1;
      dcvs_v3.coreVoltageCornerMin = DCVS_VOLTAGE_VCORNER_NOM_PLUS;
      dcvs_v3.coreVoltageCornerTarget = DCVS_VOLTAGE_VCORNER_NOM_PLUS;
      dcvs_v3.coreVoltageCornerMax = DCVS_VOLTAGE_VCORNER_NOM_PLUS;
      break;
    case HtpPerformanceMode::kHtpLowBalanced:
      dcvs_v3.setSleepLatency = 1;  // true
      dcvs_v3.sleepLatency = kSleepMediumLatency;
      dcvs_v3.dcvsEnable = kDcvsEnable;
      dcvs_v3.setBusParams = 1;
      dcvs_v3.busVoltageCornerMin = DCVS_VOLTAGE_VCORNER_NOM;
      dcvs_v3.busVoltageCornerTarget = DCVS_VOLTAGE_VCORNER_NOM;
      dcvs_v3.busVoltageCornerMax = DCVS_VOLTAGE_VCORNER_NOM;
      dcvs_v3.setCoreParams = 1;
      dcvs_v3.coreVoltageCornerMin = DCVS_VOLTAGE_VCORNER_NOM;
      dcvs_v3.coreVoltageCornerTarget = DCVS_VOLTAGE_VCORNER_NOM;
      dcvs_v3.coreVoltageCornerMax = DCVS_VOLTAGE_VCORNER_NOM;
      break;
    case HtpPerformanceMode::kHtpHighPowerSaver:
      dcvs_v3.setSleepLatency = 1;  // true
      dcvs_v3.sleepLatency = kSleepMediumLatency;
      dcvs_v3.dcvsEnable = kDcvsEnable;
      dcvs_v3.setBusParams = 1;
      dcvs_v3.busVoltageCornerMin = DCVS_VOLTAGE_VCORNER_SVS_PLUS;
      dcvs_v3.busVoltageCornerTarget = DCVS_VOLTAGE_VCORNER_SVS_PLUS;
      dcvs_v3.busVoltageCornerMax = DCVS_VOLTAGE_VCORNER_SVS_PLUS;
      dcvs_v3.setCoreParams = 1;
      dcvs_v3.coreVoltageCornerMin = DCVS_VOLTAGE_VCORNER_SVS_PLUS;
      dcvs_v3.coreVoltageCornerTarget = DCVS_VOLTAGE_VCORNER_SVS_PLUS;
      dcvs_v3.coreVoltageCornerMax = DCVS_VOLTAGE_VCORNER_SVS_PLUS;
      break;
    case HtpPerformanceMode::kHtpPowerSaver:
      dcvs_v3.setSleepLatency = 1;  // true
      dcvs_v3.sleepLatency = kSleepMediumLatency;
      dcvs_v3.dcvsEnable = kDcvsEnable;
      dcvs_v3.setBusParams = 1;
      dcvs_v3.busVoltageCornerMin = DCVS_VOLTAGE_VCORNER_SVS;
      dcvs_v3.busVoltageCornerTarget = DCVS_VOLTAGE_VCORNER_SVS;
      dcvs_v3.busVoltageCornerMax = DCVS_VOLTAGE_VCORNER_SVS;
      dcvs_v3.setCoreParams = 1;
      dcvs_v3.coreVoltageCornerMin = DCVS_VOLTAGE_VCORNER_SVS;
      dcvs_v3.coreVoltageCornerTarget = DCVS_VOLTAGE_VCORNER_SVS;
      dcvs_v3.coreVoltageCornerMax = DCVS_VOLTAGE_VCORNER_SVS;
      break;
    case HtpPerformanceMode::kHtpLowPowerSaver:
      dcvs_v3.setSleepLatency = 1;  // true
      dcvs_v3.sleepLatency = kSleepMediumLatency;
      dcvs_v3.dcvsEnable = kDcvsEnable;
      dcvs_v3.setBusParams = 1;
      dcvs_v3.busVoltageCornerMin = DCVS_VOLTAGE_VCORNER_SVS2;
      dcvs_v3.busVoltageCornerTarget = DCVS_VOLTAGE_VCORNER_SVS2;
      dcvs_v3.busVoltageCornerMax = DCVS_VOLTAGE_VCORNER_SVS2;
      dcvs_v3.setCoreParams = 1;
      dcvs_v3.coreVoltageCornerMin = DCVS_VOLTAGE_VCORNER_SVS2;
      dcvs_v3.coreVoltageCornerTarget = DCVS_VOLTAGE_VCORNER_SVS2;
      dcvs_v3.coreVoltageCornerMax = DCVS_VOLTAGE_VCORNER_SVS2;
      break;
    case HtpPerformanceMode::kHtpExtremePowerSaver:
      dcvs_v3.powerMode = QNN_HTP_PERF_INFRASTRUCTURE_POWERMODE_POWER_SAVER_MODE;
      dcvs_v3.setSleepLatency = 1;  // true
      dcvs_v3.sleepLatency = kSleepMediumLatency;
      dcvs_v3.dcvsEnable = kDcvsEnable;
      dcvs_v3.setBusParams = 1;
      dcvs_v3.busVoltageCornerMin = DCVS_VOLTAGE_CORNER_DISABLE;
      dcvs_v3.busVoltageCornerTarget = DCVS_VOLTAGE_CORNER_DISABLE;
      dcvs_v3.busVoltageCornerMax = DCVS_VOLTAGE_CORNER_DISABLE;
      dcvs_v3.setCoreParams = 1;
      dcvs_v3.coreVoltageCornerMin = DCVS_VOLTAGE_CORNER_DISABLE;
      dcvs_v3.coreVoltageCornerTarget = DCVS_VOLTAGE_CORNER_DISABLE;
      dcvs_v3.coreVoltageCornerMax = DCVS_VOLTAGE_CORNER_DISABLE;
      break;
    default:
      ORT_THROW("Invalid performance profile %d", static_cast<int>(htp_performance_mode));
      break;
  }
  std::vector<const QnnHtpPerfInfrastructure_PowerConfig_t*> perf_power_configs_ptr = ObtainNullTermPtrVector(power_configs);
  status = htp_perf_infra.setPowerConfig(htp_power_config_client_id, perf_power_configs_ptr.data());
  ORT_RETURN_IF(QNN_SUCCESS != status, "setPowerConfig failed for HTP performance mode.");

  return Status::OK();
}

Status QnnBackendManager::SetRpcPowerConfigs(uint32_t htp_power_config_client_id,
                                             uint32_t rpc_control_latency,
                                             uint32_t rpc_polling_time) {
  // This function is called in QNN EP's OnRunStart() even if QNN backend setup failed and the model is assigned
  // to a different EP. Therefore, we have to check that backend setup actually completed before trying to
  // set RPC control latency. Otherwise, this causes a segfault because the QNN backend library is unloaded.
  ORT_RETURN_IF_NOT(backend_setup_completed_, "Cannot set HTP RPC control latency if backend setup is not complete.");

  constexpr int kNumRpcPollingPowerConfigs = 2;
  std::vector<QnnHtpPerfInfrastructure_PowerConfig_t> rpc_power_configs;
  rpc_power_configs.reserve(kNumRpcPollingPowerConfigs);

  // Set rpc control latency here
  if (rpc_control_latency != 0) {
    auto& rpc_control_latency_cfg = rpc_power_configs.emplace_back();
    rpc_control_latency_cfg.option = QNN_HTP_PERF_INFRASTRUCTURE_POWER_CONFIGOPTION_RPC_CONTROL_LATENCY;
    rpc_control_latency_cfg.rpcControlLatencyConfig = rpc_control_latency;
  }

  // Note: v68 does not support rpc polling mode
  if (rpc_polling_time != 0) {
    auto& rpc_polling_time_cfg = rpc_power_configs.emplace_back();
    rpc_polling_time_cfg.option = QNN_HTP_PERF_INFRASTRUCTURE_POWER_CONFIGOPTION_RPC_POLLING_TIME;
    rpc_polling_time_cfg.rpcPollingTimeConfig = rpc_polling_time;
  }

  if (rpc_power_configs.size() > 0) {
    QnnDevice_Infrastructure_t qnn_device_infra = nullptr;
    auto status = qnn_interface_.deviceGetInfrastructure(&qnn_device_infra);
    ORT_RETURN_IF(QNN_SUCCESS != status, "backendGetPerfInfrastructure failed.");

    auto* htp_infra = static_cast<QnnHtpDevice_Infrastructure_t*>(qnn_device_infra);
    ORT_RETURN_IF(QNN_HTP_DEVICE_INFRASTRUCTURE_TYPE_PERF != htp_infra->infraType,
                  "HTP infra type = ", htp_infra->infraType, ", which is not perf infra type.");
    QnnHtpDevice_PerfInfrastructure_t& htp_perf_infra = htp_infra->perfInfra;

    std::vector<const QnnHtpPerfInfrastructure_PowerConfig_t*> perf_power_configs_ptr =
        ObtainNullTermPtrVector(rpc_power_configs);
    status = htp_perf_infra.setPowerConfig(htp_power_config_client_id, perf_power_configs_ptr.data());
    ORT_RETURN_IF(QNN_SUCCESS != status, "setPowerConfig failed for RPC control latency.");
  }

  return Status::OK();
}

Status QnnBackendManager::DestroyHTPPowerConfigID(uint32_t htp_power_config_id) {
  QnnDevice_Infrastructure_t qnn_device_infra = nullptr;
  auto status = qnn_interface_.deviceGetInfrastructure(&qnn_device_infra);
  ORT_RETURN_IF(QNN_SUCCESS != status, "backendGetPerfInfrastructure failed.");

  auto* htp_infra = static_cast<QnnHtpDevice_Infrastructure_t*>(qnn_device_infra);
  ORT_RETURN_IF(QNN_HTP_DEVICE_INFRASTRUCTURE_TYPE_PERF != htp_infra->infraType,
                "HTP infra type = ", htp_infra->infraType, ", which is not perf infra type.");
  QnnHtpDevice_PerfInfrastructure_t& htp_perf_infra = htp_infra->perfInfra;

  Qnn_ErrorHandle_t destroy_ret = htp_perf_infra.destroyPowerConfigId(htp_power_config_id);
  ORT_RETURN_IF(QNN_SUCCESS != destroy_ret, "destroyPowerConfigId failed.");
  return Status::OK();
}

Status QnnBackendManager::TerminateQnnLog() {
  std::lock_guard<std::recursive_mutex> lock(logger_recursive_mutex_);
  if (logger_ == nullptr) {
    return Status::OK();
  }

  if (nullptr != qnn_interface_.logFree && nullptr != log_handle_) {
    auto ret_val = qnn_interface_.logFree(log_handle_);

    // Reset QNN log handle to nullptr so other threads that are waiting on logger_recursive_mutex_ know it was freed.
    log_handle_ = nullptr;
    ORT_RETURN_IF(QNN_SUCCESS != ret_val,
                  "Unable to terminate logging in the backend.");
  }

  return Status::OK();
}

void QnnBackendManager::ReleaseResources() {
  auto result = ReleaseContext();
  if (Status::OK() != result) {
    LOGS_DEFAULT(ERROR) << "Failed to ReleaseContext: " << result.ErrorMessage();
  }

  result = ReleaseProfilehandle();
  if (Status::OK() != result) {
    LOGS_DEFAULT(ERROR) << "Failed to ReleaseProfilehandle: " << result.ErrorMessage();
  }

  result = ReleaseDevice();
  if (Status::OK() != result) {
    LOGS_DEFAULT(ERROR) << "Failed to ReleaseDevice: " << result.ErrorMessage();
  }

  result = ShutdownBackend();
  if (Status::OK() != result) {
    LOGS_DEFAULT(ERROR) << "Failed to ShutdownBackend: " << result.ErrorMessage();
  }

  result = TerminateQnnLog();
  if (Status::OK() != result) {
    LOGS_DEFAULT(ERROR) << "Failed to TerminateQnnLog: " << result.ErrorMessage();
  }

  if (backend_lib_handle_) {
    result = UnloadLib(backend_lib_handle_);
    if (Status::OK() != result) {
      LOGS_DEFAULT(ERROR) << "Failed to unload backend library: " << result.ErrorMessage();
    }
  }

  backend_setup_completed_ = false;

  return;
}

Status QnnBackendManager::ExtractBackendProfilingInfo() {
  if (ProfilingLevel::OFF == profiling_level_merge_ || ProfilingLevel::INVALID == profiling_level_merge_) {
    return Status::OK();
  }

  bool tracelogging_provider_ep_enabled = false;
#ifdef _WIN32
  auto& provider = QnnTelemetry::Instance();
  if (provider.IsEnabled()) {
    auto level = provider.Level();
    auto keyword = provider.Keyword();
    if ((keyword & static_cast<uint64_t>(onnxruntime::logging::ORTTraceLoggingKeyword::Profiling)) != 0 && level >= 5) {
      tracelogging_provider_ep_enabled = true;
    }
  }
#endif  // defined(_WIN32)

  // ETW disabled previously, but enabled now
  if (ProfilingLevel::INVALID == profiling_level_etw_ && tracelogging_provider_ep_enabled) {
    LOGS(*logger_, ERROR) << "ETW disabled previously, but enabled now. Can't do the switch! Won't output any profiling.";
    return Status::OK();
  }

  // ETW enabled previously, but disabled now
  if (ProfilingLevel::INVALID != profiling_level_etw_ && !tracelogging_provider_ep_enabled) {
    LOGS(*logger_, ERROR) << "ETW enabled previously, but disabled now. Can't do the switch! Won't output any profiling.";
    return Status::OK();
  }

  ORT_RETURN_IF(!tracelogging_provider_ep_enabled && profiling_file_path_.empty(),
                "Need to specify a CSV file via provider option profiling_file_path if ETW not enabled.");

  ORT_RETURN_IF(nullptr == profile_backend_handle_, "Backend profile handle not valid.");

  const QnnProfile_EventId_t* profile_events{nullptr};
  uint32_t num_events{0};
  Qnn_ErrorHandle_t result = qnn_interface_.profileGetEvents(profile_backend_handle_, &profile_events, &num_events);
  if (qnn_serializer_config_) {  // Using QNN Saver or IR backend
    // QNN SDK 2.28.2 returns QNN_SAVER_ERROR_DUMMY_RETVALUE, but previous QNN versions return QNN_PROFILE_NO_ERROR.
    // We accept both values.
    ORT_RETURN_IF(QNN_PROFILE_NO_ERROR != result && QNN_SAVER_ERROR_DUMMY_RETVALUE != result,
                  "Failed to get profile events. Error: ", QnnErrorHandleToString(result));
  } else {
    ORT_RETURN_IF(QNN_PROFILE_NO_ERROR != result, "Failed to get profile events. Error: ", QnnErrorHandleToString(result));
  }

  if (num_events > 0) {
    LOGS(*logger_, VERBOSE) << "profile_events: " << profile_events << " num_events: " << num_events;

    bool backendSupportsExtendedEventData = false;
    Qnn_ErrorHandle_t resultPropertyHasCapability =
        qnn_interface_.propertyHasCapability(QNN_PROPERTY_PROFILE_SUPPORTS_EXTENDED_EVENT);
    uint16_t errorCodePropertyHasCapability = static_cast<uint16_t>(resultPropertyHasCapability & 0xFFFF);
    if (errorCodePropertyHasCapability == QNN_PROPERTY_SUPPORTED) {
      LOGS(*logger_, VERBOSE) << "The QNN backend supports extended event data.";
      backendSupportsExtendedEventData = true;
    } else {
      LOGS(*logger_, VERBOSE) << "The QNN backend does not support extended event data.";
    }

    std::ofstream outfile;
    if (!profiling_file_path_.empty()) {
      // Write to CSV in append mode
      std::ifstream infile(profiling_file_path_.c_str());
      bool exists = infile.good();
      infile.close();

      outfile.open(profiling_file_path_, std::ios_base::app);
      ORT_RETURN_IF(!outfile.is_open(), "Failed to open profiling file: ", profiling_file_path_);
      // If file didn't exist before, write the header
      if (!exists) {
        outfile << "Msg Timestamp,Message,Time,Unit of Measurement,Timing Source,Event Level,Event Identifier\n";
      }
    }

    for (size_t event_idx = 0; event_idx < num_events; event_idx++) {
      ORT_RETURN_IF_ERROR(
          ExtractProfilingEvent(*(profile_events + event_idx), "ROOT", outfile, backendSupportsExtendedEventData,
                                tracelogging_provider_ep_enabled));
      ORT_RETURN_IF_ERROR(
          ExtractProfilingSubEvents(*(profile_events + event_idx), outfile, backendSupportsExtendedEventData,
                                    tracelogging_provider_ep_enabled));
    }

    if (outfile) {
      LOGS(*logger_, VERBOSE) << "Wrote QNN profiling events (" << num_events << ") to file ("
                              << profiling_file_path_ << ")";
    }
    if (tracelogging_provider_ep_enabled) {
      LOGS(*logger_, VERBOSE) << "Wrote QNN profiling events (" << num_events << ") to ETW";
    }
  }

  return Status::OK();
}

Status QnnBackendManager::ExtractProfilingSubEvents(
    QnnProfile_EventId_t profile_event_id,
    std::ofstream& outfile,
    bool useExtendedEventData,
    bool tracelogging_provider_ep_enabled) {
  const QnnProfile_EventId_t* profile_sub_events{nullptr};
  uint32_t num_sub_events{0};
  Qnn_ErrorHandle_t result = qnn_interface_.profileGetSubEvents(profile_event_id, &profile_sub_events, &num_sub_events);
  ORT_RETURN_IF(QNN_PROFILE_NO_ERROR != result, "Failed to get profile sub events. Error: ", QnnErrorHandleToString(result));

  if (num_sub_events > 0) {
    LOGS(*logger_, VERBOSE) << "profile_sub_events: " << profile_sub_events << " num_sub_events: " << num_sub_events;

    for (size_t sub_event_idx = 0; sub_event_idx < num_sub_events; sub_event_idx++) {
      ORT_RETURN_IF_ERROR(
          ExtractProfilingEvent(*(profile_sub_events + sub_event_idx), "SUB-EVENT", outfile, useExtendedEventData,
                                tracelogging_provider_ep_enabled));
      ORT_RETURN_IF_ERROR(
          ExtractProfilingSubEvents(*(profile_sub_events + sub_event_idx), outfile, useExtendedEventData,
                                    tracelogging_provider_ep_enabled));
    }

    LOGS(*logger_, VERBOSE) << "Wrote QNN profiling sub events (" << num_sub_events << ")";
  }

  return Status::OK();
}

Status QnnBackendManager::ExtractProfilingEvent(
    QnnProfile_EventId_t profile_event_id,
    const std::string& eventLevel,
    std::ofstream& outfile,
    bool useExtendedEventData,
    bool tracelogging_provider_ep_enabled) {
  if (useExtendedEventData) {
    return ExtractProfilingEventExtended(profile_event_id, eventLevel, outfile, tracelogging_provider_ep_enabled);
  } else {
    return ExtractProfilingEventBasic(profile_event_id, eventLevel, outfile, tracelogging_provider_ep_enabled);
  }
}

Status QnnBackendManager::ExtractProfilingEventBasic(
    QnnProfile_EventId_t profile_event_id,
    const std::string& eventLevel,
    std::ofstream& outfile,
    bool tracelogging_provider_ep_enabled) {
  QnnProfile_EventData_t event_data;
  Qnn_ErrorHandle_t result = qnn_interface_.profileGetEventData(profile_event_id, &event_data);
  QnnProfile_Error_t errorCode = static_cast<QnnProfile_Error_t>(result & 0xFFFF);
  ORT_RETURN_IF(QNN_PROFILE_NO_ERROR != result, "Failed to get profile event data: " + std::string(QnnProfileErrorToString(errorCode)));

  std::string message = GetEventTypeString(event_data.type);
  std::string unit = GetUnitString(event_data.unit);

  if (outfile) {
    outfile << "UNKNOWN"
            << ","
            << message << ","
            << event_data.value << ","
            << unit << ","
            << "BACKEND"
            << ","
            << eventLevel << ","
            << (event_data.identifier ? event_data.identifier : "NULL") << "\n";
  }

  if (tracelogging_provider_ep_enabled) {
#ifdef _WIN32
    LogQnnProfileEventAsTraceLogging(
        (uint64_t)0,
        message,
        std::to_string(event_data.value),
        unit,
        "BACKEND",
        eventLevel,
        (event_data.identifier ? event_data.identifier : "NULL"));
#endif
  }

  return Status::OK();
}

Status QnnBackendManager::ExtractProfilingEventExtended(
    QnnProfile_EventId_t profile_event_id,
    const std::string& eventLevel,
    std::ofstream& outfile,
    bool tracelogging_provider_ep_enabled) {
  QnnProfile_ExtendedEventData_t event_data_extended;
  auto resultGetExtendedEventData = qnn_interface_.profileGetExtendedEventData(profile_event_id, &event_data_extended);
  QnnProfile_Error_t errorCode = static_cast<QnnProfile_Error_t>(resultGetExtendedEventData & 0xFFFF);
  ORT_RETURN_IF(QNN_PROFILE_NO_ERROR != errorCode, "Failed to get profile event data: " + std::string(QnnProfileErrorToString(errorCode)));

  // need to check the version first
  std::string message = GetEventTypeString(event_data_extended.v1.type);
  std::string unit = GetUnitString(event_data_extended.v1.unit);

  if (outfile) {
    if (event_data_extended.version == QNN_PROFILE_DATA_VERSION_1) {
      outfile << event_data_extended.v1.timestamp << ","
              << message << ","
              << ExtractQnnScalarValue(event_data_extended.v1.value) << ","
              << unit << ","
              << "BACKEND"
              << ","
              << eventLevel << ","
              << (event_data_extended.v1.identifier ? event_data_extended.v1.identifier : "NULL") << "\n";
    }
  }

  if (tracelogging_provider_ep_enabled) {
#ifdef _WIN32
    LogQnnProfileEventAsTraceLogging(
        event_data_extended.v1.timestamp,
        message,
        ExtractQnnScalarValue(event_data_extended.v1.value),
        unit,
        "BACKEND",
        eventLevel,
        (event_data_extended.v1.identifier ? event_data_extended.v1.identifier : "NULL"));
#endif
  }

  return Status::OK();
}

#ifdef _WIN32
void QnnBackendManager::LogQnnProfileEventAsTraceLogging(
    uint64_t timestamp,
    const std::string& message,
    const std::string& qnnScalarValue,
    const std::string& unit,
    const std::string& timingSource,
    const std::string& eventLevel,
    const char* eventIdentifier) {
  QnnTelemetry& qnn_telemetry = QnnTelemetry::Instance();
  qnn_telemetry.LogQnnProfileEvent(timestamp, message, qnnScalarValue, unit, timingSource, eventLevel, eventIdentifier);
}
#endif

const std::string& QnnBackendManager::GetUnitString(QnnProfile_EventUnit_t unitType) {
  const auto& unitStringMap = GetUnitStringMap();
  auto it = unitStringMap.find(unitType);
  if (it != unitStringMap.end()) {
    return it->second;
  }
  static const std::string unknown = "UNKNOWN";
  return unknown;
}

const std::unordered_map<QnnProfile_EventUnit_t, std::string>& QnnBackendManager::GetUnitStringMap() {
  static const std::unordered_map<QnnProfile_EventUnit_t, std::string> unitStringMap = {
      {QNN_PROFILE_EVENTUNIT_MICROSEC, "US"},
      {QNN_PROFILE_EVENTUNIT_BYTES, "BYTES"},
      {QNN_PROFILE_EVENTUNIT_CYCLES, "CYCLES"},
      {QNN_PROFILE_EVENTUNIT_COUNT, "COUNT"},
      {QNN_PROFILE_EVENTUNIT_OBJECT, "OBJECT"},
      {QNN_PROFILE_EVENTUNIT_BACKEND, "BACKEND"}};
  return unitStringMap;
}

const std::string QnnBackendManager::GetEventTypeString(QnnProfile_EventType_t eventType) {
  // Interpret the event type
  switch (eventType) {
    case QNN_PROFILE_EVENTTYPE_INIT:
      return "INIT";
    case QNN_PROFILE_EVENTTYPE_FINALIZE:
      return "FINALIZE";
    case QNN_PROFILE_EVENTTYPE_EXECUTE:
      return "EXECUTE";
    case QNN_PROFILE_EVENTTYPE_NODE:
      return "NODE";
    case QNN_PROFILE_EVENTTYPE_EXECUTE_QUEUE_WAIT:
      return "EXECUTE QUEUE WAIT";
    case QNN_PROFILE_EVENTTYPE_EXECUTE_PREPROCESS:
      return "EXECUTE PREPROCESS";
    case QNN_PROFILE_EVENTTYPE_EXECUTE_DEVICE:
      return "EXECUTE DEVICE";
    case QNN_PROFILE_EVENTTYPE_EXECUTE_POSTPROCESS:
      return "EXECUTE POSTPROCESS";
    case QNN_PROFILE_EVENTTYPE_DEINIT:
      return "DE-INIT";
    case QNN_PROFILE_EVENTTYPE_BACKEND:
      return "BACKEND";
    default:
      if (eventType > QNN_PROFILE_EVENTTYPE_BACKEND) {
        return "BACKEND";
      }
      return "UNKNOWN";
  }
}

const char* QnnBackendManager::QnnProfileErrorToString(QnnProfile_Error_t error) {
  switch (error) {
    case QNN_PROFILE_NO_ERROR:
      return "QNN_PROFILE_NO_ERROR";
    case QNN_PROFILE_ERROR_UNSUPPORTED:
      return "QNN_PROFILE_ERROR_UNSUPPORTED";
    case QNN_PROFILE_ERROR_INVALID_ARGUMENT:
      return "QNN_PROFILE_ERROR_INVALID_ARGUMENT";
    case QNN_PROFILE_ERROR_MEM_ALLOC:
      return "QNN_PROFILE_ERROR_MEM_ALLOC";
    case QNN_PROFILE_ERROR_INVALID_HANDLE:
      return "QNN_PROFILE_ERROR_INVALID_HANDLE";
    case QNN_PROFILE_ERROR_HANDLE_IN_USE:
      return "QNN_PROFILE_ERROR_HANDLE_IN_USE";
    case QNN_PROFILE_ERROR_INCOMPATIBLE_EVENT:
      return "QNN_PROFILE_ERROR_INCOMPATIBLE_EVENT";
    default:
      return "UNKNOWN_ERROR";
  }
}

std::string QnnBackendManager::QnnErrorHandleToString(Qnn_ErrorHandle_t error) {
  return utils::GetQnnErrorMessage(qnn_interface_, error);
}

const std::string QnnBackendManager::ExtractQnnScalarValue(const Qnn_Scalar_t& scalar) {
  switch (scalar.dataType) {
    case QNN_DATATYPE_INT_8:
      return std::to_string(static_cast<int>(scalar.int8Value));
    case QNN_DATATYPE_INT_16:
      return std::to_string(scalar.int16Value);
    case QNN_DATATYPE_INT_32:
      return std::to_string(scalar.int32Value);
    case QNN_DATATYPE_INT_64:
      return std::to_string(scalar.int64Value);
    case QNN_DATATYPE_UINT_8:
      return std::to_string(static_cast<unsigned int>(scalar.uint8Value));
    case QNN_DATATYPE_UINT_16:
      return std::to_string(scalar.uint16Value);
    case QNN_DATATYPE_UINT_32:
      return std::to_string(scalar.uint32Value);
    case QNN_DATATYPE_UINT_64:
      return std::to_string(scalar.uint64Value);
    case QNN_DATATYPE_FLOAT_16:
      return std::to_string(scalar.floatValue);
    case QNN_DATATYPE_FLOAT_32:
      return std::to_string(scalar.floatValue);
    case QNN_DATATYPE_SFIXED_POINT_8:
    case QNN_DATATYPE_SFIXED_POINT_16:
    case QNN_DATATYPE_SFIXED_POINT_32:
      return std::to_string(scalar.int32Value);  // Assume using int types for signed fixed points.
    case QNN_DATATYPE_UFIXED_POINT_8:
    case QNN_DATATYPE_UFIXED_POINT_16:
    case QNN_DATATYPE_UFIXED_POINT_32:
      return std::to_string(scalar.uint32Value);  // Assume using unsigned int types for unsigned fixed points.
    case QNN_DATATYPE_BOOL_8:
      return scalar.bool8Value ? "true" : "false";
    case QNN_DATATYPE_STRING:
      return scalar.stringValue ? scalar.stringValue : "NULL";
    default:
      return "UNKNOWN";
  }
}

QnnBackendManager::~QnnBackendManager() {
  ReleaseResources();
}

void* QnnBackendManager::LoadLib(const char* file_name, int flags, std::string& error_msg) {
#ifdef _WIN32
  DWORD as_is, to_be;
  bool loaded_before = false;

  if (!file_name || ::strlen(file_name) == 0) {
    error_msg = "filename is null or empty";
    return nullptr;
  }

  // POSIX asks one of symbol resolving approaches:
  // NOW or LAZY must be specified
  if (!(flags & static_cast<int>(DlOpenFlag::DL_NOW))) {
    error_msg = "flags must include DL_NOW";
    return nullptr;
  }

  HANDLE cur_proc = GetCurrentProcess();

  if (EnumProcessModules(cur_proc, nullptr, 0, &as_is) == 0) {
    error_msg = "enumerate modules failed before loading module";
    return nullptr;
  }

  HMODULE mod;
  auto file_path = std::filesystem::path(file_name);
  if (!file_path.is_absolute()) {
    // construct an absolute path from ORT runtime path + file_name and check whether it exists.
    const Env& env = GetDefaultEnv();
    auto pathstring = env.GetRuntimePath() + ToPathString(file_name);
    auto absolute_path = pathstring.c_str();
    if (std::filesystem::exists(std::filesystem::path(absolute_path))) {
      // load library from absolute path and search for dependencies there.
      mod = LoadLibraryExW(absolute_path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    } else {
      // use default dll search order for file_name.
      mod = LoadLibraryExA(file_name, nullptr, 0);
    }
  } else {
    // file_name represents an absolute path.
    // load library from absolute path and search for dependencies there.
    mod = LoadLibraryExA(file_name, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  }
  if (!mod) {
    error_msg = "load library failed";
    return nullptr;
  }

  if (EnumProcessModules(cur_proc, nullptr, 0, &to_be) == 0) {
    error_msg = "enumerate modules failed after loading module";
    FreeLibrary(mod);
    return nullptr;
  }

  if (as_is == to_be) {
    loaded_before = true;
  }

  // (not loaded_before) and DL_LOCAL means this lib was not loaded yet
  // add it into the local set
  //
  // If loaded_before and DL_LOCAL, means this lib was already loaded
  // 2 cases here for how it was loaded before:
  // a. with DL_LOCAL, just ignore since it was already in local set
  // b. with DL_GLOBAL, POSIX asks it in global, ignore it, too
  if ((!loaded_before) && (flags & static_cast<int>(DlOpenFlag::DL_LOCAL))) {
    mod_handles_.insert(mod);
  }

  // once callers ask for global, needs to be in global thereafter
  // so the lib should be removed from local set
  if (flags & static_cast<int>(DlOpenFlag::DL_GLOBAL)) {
    mod_handles_.erase(mod);
  }

  return static_cast<void*>(mod);
#else
  ORT_UNUSED_PARAMETER(error_msg);
  int real_flags = 0;

  if (flags & static_cast<int>(DlOpenFlag::DL_NOW)) {
    real_flags |= RTLD_NOW;
  }

  if (flags & static_cast<int>(DlOpenFlag::DL_LOCAL)) {
    real_flags |= RTLD_LOCAL;
  }

  if (flags & static_cast<int>(DlOpenFlag::DL_GLOBAL)) {
    real_flags |= RTLD_GLOBAL;
  }

  return ::dlopen(file_name, real_flags);
#endif
}

Status QnnBackendManager::UnloadLib(void* handle) {
  if (!handle) {
    return Status::OK();
  }

#ifdef _WIN32
  HMODULE mod = static_cast<HMODULE>(handle);

  if (FreeLibrary(mod) == 0) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Failed to free library.");
  }
  mod_handles_.erase(mod);
#else
  auto rt = ::dlclose(handle);
  if (rt != 0) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Failed to free library.");
  }
#endif  // defined(_WIN32)

  return Status::OK();
}

void* QnnBackendManager::LibFunction(void* handle, const char* symbol, std::string& error_msg) {
#ifdef _WIN32
  FARPROC sym_addr = nullptr;
  DWORD size, size_needed;
  HMODULE mod = 0;

  if ((!handle) || (!symbol)) {
    return nullptr;
  }

  HANDLE cur_proc = GetCurrentProcess();

  if (EnumProcessModules(cur_proc, nullptr, 0, &size) == 0) {
    error_msg = "enumerate modules failed before memory allocation";
    return nullptr;
  }

  HMODULE* mod_list = static_cast<HMODULE*>(malloc(size));
  if (!mod_list) {
    error_msg = "malloc failed";
    return nullptr;
  }

  if (EnumProcessModules(cur_proc, mod_list, size, &size_needed) == 0) {
    error_msg = "enumerate modules failed after memory allocation";
    free(mod_list);
    return nullptr;
  }

  // DL_DEFAULT needs to bypass those modules with DL_LOCAL flag
  if (handle == DL_DEFAULT) {
    for (size_t i = 0; i < (size / sizeof(HMODULE)); i++) {
      auto iter = mod_handles_.find(mod_list[i]);
      if (iter != mod_handles_.end()) {
        continue;
      }
      // once find the first non-local module with symbol
      // return its address here to avoid unnecessary looping
      sym_addr = GetProcAddress(mod_list[i], symbol);
      if (sym_addr) {
        free(mod_list);
        return *(void**)(&sym_addr);
      }
    }
  } else {
    mod = static_cast<HMODULE>(handle);
  }

  free(mod_list);
  sym_addr = GetProcAddress(mod, symbol);
  if (!sym_addr) {
    error_msg = "can't resolve symbol";
    return NULL;
  }

  return *(void**)(&sym_addr);
#else
  ORT_UNUSED_PARAMETER(error_msg);
  if (handle == DL_DEFAULT) {
    return ::dlsym(RTLD_DEFAULT, symbol);
  }

  return ::dlsym(handle, symbol);
#endif
}

Status QnnBackendManager::AddQnnContextHandle(Qnn_ContextHandle_t raw_context_handle) {
  ORT_RETURN_IF(logger_ == nullptr, "logger_ should be set.");

  auto free_context_handle = [this, &logger = *logger_](Qnn_ContextHandle_t raw_context_handle) {
    const auto free_result = qnn_interface_.contextFree(raw_context_handle, nullptr);
    if (free_result != QNN_CONTEXT_NO_ERROR) {
      LOGS(logger, ERROR) << "qnn_interface.contextFree() failed: "
                          << utils::GetVerboseQnnErrorMessage(qnn_interface_, free_result);
    }
  };

  // take ownership of `raw_context_handle`
  auto context_handle = UniqueQnnContextHandle(raw_context_handle, free_context_handle);
  auto mem_handle_manager = std::make_unique<QnnContextMemHandleManager>(GetQnnInterface(), raw_context_handle,
                                                                         *logger_);

  auto context_handle_record = std::make_shared<QnnContextHandleRecord>();
  context_handle_record->context_handle = std::move(context_handle);
  context_handle_record->mem_handles = std::move(mem_handle_manager);

  const bool inserted = context_map_.try_emplace(raw_context_handle, std::move(context_handle_record)).second;
  ORT_RETURN_IF_NOT(inserted, "QNN context was already added: ", raw_context_handle);

  contexts_.push_back(raw_context_handle);

  return Status::OK();
}

Status QnnBackendManager::GetOrRegisterContextMemHandle(Qnn_ContextHandle_t context_handle,
                                                        void* shared_memory_address,
                                                        const Qnn_Tensor_t& qnn_tensor,
                                                        Qnn_MemHandle_t& mem_handle) {
  // Multi-threading situations to consider:
  // 1) Shared memory allocation is being freed in another thread while we are processing `shared_memory_address`.
  //    This implies incorrect usage as the memory is being freed while it is still in use. Let's assume this won't
  //    happen.
  // 2) The shared memory allocation clean up function is being run from another thread while the
  //    QnnContextHandleRecord or QnnBackendManager objects are being destroyed.
  //    Usage of weak_ptrs from the clean up function should ensure that those objects are only accessed while they are
  //    in scope.

  const auto context_handle_record_it = context_map_.find(context_handle);
  ORT_RETURN_IF_NOT(context_handle_record_it != context_map_.end(), "QNN context not found: ", context_handle);

  auto& context_handle_record = context_handle_record_it->second;
  auto& context_mem_handle_manager = context_handle_record->mem_handles;

  bool did_register{};
  ORT_RETURN_IF_ERROR(context_mem_handle_manager->GetOrRegister(shared_memory_address, qnn_tensor,
                                                                mem_handle, did_register));

  if (did_register) {
    HtpSharedMemoryAllocator::AllocationCleanUpFn unregister_mem_handle =
        [&logger = *logger_,
         shared_memory_address,
         weak_backend_manager = weak_from_this(),
         weak_context_handle_record = std::weak_ptr{context_handle_record}](
            void* /* allocation_base_address */) {
          // Lock QnnBackendManager shared_ptr to ensure that QNN interface is still valid.
          auto backend_manager = weak_backend_manager.lock();
          if (!backend_manager) {
            return;
          }

          // Lock QnnContextHandleRecord shared_ptr to ensure that QNN context handle is still valid.
          auto context_handle_record = weak_context_handle_record.lock();
          if (!context_handle_record) {
            return;
          }

          auto& context_mem_handle_manager = context_handle_record->mem_handles;

          auto unregister_status = context_mem_handle_manager->Unregister(shared_memory_address);
          if (!unregister_status.IsOK()) {
            LOGS(logger, ERROR) << "Failed to unregister shared memory mem handle for address: "
                                << shared_memory_address << ", error: " << unregister_status.ErrorMessage();
          }
        };

    ORT_RETURN_IF_ERROR(HtpSharedMemoryAllocator::AddAllocationCleanUp(shared_memory_address,
                                                                       std::move(unregister_mem_handle)));
  }

  return Status::OK();
}

}  // namespace qnn
}  // namespace onnxruntime
