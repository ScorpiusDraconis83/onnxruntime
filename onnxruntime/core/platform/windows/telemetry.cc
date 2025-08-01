// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/platform/windows/telemetry.h"
#include <mutex>
#include "core/common/logging/logging.h"
#include "onnxruntime_config.h"

// ETW includes
// need space after Windows.h to prevent clang-format re-ordering breaking the build.
// TraceLoggingProvider.h must follow Windows.h
#include <Windows.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 26440)  // Warning C26440 from TRACELOGGING_DEFINE_PROVIDER
#endif

#include <TraceLoggingProvider.h>
#include <evntrace.h>
#include <winmeta.h>

// Seems this workaround can be dropped when we drop support for VS2017 toolchains
// https://developercommunity.visualstudio.com/content/problem/85934/traceloggingproviderh-is-incompatible-with-utf-8.html
#ifdef _TlgPragmaUtf8Begin
#undef _TlgPragmaUtf8Begin
#define _TlgPragmaUtf8Begin
#endif

#ifdef _TlgPragmaUtf8End
#undef _TlgPragmaUtf8End
#define _TlgPragmaUtf8End
#endif

// Different versions of TraceLoggingProvider.h contain different macro variable names for the utf8 begin and end,
// and we need to cover the lower case version as well.
#ifdef _tlgPragmaUtf8Begin
#undef _tlgPragmaUtf8Begin
#define _tlgPragmaUtf8Begin
#endif

#ifdef _tlgPragmaUtf8End
#undef _tlgPragmaUtf8End
#define _tlgPragmaUtf8End
#endif

namespace onnxruntime {

namespace {
TRACELOGGING_DEFINE_PROVIDER(telemetry_provider_handle, "Microsoft.ML.ONNXRuntime",
                             // {3a26b1ff-7484-7484-7484-15261f42614d}
                             (0x3a26b1ff, 0x7484, 0x7484, 0x74, 0x84, 0x15, 0x26, 0x1f, 0x42, 0x61, 0x4d),
                             TraceLoggingOptionMicrosoftTelemetry());
}  // namespace

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#ifndef ORT_CALLER_FRAMEWORK
#define ORT_CALLER_FRAMEWORK ""
#endif

std::mutex WindowsTelemetry::mutex_;
std::mutex WindowsTelemetry::provider_change_mutex_;
uint32_t WindowsTelemetry::global_register_count_ = 0;
bool WindowsTelemetry::enabled_ = true;
uint32_t WindowsTelemetry::projection_ = 0;
UCHAR WindowsTelemetry::level_ = 0;
UINT64 WindowsTelemetry::keyword_ = 0;
std::vector<const WindowsTelemetry::EtwInternalCallback*> WindowsTelemetry::callbacks_;
std::mutex WindowsTelemetry::callbacks_mutex_;

WindowsTelemetry::WindowsTelemetry() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (global_register_count_ == 0) {
    // TraceLoggingRegister is fancy in that you can only register once GLOBALLY for the whole process
    HRESULT hr = TraceLoggingRegisterEx(telemetry_provider_handle, ORT_TL_EtwEnableCallback, nullptr);
    if (SUCCEEDED(hr)) {
      global_register_count_ += 1;
    }
  }
}

WindowsTelemetry::~WindowsTelemetry() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (global_register_count_ > 0) {
    global_register_count_ -= 1;
    if (global_register_count_ == 0) {
      TraceLoggingUnregister(telemetry_provider_handle);
    }
  }

  std::lock_guard<std::mutex> lock_callbacks(callbacks_mutex_);
  callbacks_.clear();
}

bool WindowsTelemetry::IsEnabled() const {
  std::lock_guard<std::mutex> lock(provider_change_mutex_);
  return enabled_;
}

UCHAR WindowsTelemetry::Level() const {
  std::lock_guard<std::mutex> lock(provider_change_mutex_);
  return level_;
}

UINT64 WindowsTelemetry::Keyword() const {
  std::lock_guard<std::mutex> lock(provider_change_mutex_);
  return keyword_;
}

// HRESULT WindowsTelemetry::Status() {
//     return etw_status_;
// }

void WindowsTelemetry::RegisterInternalCallback(const EtwInternalCallback& callback) {
  std::lock_guard<std::mutex> lock_callbacks(callbacks_mutex_);
  callbacks_.push_back(&callback);
}

