// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_WEBKIT_SOURCE_PLATFORM_SCHEDULER_CHILD_WEB_SCHEDULER_H_
#define THIRD_PARTY_WEBKIT_SOURCE_PLATFORM_SCHEDULER_CHILD_WEB_SCHEDULER_H_

#include <memory>
#include "base/location.h"
#include "base/single_thread_task_runner.h"
#include "base/time/time.h"
#include "platform/scheduler/renderer/page_scheduler.h"
#include "public/platform/WebThread.h"
#include "public/platform/scheduler/renderer/renderer_scheduler.h"

namespace blink {

// This class is used to submit tasks and pass other information from Blink to
// the platform's scheduler.
// TODO(skyostil): Replace this class with RendererScheduler.
class PLATFORM_EXPORT WebScheduler {
 public:
  using RendererPauseHandle = scheduler::RendererScheduler::RendererPauseHandle;

  virtual ~WebScheduler() = default;

  // Called to prevent any more pending tasks from running. Must be called on
  // the associated WebThread.
  virtual void Shutdown() = 0;

  // Returns true if there is high priority work pending on the associated
  // WebThread and the caller should yield to let the scheduler service that
  // work.  Must be called on the associated WebThread.
  virtual bool ShouldYieldForHighPriorityWork() = 0;

  // Returns true if a currently running idle task could exceed its deadline
  // without impacting user experience too much. This should only be used if
  // there is a task which cannot be pre-empted and is likely to take longer
  // than the largest expected idle task deadline. It should NOT be polled to
  // check whether more work can be performed on the current idle task after
  // its deadline has expired - post a new idle task for the continuation of
  // the work in this case.
  // Must be called from the associated WebThread.
  virtual bool CanExceedIdleDeadlineIfRequired() = 0;

  // Schedule an idle task to run the associated WebThread. For non-critical
  // tasks which may be reordered relative to other task types and may be
  // starved for an arbitrarily long time if no idle time is available.
  // Takes ownership of |IdleTask|. Can be called from any thread.
  virtual void PostIdleTask(const base::Location&, WebThread::IdleTask) = 0;

  // Like postIdleTask but guarantees that the posted task will not run
  // nested within an already-running task. Posting an idle task as
  // non-nestable may not affect when the task gets run, or it could
  // make it run later than it normally would, but it won't make it
  // run earlier than it normally would.
  virtual void PostNonNestableIdleTask(const base::Location&,
                                       WebThread::IdleTask) = 0;

  // Returns a task runner for kV8 tasks. Can be called from any thread.
  virtual base::SingleThreadTaskRunner* V8TaskRunner() = 0;

  // Returns a task runner for compositor tasks. This is intended only to be
  // used by specific animation and rendering related tasks (e.g. animated GIFS)
  // and should not generally be used.
  virtual base::SingleThreadTaskRunner* CompositorTaskRunner() = 0;

  // Creates a new PageScheduler for a given Page. Must be called from the
  // associated WebThread.
  virtual std::unique_ptr<PageScheduler> CreatePageScheduler(
      PageScheduler::Delegate*) = 0;

  // Pauses the scheduler. See RendererScheduler::PauseRenderer for details.
  // May only be called from the main thread.
  virtual std::unique_ptr<RendererPauseHandle> PauseScheduler()
      WARN_UNUSED_RESULT = 0;

  // Tells the scheduler that a navigation task is pending.
  // TODO(alexclarke): Long term should this be a task trait?
  virtual void AddPendingNavigation(
      scheduler::RendererScheduler::NavigatingFrameType) = 0;

  // Tells the scheduler that a navigation task is no longer pending.
  virtual void RemovePendingNavigation(
      scheduler::RendererScheduler::NavigatingFrameType) = 0;

  // Returns the current time recognized by the scheduler, which may perhaps
  // be based on a real or virtual time domain. Used by Timer.
  virtual base::TimeTicks MonotonicallyIncreasingVirtualTime() const = 0;

  // Test helpers.

  // Return a reference to an underlying RendererScheduler object.
  // Can be null if there is no underlying RendererScheduler
  // (e.g. worker threads).
  virtual scheduler::RendererScheduler* GetRendererSchedulerForTest() {
    return nullptr;
  }
};

}  // namespace blink

#endif  // THIRD_PARTY_WEBKIT_SOURCE_PLATFORM_SCHEDULER_CHILD_WEB_SCHEDULER_H_
