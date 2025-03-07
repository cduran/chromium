// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/task_manager/sampling/task_manager_impl.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "base/command_line.h"
#include "base/containers/adapters.h"
#include "base/task_scheduler/post_task.h"
#include "build/build_config.h"
#include "chrome/browser/task_manager/providers/browser_process_task_provider.h"
#include "chrome/browser/task_manager/providers/child_process_task_provider.h"
#include "chrome/browser/task_manager/providers/fallback_task_provider.h"
#include "chrome/browser/task_manager/providers/render_process_host_task_provider.h"
#include "chrome/browser/task_manager/providers/web_contents/web_contents_task_provider.h"
#include "chrome/browser/task_manager/sampling/shared_sampler.h"
#include "chrome/common/chrome_switches.h"
#include "components/nacl/common/buildflags.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/gpu_data_manager.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/child_process_host.h"
#include "services/resource_coordinator/public/cpp/memory_instrumentation/memory_instrumentation.h"

#if defined(OS_CHROMEOS)
#include "chrome/browser/task_manager/providers/arc/arc_process_task_provider.h"
#include "components/arc/arc_util.h"
#endif  // defined(OS_CHROMEOS)

namespace task_manager {

namespace {

base::LazyInstance<TaskManagerImpl>::DestructorAtExit
    lazy_task_manager_instance = LAZY_INSTANCE_INITIALIZER;

}  // namespace

TaskManagerImpl::TaskManagerImpl()
    : on_background_data_ready_callback_(
          base::Bind(&TaskManagerImpl::OnTaskGroupBackgroundCalculationsDone,
                     base::Unretained(this))),
      blocking_pool_runner_(base::CreateSequencedTaskRunnerWithTraits(
          {base::MayBlock(), base::TaskPriority::BACKGROUND,
           base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN})),
      shared_sampler_(new SharedSampler(blocking_pool_runner_)),
      is_running_(false),
      waiting_for_memory_dump_(false),
      weak_ptr_factory_(this) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  task_providers_.emplace_back(new BrowserProcessTaskProvider());
  task_providers_.emplace_back(new ChildProcessTaskProvider());
  if (!base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kTaskManagerShowExtraRenderers)) {
    task_providers_.emplace_back(new WebContentsTaskProvider());
  } else {
    std::unique_ptr<TaskProvider> primary_subprovider(
        new WebContentsTaskProvider());
    std::unique_ptr<TaskProvider> secondary_subprovider(
        new RenderProcessHostTaskProvider());
    task_providers_.emplace_back(new FallbackTaskProvider(
        std::move(primary_subprovider), std::move(secondary_subprovider)));
  }

#if defined(OS_CHROMEOS)
  if (arc::IsArcAvailable())
    task_providers_.emplace_back(new ArcProcessTaskProvider());
#endif  // defined(OS_CHROMEOS)
}

TaskManagerImpl::~TaskManagerImpl() {
}

// static
TaskManagerImpl* TaskManagerImpl::GetInstance() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  return lazy_task_manager_instance.Pointer();
}

void TaskManagerImpl::ActivateTask(TaskId task_id) {
  GetTaskByTaskId(task_id)->Activate();
}

bool TaskManagerImpl::IsTaskKillable(TaskId task_id) {
  return GetTaskByTaskId(task_id)->IsKillable();
}

void TaskManagerImpl::KillTask(TaskId task_id) {
  GetTaskByTaskId(task_id)->Kill();
}

double TaskManagerImpl::GetPlatformIndependentCPUUsage(TaskId task_id) const {
  return GetTaskGroupByTaskId(task_id)->platform_independent_cpu_usage();
}

base::Time TaskManagerImpl::GetStartTime(TaskId task_id) const {
#if defined(OS_WIN)
  return GetTaskGroupByTaskId(task_id)->start_time();
#else
  NOTIMPLEMENTED();
  return base::Time();
#endif
}

base::TimeDelta TaskManagerImpl::GetCpuTime(TaskId task_id) const {
#if defined(OS_WIN)
  return GetTaskGroupByTaskId(task_id)->cpu_time();
#else
  NOTIMPLEMENTED();
  return base::TimeDelta();
#endif
}

int64_t TaskManagerImpl::GetMemoryFootprintUsage(TaskId task_id) const {
  return GetTaskGroupByTaskId(task_id)->footprint_bytes();
}

int64_t TaskManagerImpl::GetSwappedMemoryUsage(TaskId task_id) const {
#if defined(OS_CHROMEOS)
  return GetTaskGroupByTaskId(task_id)->swapped_bytes();
#else
  return -1;
#endif
}

