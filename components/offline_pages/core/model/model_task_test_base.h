// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_OFFLINE_PAGES_CORE_MODEL_MODEL_TASK_TEST_BASE_H_
#define COMPONENTS_OFFLINE_PAGES_CORE_MODEL_MODEL_TASK_TEST_BASE_H_

#include "base/files/scoped_temp_dir.h"
#include "base/memory/weak_ptr.h"
#include "base/test/test_mock_time_task_runner.h"
#include "base/threading/thread_task_runner_handle.h"
#include "components/offline_pages/core/client_policy_controller.h"
#include "components/offline_pages/core/model/offline_page_item_generator.h"
#include "components/offline_pages/core/offline_page_item.h"
#include "components/offline_pages/core/offline_page_metadata_store_test_util.h"
#include "components/offline_pages/core/offline_page_types.h"
#include "components/offline_pages/core/offline_store_types.h"
#include "components/offline_pages/core/task_test_base.h"

namespace offline_pages {
class ModelTaskTestBase : public TaskTestBase,
                          public base::SupportsWeakPtr<ModelTaskTestBase> {
 public:
  ModelTaskTestBase();
  ~ModelTaskTestBase() override;

  void SetUp() override;
  void TearDown() override;

  const base::FilePath& TemporaryDir();

  // Calls generator()->CreateItemWithTempFile() and inserts the item into the
  // database.
  OfflinePageItem AddPage();

  // Calls generator()->CreateItem() and inserts the item into the database.
  OfflinePageItem AddPageWithoutFile();

  OfflinePageMetadataStoreTestUtil* store_test_util() {
    return &store_test_util_;
  }
  OfflinePageMetadataStoreSQL* store() { return store_test_util_.store(); }
  OfflinePageItemGenerator* generator() { return &generator_; }
  ClientPolicyController* policy_controller() { return &policy_controller_; }

 private:
  OfflinePageMetadataStoreTestUtil store_test_util_;
  OfflinePageItemGenerator generator_;
  base::ScopedTempDir temporary_dir_;
  ClientPolicyController policy_controller_;
};

}  // namespace offline_pages

#endif  // COMPONENTS_OFFLINE_PAGES_CORE_MODEL_MODEL_TASK_TEST_BASE_H_
