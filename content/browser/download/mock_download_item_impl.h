// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_DOWNLOAD_MOCK_DOWNLOAD_ITEM_IMPL_H_
#define CONTENT_BROWSER_DOWNLOAD_MOCK_DOWNLOAD_ITEM_IMPL_H_

#include <memory>
#include <string>
#include <vector>

#include "base/macros.h"
#include "components/download/public/common/download_create_info.h"
#include "components/download/public/common/download_file.h"
#include "components/download/public/common/download_request_handle_interface.h"
#include "content/browser/download/download_item_impl.h"
#include "testing/gmock/include/gmock/gmock.h"

namespace content {

class DownloadManager;

class MockDownloadItemImpl : public DownloadItemImpl {
 public:
  // Use history constructor for minimal base object.
  explicit MockDownloadItemImpl(DownloadItemImplDelegate* delegate);
  ~MockDownloadItemImpl() override;

  MOCK_METHOD5(OnDownloadTargetDetermined,
               void(const base::FilePath&,
                    TargetDisposition,
                    download::DownloadDangerType,
                    const base::FilePath&,
                    download::DownloadInterruptReason));
  MOCK_METHOD1(AddObserver, void(download::DownloadItem::Observer*));
  MOCK_METHOD1(RemoveObserver, void(download::DownloadItem::Observer*));
  MOCK_METHOD0(UpdateObservers, void());
  MOCK_METHOD0(CanShowInFolder, bool());
  MOCK_METHOD0(CanOpenDownload, bool());
  MOCK_METHOD0(ShouldOpenFileBasedOnExtension, bool());
  MOCK_METHOD0(OpenDownload, void());
  MOCK_METHOD0(ShowDownloadInShell, void());
  MOCK_METHOD0(ValidateDangerousDownload, void());
  MOCK_METHOD2(StealDangerousDownload, void(bool, const AcquireFileCallback&));
  MOCK_METHOD3(UpdateProgress, void(int64_t, int64_t, const std::string&));
  MOCK_METHOD1(Cancel, void(bool));
  MOCK_METHOD0(MarkAsComplete, void());
  void OnAllDataSaved(int64_t, std::unique_ptr<crypto::SecureHash>) override {
    NOTREACHED();
  }
  MOCK_METHOD0(OnDownloadedFileRemoved, void());
  void Start(
      std::unique_ptr<download::DownloadFile> download_file,
      std::unique_ptr<download::DownloadRequestHandleInterface> req_handle,
      const download::DownloadCreateInfo& create_info) override {
    MockStart(download_file.get(), req_handle.get());
  }

  MOCK_METHOD2(MockStart,
               void(download::DownloadFile*,
                    download::DownloadRequestHandleInterface*));

  MOCK_METHOD0(Remove, void());
  MOCK_CONST_METHOD1(TimeRemaining, bool(base::TimeDelta*));
  MOCK_CONST_METHOD0(CurrentSpeed, int64_t());
  MOCK_CONST_METHOD0(PercentComplete, int());
  MOCK_CONST_METHOD0(AllDataSaved, bool());
  MOCK_CONST_METHOD1(MatchesQuery, bool(const base::string16& query));
  MOCK_CONST_METHOD0(IsDone, bool());
  MOCK_CONST_METHOD0(GetFullPath, const base::FilePath&());
  MOCK_CONST_METHOD0(GetTargetFilePath, const base::FilePath&());
  MOCK_CONST_METHOD0(GetTargetDisposition, TargetDisposition());
  MOCK_METHOD2(OnContentCheckCompleted,
               void(download::DownloadDangerType,
                    download::DownloadInterruptReason));
  MOCK_CONST_METHOD0(GetState, DownloadState());
  MOCK_CONST_METHOD0(GetUrlChain, const std::vector<GURL>&());
  MOCK_METHOD1(SetTotalBytes, void(int64_t));
  MOCK_CONST_METHOD0(GetURL, const GURL&());
  MOCK_CONST_METHOD0(GetOriginalUrl, const GURL&());
  MOCK_CONST_METHOD0(GetReferrerUrl, const GURL&());
  MOCK_CONST_METHOD0(GetTabUrl, const GURL&());
  MOCK_CONST_METHOD0(GetTabReferrerUrl, const GURL&());
  MOCK_CONST_METHOD0(GetSuggestedFilename, std::string());
  MOCK_CONST_METHOD0(GetContentDisposition, std::string());
  MOCK_CONST_METHOD0(GetMimeType, std::string());
  MOCK_CONST_METHOD0(GetOriginalMimeType, std::string());
  MOCK_CONST_METHOD0(GetReferrerCharset, std::string());
  MOCK_CONST_METHOD0(GetRemoteAddress, std::string());
  MOCK_CONST_METHOD0(GetTotalBytes, int64_t());
  MOCK_CONST_METHOD0(GetReceivedBytes, int64_t());
  MOCK_CONST_METHOD0(GetReceivedSlices, const std::vector<ReceivedSlice>&());
  MOCK_CONST_METHOD0(GetHashState, const std::string&());
  MOCK_CONST_METHOD0(GetHash, const std::string&());
  MOCK_CONST_METHOD0(GetId, uint32_t());
  MOCK_CONST_METHOD0(GetGuid, const std::string&());
  MOCK_CONST_METHOD0(GetStartTime, base::Time());
  MOCK_CONST_METHOD0(GetEndTime, base::Time());
  MOCK_METHOD0(GetDownloadManager, DownloadManager*());
  MOCK_CONST_METHOD0(IsPaused, bool());
  MOCK_CONST_METHOD0(GetOpenWhenComplete, bool());
  MOCK_METHOD1(SetOpenWhenComplete, void(bool));
  MOCK_CONST_METHOD0(GetFileExternallyRemoved, bool());
  MOCK_CONST_METHOD0(GetDangerType, download::DownloadDangerType());
  MOCK_CONST_METHOD0(IsDangerous, bool());
  MOCK_METHOD0(GetAutoOpened, bool());
  MOCK_CONST_METHOD0(GetForcedFilePath, const base::FilePath&());
  MOCK_CONST_METHOD0(HasUserGesture, bool());
  MOCK_CONST_METHOD0(GetTransitionType, ui::PageTransition());
  MOCK_CONST_METHOD0(IsTemporary, bool());
  MOCK_METHOD1(SetOpened, void(bool));
  MOCK_CONST_METHOD0(GetOpened, bool());
  MOCK_CONST_METHOD0(GetLastAccessTime, base::Time());
  MOCK_CONST_METHOD0(GetLastModifiedTime, const std::string&());
  MOCK_CONST_METHOD0(GetETag, const std::string&());
  MOCK_CONST_METHOD0(GetLastReason, download::DownloadInterruptReason());
  MOCK_CONST_METHOD0(GetFileNameToReportUser, base::FilePath());
  MOCK_METHOD1(SetDisplayName, void(const base::FilePath&));
  // May be called when vlog is on.
  std::string DebugString(bool verbose) const override { return std::string(); }
};
}  // namespace content

#endif  // CONTENT_BROWSER_DOWNLOAD_MOCK_DOWNLOAD_ITEM_IMPL_H_