void WindowsTelemetry::UnregisterInternalCallback(const EtwInternalCallback& callback) {
  std::lock_guard<std::mutex> lock_callbacks(callbacks_mutex_);
  auto new_end = std::remove_if(callbacks_.begin(), callbacks_.end(),
                                [&callback](const EtwInternalCallback* ptr) {
                                  return ptr == &callback;
                                });
  callbacks_.erase(new_end, callbacks_.end());
}

void NTAPI WindowsTelemetry::ORT_TL_EtwEnableCallback(
    _In_ LPCGUID SourceId,
    _In_ ULONG IsEnabled,
    _In_ UCHAR Level,
    _In_ ULONGLONG MatchAnyKeyword,
    _In_ ULONGLONG MatchAllKeyword,
    _In_opt_ PEVENT_FILTER_DESCRIPTOR FilterData,
    _In_opt_ PVOID CallbackContext) {
  std::lock_guard<std::mutex> lock(provider_change_mutex_);
  enabled_ = (IsEnabled != 0);
  level_ = Level;
  keyword_ = MatchAnyKeyword;

  InvokeCallbacks(SourceId, IsEnabled, Level, MatchAnyKeyword, MatchAllKeyword, FilterData, CallbackContext);
}

void WindowsTelemetry::InvokeCallbacks(LPCGUID SourceId, ULONG IsEnabled, UCHAR Level, ULONGLONG MatchAnyKeyword,
                                       ULONGLONG MatchAllKeyword, PEVENT_FILTER_DESCRIPTOR FilterData,
                                       PVOID CallbackContext) {
  std::lock_guard<std::mutex> lock_callbacks(callbacks_mutex_);
  for (const auto& callback : callbacks_) {
    (*callback)(SourceId, IsEnabled, Level, MatchAnyKeyword, MatchAllKeyword, FilterData, CallbackContext);
  }
}

void WindowsTelemetry::EnableTelemetryEvents() const {
  enabled_ = true;
}

void WindowsTelemetry::DisableTelemetryEvents() const {
  enabled_ = false;
}

void WindowsTelemetry::SetLanguageProjection(uint32_t projection) const {
  projection_ = projection;
}

void WindowsTelemetry::LogProcessInfo() const {
  if (global_register_count_ == 0 || enabled_ == false)
    return;

  static std::atomic<bool> process_info_logged;

  // did we already log the process info?  we only need to log it once
  if (process_info_logged.exchange(true))
    return;
  bool isRedist = true;
#if BUILD_INBOX
  isRedist = false;
#endif
  TraceLoggingWrite(telemetry_provider_handle,
                    "ProcessInfo",
                    TraceLoggingBool(true, "UTCReplace_AppSessionGuid"),
                    TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage),
                    TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
                    TraceLoggingLevel(WINEVENT_LEVEL_INFO),
                    // Telemetry info
                    TraceLoggingUInt8(0, "schemaVersion"),
                    TraceLoggingString(ORT_VERSION, "runtimeVersion"),
                    TraceLoggingBool(IsDebuggerPresent(), "isDebuggerAttached"),
                    TraceLoggingBool(isRedist, "isRedist"),
                    TraceLoggingString(ORT_CALLER_FRAMEWORK, "frameworkName"));

  process_info_logged = true;
}

void WindowsTelemetry::LogSessionCreationStart(uint32_t session_id) const {
  if (global_register_count_ == 0 || enabled_ == false)
    return;

  TraceLoggingWrite(telemetry_provider_handle,
                    "SessionCreationStart",
                    TraceLoggingBool(true, "UTCReplace_AppSessionGuid"),
                    TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage),
                    TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
                    TraceLoggingUInt32(session_id, "sessionId"),
                    TraceLoggingLevel(WINEVENT_LEVEL_INFO));
}

void WindowsTelemetry::LogEvaluationStop(uint32_t session_id) const {
  if (global_register_count_ == 0 || enabled_ == false)
    return;

  TraceLoggingWrite(telemetry_provider_handle,
                    "EvaluationStop",
                    TraceLoggingUInt32(session_id, "sessionId"));
}

void WindowsTelemetry::LogEvaluationStart(uint32_t session_id) const {
  if (global_register_count_ == 0 || enabled_ == false)
    return;

  TraceLoggingWrite(telemetry_provider_handle,
                    "EvaluationStart",
                    TraceLoggingUInt32(session_id, "sessionId"));
}