int64_t TaskManagerImpl::GetGpuMemoryUsage(TaskId task_id,
                                           bool* has_duplicates) const {
  const TaskGroup* task_group = GetTaskGroupByTaskId(task_id);
  if (has_duplicates)
    *has_duplicates = task_group->gpu_memory_has_duplicates();
  return task_group->gpu_memory();
}

base::MemoryState TaskManagerImpl::GetMemoryState(TaskId task_id) const {
  return GetTaskGroupByTaskId(task_id)->memory_state();
}

int TaskManagerImpl::GetIdleWakeupsPerSecond(TaskId task_id) const {
  return GetTaskGroupByTaskId(task_id)->idle_wakeups_per_second();
}

int TaskManagerImpl::GetHardFaultsPerSecond(TaskId task_id) const {
#if defined(OS_WIN)
  return GetTaskGroupByTaskId(task_id)->hard_faults_per_second();
#else
  return -1;
#endif
}

int TaskManagerImpl::GetNaClDebugStubPort(TaskId task_id) const {
#if BUILDFLAG(ENABLE_NACL)
  return GetTaskGroupByTaskId(task_id)->nacl_debug_stub_port();
#else
  return -2;
#endif  // BUILDFLAG(ENABLE_NACL)
}

void TaskManagerImpl::GetGDIHandles(TaskId task_id,
                                    int64_t* current,
                                    int64_t* peak) const {
#if defined(OS_WIN)
  const TaskGroup* task_group = GetTaskGroupByTaskId(task_id);
  *current = task_group->gdi_current_handles();
  *peak = task_group->gdi_peak_handles();
#else
  *current = -1;
  *peak = -1;
#endif  // defined(OS_WIN)
}

void TaskManagerImpl::GetUSERHandles(TaskId task_id,
                                     int64_t* current,
                                     int64_t* peak) const {
#if defined(OS_WIN)
  const TaskGroup* task_group = GetTaskGroupByTaskId(task_id);
  *current = task_group->user_current_handles();
  *peak = task_group->user_peak_handles();
#else
  *current = -1;
  *peak = -1;
#endif  // defined(OS_WIN)
}

int TaskManagerImpl::GetOpenFdCount(TaskId task_id) const {
#if defined(OS_LINUX)
  return GetTaskGroupByTaskId(task_id)->open_fd_count();
#else
  return -1;
#endif  // defined(OS_LINUX)
}

bool TaskManagerImpl::IsTaskOnBackgroundedProcess(TaskId task_id) const {
  return GetTaskGroupByTaskId(task_id)->is_backgrounded();
}

const base::string16& TaskManagerImpl::GetTitle(TaskId task_id) const {
  return GetTaskByTaskId(task_id)->title();
}

const std::string& TaskManagerImpl::GetTaskNameForRappor(TaskId task_id) const {
  return GetTaskByTaskId(task_id)->rappor_sample_name();
}

base::string16 TaskManagerImpl::GetProfileName(TaskId task_id) const {
  return GetTaskByTaskId(task_id)->GetProfileName();
}

const gfx::ImageSkia& TaskManagerImpl::GetIcon(TaskId task_id) const {
  return GetTaskByTaskId(task_id)->icon();
}

const base::ProcessHandle& TaskManagerImpl::GetProcessHandle(
    TaskId task_id) const {
  return GetTaskGroupByTaskId(task_id)->process_handle();
}

const base::ProcessId& TaskManagerImpl::GetProcessId(TaskId task_id) const {
  return GetTaskGroupByTaskId(task_id)->process_id();
}

Task::Type TaskManagerImpl::GetType(TaskId task_id) const {
  return GetTaskByTaskId(task_id)->GetType();
}

int TaskManagerImpl::GetTabId(TaskId task_id) const {
  return GetTaskByTaskId(task_id)->GetTabId();
}

int TaskManagerImpl::GetChildProcessUniqueId(TaskId task_id) const {
  return GetTaskByTaskId(task_id)->GetChildProcessUniqueID();
}

void TaskManagerImpl::GetTerminationStatus(TaskId task_id,
                                           base::TerminationStatus* out_status,
                                           int* out_error_code) const {
  GetTaskByTaskId(task_id)->GetTerminationStatus(out_status, out_error_code);
}

int64_t TaskManagerImpl::GetNetworkUsage(TaskId task_id) const {
  return GetTaskByTaskId(task_id)->network_usage_rate();
}

int64_t TaskManagerImpl::GetCumulativeNetworkUsage(TaskId task_id) const {
  return GetTaskByTaskId(task_id)->cumulative_network_usage();
}

