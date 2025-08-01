// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <string>
#include <vector>
#include <unordered_map>

#include "core/common/status.h"
#include "core/common/common.h"

struct _LUID;
using LUID = _LUID;

namespace onnxruntime {

/**
 * Configuration information for a session.
 * An interface used by the onnxruntime implementation to
 * access operating system functionality for telemetry
 *
 * look at env.h and the Env objection which is the activation factory
 * for telemetry instances
 *
 * All Telemetry implementations are safe for concurrent access from
 * multiple threads without any external synchronization.
 */
class Telemetry {
 public:
  // don't create these, use Env::GetTelemetryProvider() instead
  // this constructor is made public so that other platform Env providers can
  // use this base class as a "stub" implementation
  Telemetry() = default;
  virtual ~Telemetry() = default;

  virtual void EnableTelemetryEvents() const;
  virtual void DisableTelemetryEvents() const;
  virtual void SetLanguageProjection(uint32_t projection) const;

  virtual bool IsEnabled() const;

  // Get the current logging level
  virtual unsigned char Level() const;

  // Get the current keyword
  virtual uint64_t Keyword() const;

  virtual void LogProcessInfo() const;

  virtual void LogSessionCreationStart(uint32_t session_id) const;

  virtual void LogEvaluationStop(uint32_t session_id) const;

  virtual void LogEvaluationStart(uint32_t session_id) const;

  virtual void LogSessionCreation(uint32_t session_id, int64_t ir_version, const std::string& model_producer_name,
                                  const std::string& model_producer_version, const std::string& model_domain,
                                  const std::unordered_map<std::string, int>& domain_to_version_map,
                                  const std::string& model_file_name,
                                  const std::string& model_graph_name,
                                  const std::string& model_weight_type,
                                  const std::string& model_graph_hash,
                                  const std::string& model_weight_hash,
                                  const std::unordered_map<std::string, std::string>& model_metadata,
                                  const std::string& loadedFrom, const std::vector<std::string>& execution_provider_ids,
                                  bool use_fp16, bool captureState) const;

  virtual void LogRuntimeError(uint32_t session_id, const common::Status& status, const char* file,
                               const char* function, uint32_t line) const;

  virtual void LogRuntimePerf(uint32_t session_id, uint32_t total_runs_since_last, int64_t total_run_duration_since_last,
                              std::unordered_map<int64_t, long long> duration_per_batch_size) const;

  virtual void LogExecutionProviderEvent(LUID* adapterLuid) const;

  virtual void LogDriverInfoEvent(const std::string_view device_class,
                                  const std::wstring_view& driver_names,
                                  const std::wstring_view& driver_versions) const;

  virtual void LogAutoEpSelection(uint32_t session_id, const std::string& selection_policy,
                                  const std::vector<std::string>& requested_execution_provider_ids,
                                  const std::vector<std::string>& available_execution_provider_ids) const;

  virtual void LogProviderOptions(const std::string& provider_id,
                                  const std::string& provider_options_string,
                                  bool captureState) const;

 private:
  ORT_DISALLOW_COPY_ASSIGNMENT_AND_MOVE(Telemetry);
};

}  // namespace onnxruntime