void WindowsTelemetry::LogSessionCreation(uint32_t session_id, int64_t ir_version, const std::string& model_producer_name,
                                          const std::string& model_producer_version, const std::string& model_domain,
                                          const std::unordered_map<std::string, int>& domain_to_version_map,
                                          const std::string& model_file_name,
                                          const std::string& model_graph_name,
                                          const std::string& model_weight_type,
                                          const std::string& model_graph_hash,
                                          const std::string& model_weight_hash,
                                          const std::unordered_map<std::string, std::string>& model_metadata,
                                          const std::string& loaded_from, const std::vector<std::string>& execution_provider_ids,
                                          bool use_fp16, bool captureState) const {
  if (global_register_count_ == 0 || enabled_ == false)
    return;

  // build the strings we need

  std::string domain_to_version_string;
  bool first = true;
  for (auto& i : domain_to_version_map) {
    if (first) {
      first = false;
    } else {
      domain_to_version_string += ',';
    }
    domain_to_version_string += i.first;
    domain_to_version_string += '=';
    domain_to_version_string += std::to_string(i.second);
  }

  std::string model_metadata_string;
  first = true;
  for (auto& i : model_metadata) {
    if (first) {
      first = false;
    } else {
      model_metadata_string += ',';
    }
    model_metadata_string += i.first;
    model_metadata_string += '=';
    model_metadata_string += i.second;
  }

  std::string execution_provider_string;
  first = true;
  for (auto& i : execution_provider_ids) {
    if (first) {
      first = false;
    } else {
      execution_provider_string += ',';
    }
    execution_provider_string += i;
  }

  // Difference is MeasureEvent & isCaptureState, but keep in sync otherwise
  if (!captureState) {
    TraceLoggingWrite(telemetry_provider_handle,
                      "SessionCreation",
                      TraceLoggingBool(true, "UTCReplace_AppSessionGuid"),
                      TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage),
                      TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
                      TraceLoggingKeyword(static_cast<uint64_t>(onnxruntime::logging::ORTTraceLoggingKeyword::Session)),
                      TraceLoggingLevel(WINEVENT_LEVEL_INFO),
                      // Telemetry info
                      TraceLoggingUInt8(0, "schemaVersion"),
                      TraceLoggingUInt32(session_id, "sessionId"),
                      TraceLoggingInt64(ir_version, "irVersion"),
                      TraceLoggingUInt32(projection_, "OrtProgrammingProjection"),
                      TraceLoggingString(model_producer_name.c_str(), "modelProducerName"),
                      TraceLoggingString(model_producer_version.c_str(), "modelProducerVersion"),
                      TraceLoggingString(model_domain.c_str(), "modelDomain"),
                      TraceLoggingBool(use_fp16, "usefp16"),
                      TraceLoggingString(domain_to_version_string.c_str(), "domainToVersionMap"),
                      TraceLoggingString(model_file_name.c_str(), "modelFileName"),
                      TraceLoggingString(model_graph_name.c_str(), "modelGraphName"),
                      TraceLoggingString(model_weight_type.c_str(), "modelWeightType"),
                      TraceLoggingString(model_graph_hash.c_str(), "modelGraphHash"),
                      TraceLoggingString(model_weight_hash.c_str(), "modelWeightHash"),
                      TraceLoggingString(model_metadata_string.c_str(), "modelMetaData"),
                      TraceLoggingString(loaded_from.c_str(), "loadedFrom"),
                      TraceLoggingString(execution_provider_string.c_str(), "executionProviderIds"));
  } else {
    TraceLoggingWrite(telemetry_provider_handle,
                      "SessionCreation_CaptureState",
                      TraceLoggingBool(true, "UTCReplace_AppSessionGuid"),
                      TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage),
                      // Not a measure event
                      TraceLoggingKeyword(static_cast<uint64_t>(onnxruntime::logging::ORTTraceLoggingKeyword::Session)),
                      TraceLoggingLevel(WINEVENT_LEVEL_INFO),
                      // Telemetry info
                      TraceLoggingUInt8(0, "schemaVersion"),
                      TraceLoggingUInt32(session_id, "sessionId"),
                      TraceLoggingInt64(ir_version, "irVersion"),
                      TraceLoggingUInt32(projection_, "OrtProgrammingProjection"),
                      TraceLoggingString(model_producer_name.c_str(), "modelProducerName"),
                      TraceLoggingString(model_producer_version.c_str(), "modelProducerVersion"),
                      TraceLoggingString(model_domain.c_str(), "modelDomain"),
                      TraceLoggingBool(use_fp16, "usefp16"),
                      TraceLoggingString(domain_to_version_string.c_str(), "domainToVersionMap"),
                      TraceLoggingString(model_file_name.c_str(), "modelFileName"),
                      TraceLoggingString(model_graph_name.c_str(), "modelGraphName"),
                      TraceLoggingString(model_weight_type.c_str(), "modelWeightType"),
                      TraceLoggingString(model_graph_hash.c_str(), "modelGraphHash"),
                      TraceLoggingString(model_weight_hash.c_str(), "modelWeightHash"),
                      TraceLoggingString(model_metadata_string.c_str(), "modelMetaData"),
                      TraceLoggingString(loaded_from.c_str(), "loadedFrom"),
                      TraceLoggingString(execution_provider_string.c_str(), "executionProviderIds"));
  }
}