int64_t TaskManagerImpl::GetProcessTotalNetworkUsage(TaskId task_id) const {
  return GetTaskGroupByTaskId(task_id)->per_process_network_usage_rate();
}

int64_t TaskManagerImpl::GetCumulativeProcessTotalNetworkUsage(
    TaskId task_id) const {
  return GetTaskGroupByTaskId(task_id)->cumulative_per_process_network_usage();
}

int64_t TaskManagerImpl::GetSqliteMemoryUsed(TaskId task_id) const {
  return GetTaskByTaskId(task_id)->GetSqliteMemoryUsed();
}

bool TaskManagerImpl::GetV8Memory(TaskId task_id,
                                  int64_t* allocated,
                                  int64_t* used) const {
  const Task* task = GetTaskByTaskId(task_id);
  if (!task->ReportsV8Memory())
    return false;

  *allocated = task->GetV8MemoryAllocated();
  *used = task->GetV8MemoryUsed();

  return true;
}

bool TaskManagerImpl::GetWebCacheStats(
    TaskId task_id,
    blink::WebCache::ResourceTypeStats* stats) const {
  const Task* task = GetTaskByTaskId(task_id);
  if (!task->ReportsWebCacheStats())
    return false;

  *stats = task->GetWebCacheStats();

  return true;
}

int TaskManagerImpl::GetKeepaliveCount(TaskId task_id) const {
  const Task* task = GetTaskByTaskId(task_id);
  if (!task)
    return -1;

  return task->GetKeepaliveCount();
}

const TaskIdList& TaskManagerImpl::GetTaskIdsList() const {
  DCHECK(is_running_) << "Task manager is not running. You must observe the "
      "task manager for it to start running";

  if (sorted_task_ids_.empty()) {
    // |comparator| groups and sorts by subtask-ness (to push all subtasks to be
    // last), then by process type (e.g. the browser process should be first;
    // renderer processes should be together), then tab id (processes used by
    // the same tab should be kept together, and a tab should have a stable
    // position in the list as it cycles through processes, and tab creation
    // order is meaningful), and finally by task id (when all else is equal, put
    // the oldest tasks first).
    auto comparator = [](const Task* a, const Task* b) -> bool {
      return std::make_tuple(a->HasParentTask(), a->GetType(), a->GetTabId(),
                             a->task_id()) <
             std::make_tuple(b->HasParentTask(), b->GetType(), b->GetTabId(),
                             b->task_id());
    };

    const size_t num_groups = task_groups_by_proc_id_.size();
    const size_t num_tasks = task_groups_by_task_id_.size();

    // Populate |tasks_to_visit| with one task from each group.
    std::vector<const Task*> tasks_to_visit;
    tasks_to_visit.reserve(num_groups);
    std::unordered_map<const Task*, std::vector<const Task*>> children;
    for (const auto& groups_pair : task_groups_by_proc_id_) {
      // The first task in the group (per |comparator|) is the one used for
      // sorting the group relative to other groups.
      const std::vector<Task*>& tasks = groups_pair.second->tasks();
      Task* group_task =
          *std::min_element(tasks.begin(), tasks.end(), comparator);
      tasks_to_visit.push_back(group_task);

      // Build the parent-to-child map, for use later.
      for (const Task* task : tasks) {
        if (task->HasParentTask())
          children[task->GetParentTask()].push_back(task);
        else
          DCHECK(!group_task->HasParentTask());
      }
    }

    // Now sort |tasks_to_visit| in reverse order (putting the browser process
    // at back()). We will treat it as a stack from now on.
    std::sort(tasks_to_visit.rbegin(), tasks_to_visit.rend(), comparator);
    DCHECK_EQ(Task::BROWSER, tasks_to_visit.back()->GetType());

    // Using |tasks_to_visit| as a stack, and |visited_groups| to track which
    // groups we've already added, add groups to |sorted_task_ids_| until all
    // groups have been added.
    sorted_task_ids_.reserve(num_tasks);
    std::unordered_set<TaskGroup*> visited_groups;
    visited_groups.reserve(num_groups);
    std::vector<Task*> current_group_tasks;  // Outside loop for fewer mallocs.
    while (visited_groups.size() < num_groups) {
      DCHECK(!tasks_to_visit.empty());
      TaskGroup* current_group =
          GetTaskGroupByTaskId(tasks_to_visit.back()->task_id());
      tasks_to_visit.pop_back();

      // Mark |current_group| as visited. If this fails, we've already added
      // the group, and should skip over it.
      if (!visited_groups.insert(current_group).second)
        continue;

      // Make a copy of |current_group->tasks()|, sort it, and append the ids.
      current_group_tasks = current_group->tasks();
      std::sort(current_group_tasks.begin(), current_group_tasks.end(),
                comparator);
      for (Task* task : current_group_tasks)
        sorted_task_ids_.push_back(task->task_id());

      // Find the children of the tasks we just added, and push them into
      // |tasks_to_visit|, so that we visit them soon. Work in reverse order,
      // so that we visit them in forward order.
      for (Task* parent : base::Reversed(current_group_tasks)) {
        auto children_of_parent = children.find(parent);
        if (children_of_parent != children.end()) {
          // Sort children[parent], and then append in reversed order.
          std::sort(children_of_parent->second.begin(),
                    children_of_parent->second.end(), comparator);
          tasks_to_visit.insert(tasks_to_visit.end(),
                                children_of_parent->second.rbegin(),
                                children_of_parent->second.rend());
        }
      }
    }
    DCHECK_EQ(num_tasks, sorted_task_ids_.size());
  }

  return sorted_task_ids_;
}

