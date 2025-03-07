// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/media_galleries/chromeos/mtp_read_file_worker.h"

#include <utility>

#include "base/bind.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/memory/ptr_util.h"
#include "base/numerics/safe_conversions.h"
#include "base/task_scheduler/post_task.h"
#include "base/task_scheduler/task_traits.h"
#include "base/threading/thread_restrictions.h"
#include "chrome/browser/media_galleries/chromeos/snapshot_file_details.h"
#include "components/storage_monitor/storage_monitor.h"
#include "content/public/browser/browser_thread.h"
#include "device/media_transfer_protocol/media_transfer_protocol_manager.h"
#include "third_party/cros_system_api/dbus/service_constants.h"

using storage_monitor::StorageMonitor;

namespace {

// Appends |data| to the snapshot file specified by the |snapshot_file_path| on
// a background sequence that allows IO. Returns the number of bytes written to
// the snapshot file. In case of failure, returns zero.
uint32_t WriteDataChunkIntoSnapshotFileOnFileThread(
    const base::FilePath& snapshot_file_path,
    const std::string& data) {
  base::AssertBlockingAllowed();
  return base::AppendToFile(snapshot_file_path, data.c_str(), data.size())
             ? base::checked_cast<uint32_t>(data.size())
             : 0;
}

}  // namespace

MTPReadFileWorker::MTPReadFileWorker(const std::string& device_handle)
    : device_handle_(device_handle),
      weak_ptr_factory_(this) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK(!device_handle_.empty());
}

MTPReadFileWorker::~MTPReadFileWorker() {
}

void MTPReadFileWorker::WriteDataIntoSnapshotFile(
    const SnapshotRequestInfo& request_info,
    const base::File::Info& snapshot_file_info) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  ReadDataChunkFromDeviceFile(
      std::make_unique<SnapshotFileDetails>(request_info, snapshot_file_info));
}

void MTPReadFileWorker::ReadDataChunkFromDeviceFile(
    std::unique_ptr<SnapshotFileDetails> snapshot_file_details) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK(snapshot_file_details.get());

  // To avoid calling |snapshot_file_details| methods and passing ownership of
  // |snapshot_file_details| in the same_line.
  SnapshotFileDetails* snapshot_file_details_ptr = snapshot_file_details.get();

  device::MediaTransferProtocolManager* mtp_device_manager =
      StorageMonitor::GetInstance()->media_transfer_protocol_manager();
  mtp_device_manager->ReadFileChunk(
      device_handle_,
      snapshot_file_details_ptr->file_id(),
      snapshot_file_details_ptr->bytes_written(),
      snapshot_file_details_ptr->BytesToRead(),
      base::Bind(&MTPReadFileWorker::OnDidReadDataChunkFromDeviceFile,
                 weak_ptr_factory_.GetWeakPtr(),
                 base::Passed(&snapshot_file_details)));
}

void MTPReadFileWorker::OnDidReadDataChunkFromDeviceFile(
    std::unique_ptr<SnapshotFileDetails> snapshot_file_details,
    const std::string& data,
    bool error) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK(snapshot_file_details.get());
  snapshot_file_details->set_error_occurred(
      error || (data.size() != snapshot_file_details->BytesToRead()));
  if (snapshot_file_details->error_occurred()) {
    OnDidWriteIntoSnapshotFile(std::move(snapshot_file_details));
    return;
  }

  // To avoid calling |snapshot_file_details| methods and passing ownership of
  // |snapshot_file_details| in the same_line.
  SnapshotFileDetails* snapshot_file_details_ptr = snapshot_file_details.get();
  base::PostTaskWithTraitsAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::BACKGROUND},
      base::Bind(&WriteDataChunkIntoSnapshotFileOnFileThread,
                 snapshot_file_details_ptr->snapshot_file_path(), data),
      base::Bind(&MTPReadFileWorker::OnDidWriteDataChunkIntoSnapshotFile,
                 weak_ptr_factory_.GetWeakPtr(),
                 base::Passed(&snapshot_file_details)));
}

void MTPReadFileWorker::OnDidWriteDataChunkIntoSnapshotFile(
    std::unique_ptr<SnapshotFileDetails> snapshot_file_details,
    uint32_t bytes_written) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK(snapshot_file_details.get());
  if (snapshot_file_details->AddBytesWritten(bytes_written)) {
    if (!snapshot_file_details->IsSnapshotFileWriteComplete()) {
      ReadDataChunkFromDeviceFile(std::move(snapshot_file_details));
      return;
    }
  } else {
    snapshot_file_details->set_error_occurred(true);
  }
  OnDidWriteIntoSnapshotFile(std::move(snapshot_file_details));
}

void MTPReadFileWorker::OnDidWriteIntoSnapshotFile(
    std::unique_ptr<SnapshotFileDetails> snapshot_file_details) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK(snapshot_file_details.get());

  if (snapshot_file_details->error_occurred()) {
    content::BrowserThread::PostTask(
        content::BrowserThread::IO,
        FROM_HERE,
        base::Bind(snapshot_file_details->error_callback(),
                   base::File::FILE_ERROR_FAILED));
    return;
  }
  content::BrowserThread::PostTask(
      content::BrowserThread::IO,
      FROM_HERE,
      base::Bind(snapshot_file_details->success_callback(),
                 snapshot_file_details->file_info(),
                 snapshot_file_details->snapshot_file_path()));
}
