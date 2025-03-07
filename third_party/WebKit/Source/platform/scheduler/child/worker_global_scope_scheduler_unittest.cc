// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/scheduler/child/worker_global_scope_scheduler.h"

#include <memory>
#include "base/macros.h"
#include "base/memory/ptr_util.h"
#include "base/test/simple_test_tick_clock.h"
#include "base/test/test_simple_task_runner.h"
#include "platform/scheduler/child/worker_scheduler_impl.h"
#include "platform/scheduler/test/task_queue_manager_for_test.h"
#include "platform/wtf/Functional.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

using ::testing::ElementsAreArray;

namespace blink {
namespace scheduler {
// To avoid symbol collisions in jumbo builds.
namespace worker_global_scope_scheduler_unittest {

void AppendToVectorTestTask(std::vector<std::string>* vector,
                            std::string value) {
  vector->push_back(value);
}

class WorkerGlobalScopeSchedulerTest : public ::testing::Test {
 public:
  WorkerGlobalScopeSchedulerTest()
      : mock_task_runner_(new base::TestSimpleTaskRunner()),
        scheduler_(new WorkerSchedulerImpl(
            TaskQueueManagerForTest::Create(nullptr,
                                            mock_task_runner_,
                                            &clock_),
            nullptr /* proxy */)) {
    clock_.Advance(base::TimeDelta::FromMicroseconds(5000));
  }

  ~WorkerGlobalScopeSchedulerTest() override = default;

  void SetUp() override {
    scheduler_->Init();
    global_scope_scheduler_ =
        std::make_unique<WorkerGlobalScopeScheduler>(scheduler_.get());
  }

  void RunUntilIdle() { mock_task_runner_->RunUntilIdle(); }

  // Helper for posting a task.
  void PostTestTask(std::vector<std::string>* run_order,
                    const std::string& task_descriptor) {
    global_scope_scheduler_->GetTaskRunner(TaskType::kInternalTest)
        ->PostTask(FROM_HERE,
                   WTF::Bind(&AppendToVectorTestTask,
                             WTF::Unretained(run_order), task_descriptor));
  }

 protected:
  base::SimpleTestTickClock clock_;
  scoped_refptr<base::TestSimpleTaskRunner> mock_task_runner_;

  std::unique_ptr<WorkerSchedulerImpl> scheduler_;
  std::unique_ptr<WorkerGlobalScopeScheduler> global_scope_scheduler_;

  DISALLOW_COPY_AND_ASSIGN(WorkerGlobalScopeSchedulerTest);
};

TEST_F(WorkerGlobalScopeSchedulerTest, TestPostTasks) {
  std::vector<std::string> run_order;
  PostTestTask(&run_order, "T1");
  PostTestTask(&run_order, "T2");
  RunUntilIdle();
  PostTestTask(&run_order, "T3");
  RunUntilIdle();
  EXPECT_THAT(run_order, ::testing::ElementsAre("T1", "T2", "T3"));

  // Tasks should not run after the scheduler is disposed of.
  global_scope_scheduler_->Dispose();
  run_order.clear();
  PostTestTask(&run_order, "T4");
  PostTestTask(&run_order, "T5");
  RunUntilIdle();
  EXPECT_TRUE(run_order.empty());
}

}  // namespace worker_global_scope_scheduler_unittest
}  // namespace scheduler
}  // namespace blink