TaskIdList TaskManagerImpl::GetIdsOfTasksSharingSameProcess(
    TaskId task_id) const {
  DCHECK(is_running_) << "Task manager is not running. You must observe the "
      "task manager for it to start running";

  TaskIdList result;
  TaskGroup* group = GetTaskGroupByTaskId(task_id);
  if (group) {
    result.reserve(group->tasks().size());
    for (Task* task : group->tasks())
      result.push_back(task->task_id());
  }
  return result;
}

size_t TaskManagerImpl::GetNumberOfTasksOnSameProcess(TaskId task_id) const {
  return GetTaskGroupByTaskId(task_id)->num_tasks();
}

TaskId TaskManagerImpl::GetTaskIdForWebContents(
    content::WebContents* web_contents) const {
  if (!web_contents)
    return -1;
  content::RenderFrameHost* rfh = web_contents->GetMainFrame();
  Task* task = GetTaskByRoute(rfh->GetProcess()->GetID(), rfh->GetRoutingID());
  if (!task)
    return -1;
  return task->task_id();
}

void TaskManagerImpl::TaskAdded(Task* task) {
  DCHECK(task);

  const base::ProcessId proc_id = task->process_id();
  const TaskId task_id = task->task_id();

  std::unique_ptr<TaskGroup>& task_group = task_groups_by_proc_id_[proc_id];
  if (!task_group)
    task_group.reset(new TaskGroup(task->process_handle(), proc_id,
                                   on_background_data_ready_callback_,
                                   shared_sampler_, blocking_pool_runner_));

  task_group->AddTask(task);

  task_groups_by_task_id_[task_id] = task_group.get();

  // Invalidate the cached sorted IDs by clearing the list.
  sorted_task_ids_.clear();

  NotifyObserversOnTaskAdded(task_id);
}

void TaskManagerImpl::TaskRemoved(Task* task) {
  DCHECK(task);

  const base::ProcessId proc_id = task->process_id();
  const TaskId task_id = task->task_id();

  DCHECK(task_groups_by_proc_id_.count(proc_id));

  NotifyObserversOnTaskToBeRemoved(task_id);

  TaskGroup* task_group = GetTaskGroupByTaskId(task_id);
  task_group->RemoveTask(task);
  task_groups_by_task_id_.erase(task_id);

  if (task_group->empty())
    task_groups_by_proc_id_.erase(proc_id);  // Deletes |task_group|.

  // Invalidate the cached sorted IDs by clearing the list.
  sorted_task_ids_.clear();
}

void TaskManagerImpl::TaskUnresponsive(Task* task) {
  DCHECK(task);
  NotifyObserversOnTaskUnresponsive(task->task_id());
}

// static
void TaskManagerImpl::OnMultipleBytesTransferredUI(BytesTransferredMap params) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  for (const auto& entry : params) {
    const BytesTransferredKey& process_info = entry.first;
    const BytesTransferredParam& bytes_transferred = entry.second;

    if (!GetInstance()->UpdateTasksWithBytesTransferred(process_info,
                                                        bytes_transferred)) {
      // We can't match a task to the notification.  That might mean the
      // tab that started a download was closed, or the request may have had
      // no originating task associated with it in the first place.
      //
      // Orphaned/unaccounted activity is credited to the Browser process.
      BytesTransferredKey browser_process_key = {
          content::ChildProcessHost::kInvalidUniqueID, MSG_ROUTING_NONE};
      GetInstance()->UpdateTasksWithBytesTransferred(browser_process_key,
                                                     bytes_transferred);
    }
  }
}

void TaskManagerImpl::OnVideoMemoryUsageStatsUpdate(
    const gpu::VideoMemoryUsageStats& gpu_memory_stats) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  gpu_memory_stats_ = gpu_memory_stats;
}

