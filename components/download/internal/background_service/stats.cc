// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/download/internal/background_service/stats.h"

#include <map>

#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/metrics/histogram_functions.h"
#include "base/metrics/histogram_macros.h"
#include "components/download/internal/background_service/startup_status.h"

namespace download {
namespace stats {
namespace {

// The maximum tracked file size in KB, larger files will fall into overflow
// bucket.
const int64_t kMaxFileSizeKB = 4 * 1024 * 1024; /* 4GB */

// Enum used by UMA metrics to track various reasons of pausing a download.
enum class PauseReason {
  // The download was paused. The reason can be anything.
  ANY = 0,

  // The download was paused due to unsatisfied device criteria.
  UNMET_DEVICE_CRITERIA = 1,

  // The download was paused by client.
  PAUSE_BY_CLIENT = 2,

  // The download was paused due to external download.
  EXTERNAL_DOWNLOAD = 3,

  // The download was paused due to navigation.
  EXTERNAL_NAVIGATION = 4,

  // The count of entries for the enum.
  COUNT = 5,
};

// Converts DownloadTaskType to histogram suffix.
// Should maps to suffix string in histograms.xml.
std::string TaskTypeToHistogramSuffix(DownloadTaskType task_type) {
  switch (task_type) {
    case DownloadTaskType::DOWNLOAD_TASK:
      return "DownloadTask";
    case DownloadTaskType::CLEANUP_TASK:
      return "CleanUpTask";
  }
  NOTREACHED();
  return std::string();
}

// Converts Entry::State to histogram suffix.
// Should maps to suffix string in histograms.xml.
std::string EntryStateToHistogramSuffix(Entry::State state) {
  std::string suffix;
  switch (state) {
    case Entry::State::NEW:
      return "New";
    case Entry::State::AVAILABLE:
      return "Available";
    case Entry::State::ACTIVE:
      return "Active";
    case Entry::State::PAUSED:
      return "Paused";
    case Entry::State::COMPLETE:
      return "Complete";
    case Entry::State::COUNT:
      break;
  }
  NOTREACHED();
  return std::string();
}

// Converts DownloadClient to histogram suffix.
// Should maps to suffix string in histograms.xml.
std::string ClientToHistogramSuffix(DownloadClient client) {
  switch (client) {
    case DownloadClient::TEST:
    case DownloadClient::TEST_2:
    case DownloadClient::TEST_3:
    case DownloadClient::INVALID:
      return "__Test__";
    case DownloadClient::OFFLINE_PAGE_PREFETCH:
      return "OfflinePage";
    case DownloadClient::BACKGROUND_FETCH:
      return "BackgroundFetch";
    case DownloadClient::DEBUGGING:
      return "Debugging";
    case DownloadClient::BOUNDARY:
      NOTREACHED();
      break;
  }
  NOTREACHED();
  return std::string();
}

// Converts CompletionType to histogram suffix.
// Should maps to suffix string in histograms.xml.
std::string CompletionTypeToHistogramSuffix(CompletionType type) {
  switch (type) {
    case CompletionType::SUCCEED:
      return "Succeed";
    case CompletionType::FAIL:
      return "Fail";
    case CompletionType::ABORT:
      return "Abort";
    case CompletionType::TIMEOUT:
      return "Timeout";
    case CompletionType::UNKNOWN:
      return "Unknown";
    case CompletionType::CANCEL:
      return "Cancel";
    case CompletionType::OUT_OF_RETRIES:
      return "OutOfRetries";
    case CompletionType::OUT_OF_RESUMPTIONS:
      return "OutOfResumptions";
    case CompletionType::COUNT:
      NOTREACHED();
  }
  NOTREACHED();
  return std::string();
}

// Converts FileCleanupReason to histogram suffix.
// Should maps to suffix string in histograms.xml.
std::string FileCleanupReasonToHistogramSuffix(FileCleanupReason reason) {
  switch (reason) {
    case FileCleanupReason::TIMEOUT:
      return "Timeout";
    case FileCleanupReason::ORPHANED:
      return "Orphaned";
    case FileCleanupReason::UNKNOWN:
      return "Unknown";
    case FileCleanupReason::HARD_RECOVERY:
      return "HardRecovery";
    case FileCleanupReason::COUNT:
      NOTREACHED();
  }
  NOTREACHED();
  return std::string();
}

// Helper method to log StartUpResult.
void LogStartUpResult(bool in_recovery, StartUpResult result) {
  if (in_recovery) {
    base::UmaHistogramEnumeration("Download.Service.StartUpStatus.Recovery",
                                  result, StartUpResult::COUNT);
  } else {
    base::UmaHistogramEnumeration(
        "Download.Service.StartUpStatus.Initialization", result,
        StartUpResult::COUNT);
  }
}

// Helper method to log the number of entries under a particular state.
void LogDatabaseRecords(Entry::State state, uint32_t record_count) {
  std::string name("Download.Service.Db.Records");
  name.append(".").append(EntryStateToHistogramSuffix(state));
  base::UmaHistogramCustomCounts(name, record_count, 1, 500, 50);
}

// Helper method to log the pause reason for a particular download.
void LogDownloadPauseReason(PauseReason reason) {
  UMA_HISTOGRAM_ENUMERATION("Download.Service.PauseReason", reason,
                            PauseReason::COUNT);
}

}  // namespace

void LogControllerStartupStatus(bool in_recovery, const StartupStatus& status) {
  DCHECK(status.Complete());

  // Total counts for general success/failure rate.
  LogStartUpResult(in_recovery, status.Ok() ? StartUpResult::SUCCESS
                                            : StartUpResult::FAILURE);

  // Failure reasons.
  if (!status.driver_ok.value())
    LogStartUpResult(in_recovery, StartUpResult::FAILURE_REASON_DRIVER);
  if (!status.model_ok.value())
    LogStartUpResult(in_recovery, StartUpResult::FAILURE_REASON_MODEL);
  if (!status.file_monitor_ok.value())
    LogStartUpResult(in_recovery, StartUpResult::FAILURE_REASON_FILE_MONITOR);
}

void LogServiceApiAction(DownloadClient client, ServiceApiAction action) {
  // Total count for each action.
  std::string name("Download.Service.Request.ClientAction");
  base::UmaHistogramEnumeration(name, action, ServiceApiAction::COUNT);

  // Total count for each action with client suffix.
  name.append(".").append(ClientToHistogramSuffix(client));
  base::UmaHistogramEnumeration(name, action, ServiceApiAction::COUNT);
}

void LogStartDownloadResult(DownloadClient client,
                            DownloadParams::StartResult result) {
  // Total count for each start result.
  std::string name("Download.Service.Request.StartResult");
  base::UmaHistogramEnumeration(name, result,
                                DownloadParams::StartResult::COUNT);

  // Total count for each client result with client suffix.
  name.append(".").append(ClientToHistogramSuffix(client));
  base::UmaHistogramEnumeration(name, result,
                                DownloadParams::StartResult::COUNT);
}

void LogStartDownloadResponse(DownloadClient client,
                              Client::ShouldDownload should_download) {
  // Total count for each start response.
  std::string name("Download.Service.Request.StartResponse");
  base::UmaHistogramEnumeration(name, should_download,
                                Client::ShouldDownload::COUNT);

  // Total count for each client response with client suffix.
  name.append(".").append(ClientToHistogramSuffix(client));
  base::UmaHistogramEnumeration(name, should_download,
                                Client::ShouldDownload::COUNT);
}

void LogDownloadParams(const DownloadParams& params) {
  UMA_HISTOGRAM_ENUMERATION("Download.Service.Request.BatteryRequirement",
                            params.scheduling_params.battery_requirements,
                            SchedulingParams::BatteryRequirements::COUNT);
  UMA_HISTOGRAM_ENUMERATION("Download.Service.Request.NetworkRequirement",
                            params.scheduling_params.network_requirements,
                            SchedulingParams::NetworkRequirements::COUNT);
  UMA_HISTOGRAM_ENUMERATION("Download.Service.Request.Priority",
                            params.scheduling_params.priority,
                            SchedulingParams::Priority::COUNT);
}

void LogRecoveryOperation(Entry::State to_state) {
  UMA_HISTOGRAM_ENUMERATION("Download.Service.Recovery", to_state,
                            Entry::State::COUNT);
}

void LogDownloadCompletion(CompletionType type, uint64_t file_size_bytes) {
  // Records completion type.
  UMA_HISTOGRAM_ENUMERATION("Download.Service.Finish.Type", type,
                            CompletionType::COUNT);

  // TODO(xingliu): Use DownloadItem::GetStartTime and DownloadItem::GetEndTime
  // to record the completion time to histogram "Download.Service.Finish.Time".
  // Also propagates and records the mime type here.

  // Records the file size.
  std::string name("Download.Service.Finish.FileSize");
  uint64_t file_size_kb = file_size_bytes / 1024;
  base::UmaHistogramCustomCounts(name, file_size_kb, 1, kMaxFileSizeKB, 50);

  name.append(".").append(CompletionTypeToHistogramSuffix(type));
  base::UmaHistogramCustomCounts(name, file_size_kb, 1, kMaxFileSizeKB, 50);
}

void LogDownloadPauseReason(bool unmet_device_criteria,
                            bool pause_by_client,
                            bool external_navigation,
                            bool external_download) {
  LogDownloadPauseReason(PauseReason::ANY);

  if (unmet_device_criteria)
    LogDownloadPauseReason(PauseReason::UNMET_DEVICE_CRITERIA);

  if (pause_by_client)
    LogDownloadPauseReason(PauseReason::PAUSE_BY_CLIENT);

  if (external_navigation)
    LogDownloadPauseReason(PauseReason::EXTERNAL_NAVIGATION);

  if (external_download)
    LogDownloadPauseReason(PauseReason::EXTERNAL_DOWNLOAD);
}

void LogModelOperationResult(ModelAction action, bool success) {
  if (success) {
    UMA_HISTOGRAM_ENUMERATION("Download.Service.Db.Operation.Success", action,
                              ModelAction::COUNT);
  } else {
    UMA_HISTOGRAM_ENUMERATION("Download.Service.Db.Operation.Failure", action,
                              ModelAction::COUNT);
  }
}

void LogEntries(std::map<Entry::State, uint32_t>& entries_count) {
  uint32_t total_records = 0;
  for (const auto& entry_count : entries_count)
    total_records += entry_count.second;

  // Total number of records in database.
  base::UmaHistogramCustomCounts("Download.Service.Db.Records", total_records,
                                 1, 500, 50);

  // Number of records for each Entry::State.
  for (Entry::State state = Entry::State::NEW; state != Entry::State::COUNT;
       state = (Entry::State)((int)(state) + 1)) {
    LogDatabaseRecords(state, entries_count[state]);
  }
}

void LogScheduledTaskStatus(DownloadTaskType task_type,
                            ScheduledTaskStatus status) {
  std::string name("Download.Service.TaskScheduler.Status");
  base::UmaHistogramEnumeration(name, status, ScheduledTaskStatus::COUNT);

  name.append(".").append(TaskTypeToHistogramSuffix(task_type));
  base::UmaHistogramEnumeration(name, status, ScheduledTaskStatus::COUNT);
}

void LogsFileDirectoryCreationError(base::File::Error error) {
  // Maps to histogram enum PlatformFileError.
  UMA_HISTOGRAM_ENUMERATION("Download.Service.Files.DirCreationError", -error,
                            -base::File::Error::FILE_ERROR_MAX);
}

void LogFileCleanupStatus(FileCleanupReason reason,
                          int succeeded_cleanups,
                          int failed_cleanups,
                          int external_cleanups) {
  std::string name("Download.Service.Files.CleanUp.Success");
  base::UmaHistogramCounts100(name, succeeded_cleanups);
  name.append(".").append(FileCleanupReasonToHistogramSuffix(reason));
  base::UmaHistogramCounts100(name, succeeded_cleanups);

  name = "Download.Service.Files.CleanUp.Failure";
  base::UmaHistogramCounts100(name, failed_cleanups);
  name.append(".").append(FileCleanupReasonToHistogramSuffix(reason));
  base::UmaHistogramCounts100(name, failed_cleanups);

  name = "Download.Service.Files.CleanUp.External";
  base::UmaHistogramCounts100(name, external_cleanups);
  name.append(".").append(FileCleanupReasonToHistogramSuffix(reason));
  base::UmaHistogramCounts100(name, external_cleanups);
}

void LogFileLifeTime(const base::TimeDelta& file_life_time,
                     int num_cleanup_attempts) {
  UMA_HISTOGRAM_CUSTOM_TIMES("Download.Service.Files.LifeTime", file_life_time,
                             base::TimeDelta::FromSeconds(1),
                             base::TimeDelta::FromDays(8), 100);
  base::UmaHistogramSparse("Download.Service.Files.Cleanup.Attempts",
                           num_cleanup_attempts);
}

void LogFileDirDiskUtilization(int64_t total_disk_space,
                               int64_t free_disk_space,
                               int64_t files_size) {
  UMA_HISTOGRAM_PERCENTAGE("Download.Service.Files.FreeDiskSpace",
                           (free_disk_space * 100) / total_disk_space);
  UMA_HISTOGRAM_PERCENTAGE("Download.Service.Files.DiskUsed",
                           (files_size * 100) / total_disk_space);
}

void LogFilePathRenamed(bool renamed) {
  UMA_HISTOGRAM_BOOLEAN("Download.Service.Files.PathRenamed", renamed);
}

void LogEntryEvent(DownloadEvent event) {
  UMA_HISTOGRAM_ENUMERATION("Download.Service.Entry.Event", event,
                            DownloadEvent::COUNT);
}

void LogEntryResumptionCount(uint32_t resume_count) {
  UMA_HISTOGRAM_COUNTS_100("Download.Service.Entry.ResumptionCount",
                           resume_count);
}

void LogEntryRetryCount(uint32_t retry_count) {
  UMA_HISTOGRAM_COUNTS_100("Download.Service.Entry.RetryCount", retry_count);
}

}  // namespace stats
}  // namespace download