void WindowsTelemetry::LogRuntimeError(uint32_t session_id, const common::Status& status, const char* file,
                                       const char* function, uint32_t line) const {
  if (global_register_count_ == 0 || enabled_ == false)
    return;

#ifdef _WIN32
  HRESULT hr = common::StatusCodeToHRESULT(static_cast<common::StatusCode>(status.Code()));
  TraceLoggingWrite(telemetry_provider_handle,
                    "RuntimeError",
                    TraceLoggingBool(true, "UTCReplace_AppSessionGuid"),
                    TelemetryPrivacyDataTag(PDT_ProductAndServicePerformance),
                    TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
                    TraceLoggingLevel(WINEVENT_LEVEL_ERROR),
                    // Telemetry info
                    TraceLoggingUInt8(0, "schemaVersion"),
                    TraceLoggingHResult(hr, "hResult"),
                    TraceLoggingUInt32(session_id, "sessionId"),
                    TraceLoggingUInt32(status.Code(), "errorCode"),
                    TraceLoggingUInt32(status.Category(), "errorCategory"),
                    TraceLoggingString(status.ErrorMessage().c_str(), "errorMessage"),
                    TraceLoggingString(file, "file"),
                    TraceLoggingString(function, "function"),
                    TraceLoggingInt32(line, "line"));
#else
  TraceLoggingWrite(telemetry_provider_handle,
                    "RuntimeError",
                    TraceLoggingBool(true, "UTCReplace_AppSessionGuid"),
                    TelemetryPrivacyDataTag(PDT_ProductAndServicePerformance),
                    TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
                    TraceLoggingLevel(WINEVENT_LEVEL_ERROR),
                    // Telemetry info
                    TraceLoggingUInt8(0, "schemaVersion"),
                    TraceLoggingUInt32(session_id, "sessionId"),
                    TraceLoggingUInt32(status.Code(), "errorCode"),
                    TraceLoggingUInt32(status.Category(), "errorCategory"),
                    TraceLoggingString(status.ErrorMessage().c_str(), "errorMessage"),
                    TraceLoggingString(file, "file"),
                    TraceLoggingString(function, "function"),
                    TraceLoggingInt32(line, "line"));
#endif
}

void WindowsTelemetry::LogRuntimePerf(uint32_t session_id, uint32_t total_runs_since_last, int64_t total_run_duration_since_last,
                                      std::unordered_map<int64_t, long long> duration_per_batch_size) const {
  if (global_register_count_ == 0 || enabled_ == false)
    return;

  // Convert duration_per_batch_size to a formatted string
  std::string total_duration_per_batch_size;
  for (const auto& entry : duration_per_batch_size) {
    if (!total_duration_per_batch_size.empty()) {
      total_duration_per_batch_size += ", ";
    }
    total_duration_per_batch_size += std::to_string(entry.first);
    total_duration_per_batch_size += ": ";
    total_duration_per_batch_size += std::to_string(entry.second);
  }

  TraceLoggingWrite(telemetry_provider_handle,
                    "RuntimePerf",
                    TraceLoggingBool(true, "UTCReplace_AppSessionGuid"),
                    TelemetryPrivacyDataTag(PDT_ProductAndServicePerformance),
                    TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
                    // Telemetry info
                    TraceLoggingUInt8(0, "schemaVersion"),
                    TraceLoggingUInt32(session_id, "sessionId"),
                    TraceLoggingUInt32(total_runs_since_last, "totalRuns"),
                    TraceLoggingInt64(total_run_duration_since_last, "totalRunDuration"),
                    TraceLoggingString(total_duration_per_batch_size.c_str(), "totalRunDurationPerBatchSize"));
}

void WindowsTelemetry::LogExecutionProviderEvent(LUID* adapterLuid) const {
  if (global_register_count_ == 0 || enabled_ == false)
    return;

  TraceLoggingWrite(telemetry_provider_handle,
                    "ExecutionProviderEvent",
                    TraceLoggingBool(true, "UTCReplace_AppSessionGuid"),
                    TelemetryPrivacyDataTag(PDT_ProductAndServicePerformance),
                    TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
                    // Telemetry info
                    TraceLoggingUInt32(adapterLuid->LowPart, "adapterLuidLowPart"),
                    TraceLoggingUInt32(adapterLuid->HighPart, "adapterLuidHighPart"));
}