void TaskManagerImpl::OnReceivedMemoryDump(
    bool success,
    std::unique_ptr<memory_instrumentation::GlobalMemoryDump> dump) {
  waiting_for_memory_dump_ = false;
  // We can ignore the value of success as it is a coarse grained indicator
  // of whether the global dump was successful; usually because of a missing
  // process or OS dumps. There may still be useful information for other
  // processes in the global dump when success is false.
  if (!dump)
    return;
  for (const auto& pmd : dump->process_dumps()) {
    auto it = task_groups_by_proc_id_.find(pmd.pid());
    if (it == task_groups_by_proc_id_.end())
      continue;
    it->second->set_footprint_bytes(
        static_cast<uint64_t>(pmd.os_dump().private_footprint_kb) * 1024);
  }
}

void TaskManagerImpl::Refresh() {
  if (IsResourceRefreshEnabled(REFRESH_TYPE_GPU_MEMORY)) {
    content::GpuDataManager::GetInstance()->RequestVideoMemoryUsageStatsUpdate(
        base::Bind(&TaskManagerImpl::OnVideoMemoryUsageStatsUpdate,
                   weak_ptr_factory_.GetWeakPtr()));
  }

  if (IsResourceRefreshEnabled(REFRESH_TYPE_MEMORY_FOOTPRINT) &&
      !waiting_for_memory_dump_) {
    // The callback keeps this object alive until the callback is invoked.
    waiting_for_memory_dump_ = true;
    auto callback = base::Bind(&TaskManagerImpl::OnReceivedMemoryDump,
                               weak_ptr_factory_.GetWeakPtr());
    memory_instrumentation::MemoryInstrumentation::GetInstance()
        ->RequestGlobalDump(std::move(callback));
  }

  for (auto& groups_itr : task_groups_by_proc_id_) {
    groups_itr.second->Refresh(gpu_memory_stats_,
                               GetCurrentRefreshTime(),
                               enabled_resources_flags());
  }

  NotifyObserversOnRefresh(GetTaskIdsList());
}

void TaskManagerImpl::StartUpdating() {
  if (is_running_)
    return;

  is_running_ = true;

  for (const auto& provider : task_providers_)
    provider->SetObserver(this);

  io_thread_helper_manager_.reset(new IoThreadHelperManager(
      base::BindRepeating(&TaskManagerImpl::OnMultipleBytesTransferredUI)));
}

void TaskManagerImpl::StopUpdating() {
  if (!is_running_)
    return;

  is_running_ = false;

  io_thread_helper_manager_.reset();

  for (const auto& provider : task_providers_)
    provider->ClearObserver();

  task_groups_by_proc_id_.clear();
  task_groups_by_task_id_.clear();
  sorted_task_ids_.clear();
}

Task* TaskManagerImpl::GetTaskByRoute(int child_id, int route_id) const {
  for (const auto& task_provider : task_providers_) {
    Task* task = task_provider->GetTaskOfUrlRequest(child_id, route_id);
    if (task)
      return task;
  }
  return nullptr;
}

bool TaskManagerImpl::UpdateTasksWithBytesTransferred(
    const BytesTransferredKey& key,
    const BytesTransferredParam& param) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  Task* task = GetTaskByRoute(key.child_id, key.route_id);
  if (task) {
    task->OnNetworkBytesRead(param.byte_read_count);
    task->OnNetworkBytesSent(param.byte_sent_count);
    return true;
  }

  // Couldn't match the bytes to any existing task.
  return false;
}

TaskGroup* TaskManagerImpl::GetTaskGroupByTaskId(TaskId task_id) const {
  auto it = task_groups_by_task_id_.find(task_id);
  DCHECK(it != task_groups_by_task_id_.end());
  return it->second;
}

Task* TaskManagerImpl::GetTaskByTaskId(TaskId task_id) const {
  return GetTaskGroupByTaskId(task_id)->GetTaskById(task_id);
}

void TaskManagerImpl::OnTaskGroupBackgroundCalculationsDone() {
  // TODO(afakhry): There should be a better way for doing this!
  bool are_all_processes_data_ready = true;
  for (const auto& groups_itr : task_groups_by_proc_id_) {
    are_all_processes_data_ready &=
        groups_itr.second->AreBackgroundCalculationsDone();
  }
  if (are_all_processes_data_ready) {
    NotifyObserversOnRefreshWithBackgroundCalculations(GetTaskIdsList());
    for (const auto& groups_itr : task_groups_by_proc_id_)
      groups_itr.second->ClearCurrentBackgroundCalculationsFlags();
  }
}

}  // namespace task_manager
