// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_OFFLINE_PAGES_CORE_MODEL_OFFLINE_PAGE_MODEL_TASKIFIED_H_
#define COMPONENTS_OFFLINE_PAGES_CORE_MODEL_OFFLINE_PAGE_MODEL_TASKIFIED_H_

#include <stdint.h>

#include <utility>

#include "base/callback.h"
#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/time/clock.h"
#include "components/keyed_service/core/keyed_service.h"
#include "components/offline_pages/core/model/clear_storage_task.h"
#include "components/offline_pages/core/offline_page_archiver.h"
#include "components/offline_pages/core/offline_page_model.h"
#include "components/offline_pages/core/offline_page_model_event_logger.h"
#include "components/offline_pages/core/offline_page_types.h"
#include "components/offline_pages/core/offline_store_types.h"
#include "components/offline_pages/core/task_queue.h"

class GURL;
namespace base {
class FilePath;
class SequencedTaskRunner;
}  // namespace base

namespace offline_pages {

struct ClientId;
struct OfflinePageItem;

class ArchiveManager;
class ClientPolicyController;
class OfflinePageArchiver;
class OfflinePageMetadataStoreSQL;
class SystemDownloadManager;

// Implementaion of OfflinePageModel, which is a service for saving pages
// offline. It's an entry point to get information about Offline Pages and the
// base of related Offline Pages features.
// It owns a database which stores offline metadata, and uses TaskQueue for
// executing various tasks, including database operation or other process that
// needs to run on a background thread.
class OfflinePageModelTaskified : public OfflinePageModel,
                                  public TaskQueue::Delegate {
 public:
  // Initial delay after which a list of items for upgrade will be generated.
  static constexpr base::TimeDelta kInitialUpgradeSelectionDelay =
      base::TimeDelta::FromSeconds(45);

  // Delay between the scheduling and actual running of maintenance tasks. To
  // not cause the re-opening of the metadata store this delay should be kept
  // smaller than OfflinePageMetadataStoreSQL::kClosingDelay.
  static constexpr base::TimeDelta kMaintenanceTasksDelay =
      base::TimeDelta::FromSeconds(10);

  // Minimum delay between runs of maintenance tasks during a Chrome session.
  static constexpr base::TimeDelta kClearStorageInterval =
      base::TimeDelta::FromMinutes(30);

  OfflinePageModelTaskified(
      std::unique_ptr<OfflinePageMetadataStoreSQL> store,
      std::unique_ptr<ArchiveManager> archive_manager,
      std::unique_ptr<SystemDownloadManager> download_manager,
      const scoped_refptr<base::SequencedTaskRunner>& task_runner,
      std::unique_ptr<base::Clock> clock);
  ~OfflinePageModelTaskified() override;

  // TaskQueue::Delegate implementation.
  void OnTaskQueueIsIdle() override;

  // OfflinePageModel implementation.
  void AddObserver(Observer* observer) override;
  void RemoveObserver(Observer* observer) override;

  void SavePage(const SavePageParams& save_page_params,
                std::unique_ptr<OfflinePageArchiver> archiver,
                const SavePageCallback& callback) override;
  void AddPage(const OfflinePageItem& page,
               const AddPageCallback& callback) override;
  void MarkPageAccessed(int64_t offline_id) override;

  void DeletePagesByOfflineId(const std::vector<int64_t>& offline_ids,
                              const DeletePageCallback& callback) override;
  void DeletePagesByClientIds(const std::vector<ClientId>& client_ids,
                              const DeletePageCallback& callback) override;
  void DeletePagesByClientIdsAndOrigin(
      const std::vector<ClientId>& client_ids,
      const std::string& origin,
      const DeletePageCallback& callback) override;
  void DeleteCachedPagesByURLPredicate(
      const UrlPredicate& predicate,
      const DeletePageCallback& callback) override;

  void GetAllPages(const MultipleOfflinePageItemCallback& callback) override;
  void GetPageByOfflineId(
      int64_t offline_id,
      const SingleOfflinePageItemCallback& callback) override;
  void GetPagesByClientIds(
      const std::vector<ClientId>& client_ids,
      const MultipleOfflinePageItemCallback& callback) override;
  void GetPagesByURL(const GURL& url,
                     URLSearchMode url_search_mode,
                     const MultipleOfflinePageItemCallback& callback) override;
  void GetPagesByNamespace(
      const std::string& name_space,
      const MultipleOfflinePageItemCallback& callback) override;
  // Get all pages in the namespaces that will be removed on cache reset.
  void GetPagesRemovedOnCacheReset(
      const MultipleOfflinePageItemCallback& callback) override;
  // Get all pages in the namespaces that are shown in download ui.
  void GetPagesSupportedByDownloads(
      const MultipleOfflinePageItemCallback& callback) override;
  void GetPagesByRequestOrigin(
      const std::string& request_origin,
      const MultipleOfflinePageItemCallback& callback) override;
  void GetPageBySizeAndDigest(
      int64_t file_size,
      const std::string& digest,
      const SingleOfflinePageItemCallback& callback) override;

  void GetOfflineIdsForClientId(
      const ClientId& client_id,
      const MultipleOfflineIdCallback& callback) override;

  const base::FilePath& GetInternalArchiveDirectory(
      const std::string& name_space) const override;
  bool IsArchiveInInternalDir(const base::FilePath& file_path) const override;

  ClientPolicyController* GetPolicyController() override;

  OfflineEventLogger* GetLogger() override;

  // Methods for testing only:
  OfflinePageMetadataStoreSQL* GetStoreForTesting() { return store_.get(); }
  void SetClockForTesting(std::unique_ptr<base::Clock> clock) {
    clock_ = std::move(clock);
  }
  void SetSkipClearingOriginalUrlForTesting() {
    skip_clearing_original_url_for_testing_ = true;
  }
  void DoNotRunMaintenanceTasksForTesting() {
    skip_maintenance_tasks_for_testing_ = true;
  }

 private:
  // TODO(romax): https://crbug.com/791115, remove the friend class usage.
  friend class OfflinePageModelTaskifiedTest;

  // Callbacks for saving pages.
  void InformSavePageDone(const SavePageCallback& calback,
                          SavePageResult result,
                          const ClientId& client_id,
                          int64_t offline_id);
  void OnAddPageForSavePageDone(const SavePageCallback& callback,
                                const OfflinePageItem& page_attempted,
                                AddPageResult add_page_result,
                                int64_t offline_id);
  void OnCreateArchiveDone(const SavePageParams& save_page_params,
                           int64_t offline_id,
                           const base::Time& start_time,
                           const SavePageCallback& callback,
                           OfflinePageArchiver* archiver,
                           OfflinePageArchiver::ArchiverResult archiver_result,
                           const GURL& saved_url,
                           const base::FilePath& file_path,
                           const base::string16& title,
                           int64_t file_size,
                           const std::string& digest);

  // Callback for adding pages.
  void OnAddPageDone(const OfflinePageItem& page,
                     const AddPageCallback& callback,
                     AddPageResult result);

  // Callbacks for deleting pages.
  void OnDeleteDone(
      const DeletePageCallback& callback,
      DeletePageResult result,
      const std::vector<OfflinePageModel::DeletedPageInfo>& infos);

  // Methods for clearing temporary pages and performing consistency checks. The
  // latter are executed only once per Chrome session.
  void ScheduleMaintenanceTasks();
  void RunMaintenanceTasks(const base::Time now, bool first_run);
  void OnClearCachedPagesDone(size_t deleted_page_count,
                              ClearStorageTask::ClearStorageResult result);

  // Method for upgrade to public storage.
  void PostSelectItemsMarkedForUpgrade();
  void SelectItemsMarkedForUpgrade();
  void OnSelectItemsMarkedForUpgradeDone(
      const MultipleOfflinePageItemResult& pages_for_upgrade);

  // Methods for publishing the page to the public directory.
  void PublishArchive(const OfflinePageItem& offline_page,
                      const SavePageCallback& callback,
                      OfflinePageArchiver* archiver);

  // Callback for when PublishArchive has completd.
  void PublishArchiveDone(const SavePageCallback& save_page_callback,
                          const OfflinePageItem& offline_page,
                          PublishArchiveResult* archive_result);

  // Method for unpublishing the page from the system download manager.
  static void RemoveFromDownloadManager(
      SystemDownloadManager* download_manager,
      const std::vector<int64_t>& system_download_ids);

  // Other utility methods.
  void RemovePagesMatchingUrlAndNamespace(const OfflinePageItem& page);
  void ErasePendingArchiver(OfflinePageArchiver* archiver);
  void CreateArchivesDirectoryIfNeeded();
  base::Time GetCurrentTime();

  // Persistent store for offline page metadata.
  std::unique_ptr<OfflinePageMetadataStoreSQL> store_;

  // Manager for the offline archive files and directory.
  std::unique_ptr<ArchiveManager> archive_manager_;

  // Manages interaction with the OS download manager, if present.
  std::unique_ptr<SystemDownloadManager> download_manager_;

  // Controller of the client policies.
  std::unique_ptr<ClientPolicyController> policy_controller_;

  // The observers.
  base::ObserverList<Observer> observers_;

  // Pending archivers owned by this model.
  // This is above the definition of |task_queue_|. Since the queue may hold raw
  // pointers to the pending archivers, and the pending archivers are better
  // destructed after the |task_queue_|.
  std::vector<std::unique_ptr<OfflinePageArchiver>> pending_archivers_;

  // Clock for testing only.
  std::unique_ptr<base::Clock> clock_;

  // Logger to facilitate recording of events.
  OfflinePageModelEventLogger offline_event_logger_;

  // The task queue used for executing various tasks.
  TaskQueue task_queue_;

  // The last scheduling timestamp of the model maintenance tasks that took
  // place during the current Chrome session.
  base::Time last_maintenance_tasks_schedule_time_;

  // For testing only.
  // This value will be affecting the CreateArchiveTasks that are created by the
  // model to skip saving original_urls.
  bool skip_clearing_original_url_for_testing_;

  // For testing only.
  // This flag controls the execution of maintenance tasks; when false they will
  // not be executed.
  bool skip_maintenance_tasks_for_testing_;

  const scoped_refptr<base::SequencedTaskRunner> task_runner_;

  base::WeakPtrFactory<OfflinePageModelTaskified> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(OfflinePageModelTaskified);
};

}  // namespace offline_pages

#endif  // COMPONENTS_OFFLINE_PAGES_CORE_MODEL_OFFLINE_PAGE_MODEL_TASKIFIED_H_