void WindowsTelemetry::LogDriverInfoEvent(const std::string_view device_class, const std::wstring_view& driver_names, const std::wstring_view& driver_versions) const {
  if (global_register_count_ == 0 || enabled_ == false)
    return;

  TraceLoggingWrite(telemetry_provider_handle,
                    "DriverInfo",
                    TraceLoggingBool(true, "UTCReplace_AppSessionGuid"),
                    TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage),
                    TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
                    TraceLoggingLevel(WINEVENT_LEVEL_INFO),
                    // Telemetry info
                    TraceLoggingUInt8(0, "schemaVersion"),
                    TraceLoggingString(device_class.data(), "deviceClass"),
                    TraceLoggingWideString(driver_names.data(), "driverNames"),
                    TraceLoggingWideString(driver_versions.data(), "driverVersions"));
}

void WindowsTelemetry::LogAutoEpSelection(uint32_t session_id, const std::string& selection_policy,
                                          const std::vector<std::string>& requested_execution_provider_ids,
                                          const std::vector<std::string>& available_execution_provider_ids) const {
  if (global_register_count_ == 0 || enabled_ == false)
    return;

  // Build requested execution provider string
  std::string requested_execution_provider_string;
  bool first = true;
  for (const auto& ep_id : requested_execution_provider_ids) {
    if (first) {
      first = false;
    } else {
      requested_execution_provider_string += ',';
    }
    requested_execution_provider_string += ep_id;
  }

  // Build available execution provider string
  std::string available_execution_provider_string;
  first = true;
  for (const auto& ep_id : available_execution_provider_ids) {
    if (first) {
      first = false;
    } else {
      available_execution_provider_string += ',';
    }
    available_execution_provider_string += ep_id;
  }

  TraceLoggingWrite(telemetry_provider_handle,
                    "EpAutoSelection",
                    TraceLoggingBool(true, "UTCReplace_AppSessionGuid"),
                    TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage),
                    TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
                    TraceLoggingKeyword(static_cast<uint64_t>(onnxruntime::logging::ORTTraceLoggingKeyword::Session)),
                    TraceLoggingLevel(WINEVENT_LEVEL_INFO),
                    // Telemetry info
                    TraceLoggingUInt8(0, "schemaVersion"),
                    TraceLoggingUInt32(session_id, "sessionId"),
                    TraceLoggingString(selection_policy.c_str(), "selectionPolicy"),
                    TraceLoggingString(requested_execution_provider_string.c_str(), "requestedExecutionProviderIds"),
                    TraceLoggingString(available_execution_provider_string.c_str(), "availableExecutionProviderIds"));
}

void WindowsTelemetry::LogProviderOptions(const std::string& provider_id, const std::string& provider_options_string, bool captureState) const {
  if (global_register_count_ == 0 || enabled_ == false)
    return;

  // Difference is MeasureEvent & isCaptureState, but keep in sync otherwise
  if (!captureState) {
    TraceLoggingWrite(telemetry_provider_handle,
                      "ProviderOptions",
                      TraceLoggingBool(true, "UTCReplace_AppSessionGuid"),
                      TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage),
                      TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
                      TraceLoggingKeyword(static_cast<uint64_t>(onnxruntime::logging::ORTTraceLoggingKeyword::Session)),
                      TraceLoggingLevel(WINEVENT_LEVEL_INFO),
                      // Telemetry info
                      TraceLoggingUInt8(0, "schemaVersion"),
                      TraceLoggingString(provider_id.c_str(), "providerId"),
                      TraceLoggingString(provider_options_string.c_str(), "providerOptions"));
  } else {
    TraceLoggingWrite(telemetry_provider_handle,
                      "ProviderOptions_CaptureState",
                      TraceLoggingBool(true, "UTCReplace_AppSessionGuid"),
                      TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage),
                      // Not a measure event
                      TraceLoggingKeyword(static_cast<uint64_t>(onnxruntime::logging::ORTTraceLoggingKeyword::Session)),
                      TraceLoggingLevel(WINEVENT_LEVEL_INFO),
                      // Telemetry info
                      TraceLoggingUInt8(0, "schemaVersion"),
                      TraceLoggingString(provider_id.c_str(), "providerId"),
                      TraceLoggingString(provider_options_string.c_str(), "providerOptions"));
  }
}

}  // namespace onnxruntime
