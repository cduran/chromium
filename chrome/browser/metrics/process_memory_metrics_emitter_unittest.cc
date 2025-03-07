// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/metrics/process_memory_metrics_emitter.h"

#include "base/containers/flat_map.h"
#include "base/memory/ref_counted.h"
#include "base/process/process_handle.h"
#include "base/test/scoped_task_environment.h"
#include "build/build_config.h"
#include "chrome/browser/metrics/renderer_uptime_tracker.h"
#include "components/ukm/test_ukm_recorder.h"
#include "services/metrics/public/cpp/ukm_builders.h"
#include "services/metrics/public/cpp/ukm_recorder.h"
#include "testing/gtest/include/gtest/gtest.h"

using GlobalMemoryDump = memory_instrumentation::GlobalMemoryDump;
using GlobalMemoryDumpPtr = memory_instrumentation::mojom::GlobalMemoryDumpPtr;
using ProcessMemoryDumpPtr =
    memory_instrumentation::mojom::ProcessMemoryDumpPtr;
using OSMemDumpPtr = memory_instrumentation::mojom::OSMemDumpPtr;
using PageInfoPtr = resource_coordinator::mojom::PageInfoPtr;
using ProcessType = memory_instrumentation::mojom::ProcessType;
using ProcessInfoPtr = resource_coordinator::mojom::ProcessInfoPtr;
using ProcessInfoVector = std::vector<ProcessInfoPtr>;

namespace {

using UkmEntry = ukm::builders::Memory_Experimental;

// Provide fake to surface ReceivedMemoryDump and ReceivedProcessInfos to public
// visibility.
class ProcessMemoryMetricsEmitterFake : public ProcessMemoryMetricsEmitter {
 public:
  ProcessMemoryMetricsEmitterFake(
      ukm::TestAutoSetUkmRecorder& test_ukm_recorder)
      : ukm_recorder_(&test_ukm_recorder) {
    MarkServiceRequestsInProgress();
  }

  void ReceivedMemoryDump(bool success,
                          std::unique_ptr<GlobalMemoryDump> ptr) override {
    ProcessMemoryMetricsEmitter::ReceivedMemoryDump(success, std::move(ptr));
  }

  void ReceivedProcessInfos(ProcessInfoVector process_infos) override {
    ProcessMemoryMetricsEmitter::ReceivedProcessInfos(std::move(process_infos));
  }

  bool IsResourceCoordinatorEnabled() override { return true; }

  ukm::UkmRecorder* GetUkmRecorder() override { return ukm_recorder_; }

  int GetNumberOfExtensions(base::ProcessId pid) override {
    switch (pid) {
      case 401:
        return 1;
      default:
        return 0;
    }
  }

  base::Optional<base::TimeDelta> GetProcessUptime(
      const base::Time& now,
      base::ProcessId pid) override {
    switch (pid) {
      case 401:
        return base::TimeDelta::FromSeconds(21);
      default:
        return base::TimeDelta::FromSeconds(42);
    }
  }

 private:
  ~ProcessMemoryMetricsEmitterFake() override {}

  ukm::UkmRecorder* ukm_recorder_;
  DISALLOW_COPY_AND_ASSIGN(ProcessMemoryMetricsEmitterFake);
};

void SetAllocatorDumpMetric(ProcessMemoryDumpPtr& pmd,
                            const std::string& dump_name,
                            const std::string& metric_name,
                            uint64_t value) {
  auto it = pmd->chrome_dump->entries_for_allocator_dumps.find(dump_name);
  if (it == pmd->chrome_dump->entries_for_allocator_dumps.end()) {
    memory_instrumentation::mojom::AllocatorMemDumpPtr amd(
        memory_instrumentation::mojom::AllocatorMemDump::New());
    amd->numeric_entries.insert(std::make_pair(metric_name, value));
    pmd->chrome_dump->entries_for_allocator_dumps.insert(
        std::make_pair(dump_name, std::move(amd)));
  } else {
    it->second->numeric_entries.insert(std::make_pair(metric_name, value));
  }
}

OSMemDumpPtr GetFakeOSMemDump(uint32_t resident_set_kb,
                              uint32_t private_footprint_kb,
#if defined(OS_LINUX) || defined(OS_ANDROID)
                              uint32_t shared_footprint_kb,
                              uint32_t private_swap_footprint_kb
#else
                              uint32_t shared_footprint_kb
#endif
                              ) {
  using memory_instrumentation::mojom::VmRegion;

#if defined(OS_LINUX) || defined(OS_ANDROID)
  return memory_instrumentation::mojom::OSMemDump::New(
      resident_set_kb, private_footprint_kb, shared_footprint_kb,
      private_swap_footprint_kb);
#else
  return memory_instrumentation::mojom::OSMemDump::New(
      resident_set_kb, private_footprint_kb, shared_footprint_kb);
#endif
}

void PopulateBrowserMetrics(GlobalMemoryDumpPtr& global_dump,
                            base::flat_map<const char*, int64_t>& metrics_mb) {
  ProcessMemoryDumpPtr pmd(
      memory_instrumentation::mojom::ProcessMemoryDump::New());
  pmd->process_type = ProcessType::BROWSER;
  pmd->chrome_dump = memory_instrumentation::mojom::ChromeMemDump::New();
  SetAllocatorDumpMetric(pmd, "malloc", "effective_size",
                         metrics_mb["Malloc"] * 1024 * 1024);
  OSMemDumpPtr os_dump =
      GetFakeOSMemDump(metrics_mb["Resident"] * 1024,
                       metrics_mb["PrivateMemoryFootprint"] * 1024,
#if defined(OS_LINUX) || defined(OS_ANDROID)
                       // accessing PrivateSwapFootprint on other OSes will
                       // modify metrics_mb to create the value, which leads to
                       // expectation failures.
                       metrics_mb["SharedMemoryFootprint"] * 1024,
                       metrics_mb["PrivateSwapFootprint"] * 1024
#else
                       metrics_mb["SharedMemoryFootprint"] * 1024
#endif
                       );
  pmd->os_dump = std::move(os_dump);
  global_dump->process_dumps.push_back(std::move(pmd));
}

base::flat_map<const char*, int64_t> GetExpectedBrowserMetrics() {
  return base::flat_map<const char*, int64_t>(
      {
        {"ProcessType", static_cast<int64_t>(ProcessType::BROWSER)},
            {"Resident", 10},
            {"Malloc", 20},
            {"PrivateMemoryFootprint", 30}, {"SharedMemoryFootprint", 35},
            {"Uptime", 42},
#if defined(OS_LINUX) || defined(OS_ANDROID)
            {"PrivateSwapFootprint", 50},
#endif
      },
      base::KEEP_FIRST_OF_DUPES);
}

void PopulateRendererMetrics(
    GlobalMemoryDumpPtr& global_dump,
    base::flat_map<const char*, int64_t>& metrics_mb_or_count,
    base::ProcessId pid) {
  ProcessMemoryDumpPtr pmd(
      memory_instrumentation::mojom::ProcessMemoryDump::New());
  pmd->process_type = ProcessType::RENDERER;
  pmd->chrome_dump = memory_instrumentation::mojom::ChromeMemDump::New();
  SetAllocatorDumpMetric(pmd, "malloc", "effective_size",
                         metrics_mb_or_count["Malloc"] * 1024 * 1024);
  SetAllocatorDumpMetric(pmd, "partition_alloc", "effective_size",
                         metrics_mb_or_count["PartitionAlloc"] * 1024 * 1024);
  SetAllocatorDumpMetric(pmd, "blink_gc", "effective_size",
                         metrics_mb_or_count["BlinkGC"] * 1024 * 1024);
  SetAllocatorDumpMetric(pmd, "v8", "effective_size",
                         metrics_mb_or_count["V8"] * 1024 * 1024);

  SetAllocatorDumpMetric(pmd, "blink_objects/Document", "object_count",
                         metrics_mb_or_count["NumberOfDocuments"]);
  SetAllocatorDumpMetric(pmd, "blink_objects/Frame", "object_count",
                         metrics_mb_or_count["NumberOfFrames"]);
  SetAllocatorDumpMetric(pmd, "blink_objects/LayoutObject", "object_count",
                         metrics_mb_or_count["NumberOfLayoutObjects"]);
  SetAllocatorDumpMetric(pmd, "blink_objects/Node", "object_count",
                         metrics_mb_or_count["NumberOfNodes"]);
  SetAllocatorDumpMetric(
      pmd, "partition_alloc/partitions/array_buffer", "effective_size",
      metrics_mb_or_count["PartitionAlloc.Partitions.ArrayBuffer"] * 1024 *
          1024);

  OSMemDumpPtr os_dump = GetFakeOSMemDump(
      metrics_mb_or_count["Resident"] * 1024,
      metrics_mb_or_count["PrivateMemoryFootprint"] * 1024,
#if defined(OS_LINUX) || defined(OS_ANDROID)
      // accessing PrivateSwapFootprint on other OSes will
      // modify metrics_mb_or_count to create the value, which leads to
      // expectation failures.
      metrics_mb_or_count["SharedMemoryFootprint"] * 1024,
      metrics_mb_or_count["PrivateSwapFootprint"] * 1024
#else
      metrics_mb_or_count["SharedMemoryFootprint"] * 1024
#endif
      );
  pmd->os_dump = std::move(os_dump);
  pmd->pid = pid;
  global_dump->process_dumps.push_back(std::move(pmd));
}

base::flat_map<const char*, int64_t> GetExpectedRendererMetrics() {
  return base::flat_map<const char*, int64_t>(
      {
        {"ProcessType", static_cast<int64_t>(ProcessType::RENDERER)},
            {"Resident", 110}, {"Malloc", 120}, {"PrivateMemoryFootprint", 130},
            {"SharedMemoryFootprint", 135}, {"PartitionAlloc", 140},
            {"BlinkGC", 150}, {"V8", 160}, {"NumberOfExtensions", 0},
            {"Uptime", 42},
#if defined(OS_LINUX) || defined(OS_ANDROID)
            {"PrivateSwapFootprint", 50},
#endif
            {"NumberOfDocuments", 1}, {"NumberOfFrames", 2},
            {"NumberOfLayoutObjects", 5}, {"NumberOfNodes", 3},
            {"PartitionAlloc.Partitions.ArrayBuffer", 10},
      },
      base::KEEP_FIRST_OF_DUPES);
}

void AddPageMetrics(base::flat_map<const char*, int64_t>& expected_metrics) {
  expected_metrics["IsVisible"] = true;
  expected_metrics["TimeSinceLastNavigation"] = 20;
  expected_metrics["TimeSinceLastVisibilityChange"] = 15;
}

void PopulateGpuMetrics(GlobalMemoryDumpPtr& global_dump,
                        base::flat_map<const char*, int64_t>& metrics_mb) {
  ProcessMemoryDumpPtr pmd(
      memory_instrumentation::mojom::ProcessMemoryDump::New());
  pmd->process_type = ProcessType::GPU;
  pmd->chrome_dump = memory_instrumentation::mojom::ChromeMemDump::New();
  SetAllocatorDumpMetric(pmd, "malloc", "effective_size",
                         metrics_mb["Malloc"] * 1024 * 1024);
  SetAllocatorDumpMetric(pmd, "gpu/gl", "effective_size",
                         metrics_mb["CommandBuffer"] * 1024 * 1024);
  OSMemDumpPtr os_dump =
      GetFakeOSMemDump(metrics_mb["Resident"] * 1024,
                       metrics_mb["PrivateMemoryFootprint"] * 1024,
#if defined(OS_LINUX) || defined(OS_ANDROID)
                       // accessing PrivateSwapFootprint on other OSes will
                       // modify metrics_mb to create the value, which leads to
                       // expectation failures.
                       metrics_mb["SharedMemoryFootprint"] * 1024,
                       metrics_mb["PrivateSwapFootprint"] * 1024
#else
                       metrics_mb["SharedMemoryFootprint"] * 1024
#endif
                       );
  pmd->os_dump = std::move(os_dump);
  global_dump->process_dumps.push_back(std::move(pmd));
}

base::flat_map<const char*, int64_t> GetExpectedGpuMetrics() {
  return base::flat_map<const char*, int64_t>(
      {
        {"ProcessType", static_cast<int64_t>(ProcessType::GPU)},
            {"Resident", 210},
            {"Malloc", 220},
            {"PrivateMemoryFootprint", 230}, {"SharedMemoryFootprint", 235},
            {"CommandBuffer", 240}, {"Uptime", 42},
#if defined(OS_LINUX) || defined(OS_ANDROID)
            {"PrivateSwapFootprint", 50},
#endif
      },
      base::KEEP_FIRST_OF_DUPES);
}

void PopulateMetrics(GlobalMemoryDumpPtr& global_dump,
                     ProcessType ptype,
                     base::flat_map<const char*, int64_t>& metrics_mb) {
  switch (ptype) {
    case ProcessType::BROWSER:
      PopulateBrowserMetrics(global_dump, metrics_mb);
      return;
    case ProcessType::RENDERER:
      PopulateRendererMetrics(global_dump, metrics_mb, 101);
      return;
    case ProcessType::GPU:
      PopulateGpuMetrics(global_dump, metrics_mb);
      return;
    case ProcessType::UTILITY:
    case ProcessType::PLUGIN:
    case ProcessType::OTHER:
      break;
  }

  // We shouldn't reach here.
  FAIL() << "Unknown process type case " << ptype << ".";
}

base::flat_map<const char*, int64_t> GetExpectedProcessMetrics(
    ProcessType ptype) {
  switch (ptype) {
    case ProcessType::BROWSER:
      return GetExpectedBrowserMetrics();
    case ProcessType::RENDERER:
      return GetExpectedRendererMetrics();
    case ProcessType::GPU:
      return GetExpectedGpuMetrics();
    case ProcessType::UTILITY:
    case ProcessType::PLUGIN:
    case ProcessType::OTHER:
      break;
  }

  // We shouldn't reach here.
  CHECK(false);
  return base::flat_map<const char*, int64_t>();
}

ProcessInfoVector GetProcessInfo(ukm::TestUkmRecorder& ukm_recorder) {
  ProcessInfoVector process_infos;

  // Process 200 always has no URLs.
  {
    ProcessInfoPtr process_info(
        resource_coordinator::mojom::ProcessInfo::New());
    process_info->pid = 200;
    process_infos.push_back(std::move(process_info));
  }

  // Process 201 always has 1 URL
  {
    ProcessInfoPtr process_info(
        resource_coordinator::mojom::ProcessInfo::New());
    process_info->pid = 201;
    ukm::SourceId first_source_id = ukm::UkmRecorder::GetNewSourceID();
    ukm_recorder.UpdateSourceURL(first_source_id,
                                 GURL("http://www.url201.com/"));
    PageInfoPtr page_info(resource_coordinator::mojom::PageInfo::New());

    page_info->ukm_source_id = first_source_id;
    page_info->is_visible = true;
    page_info->time_since_last_visibility_change =
        base::TimeDelta::FromSeconds(15);
    page_info->time_since_last_navigation = base::TimeDelta::FromSeconds(20);
    process_info->page_infos.push_back(std::move(page_info));
    process_infos.push_back(std::move(process_info));
  }

  // Process 202 always has 2 URL
  {
    ProcessInfoPtr process_info(
        resource_coordinator::mojom::ProcessInfo::New());
    process_info->pid = 202;
    ukm::SourceId first_source_id = ukm::UkmRecorder::GetNewSourceID();
    ukm::SourceId second_source_id = ukm::UkmRecorder::GetNewSourceID();
    ukm_recorder.UpdateSourceURL(first_source_id,
                                 GURL("http://www.url2021.com/"));
    ukm_recorder.UpdateSourceURL(second_source_id,
                                 GURL("http://www.url2022.com/"));
    PageInfoPtr page_info1(resource_coordinator::mojom::PageInfo::New());
    page_info1->ukm_source_id = first_source_id;
    page_info1->time_since_last_visibility_change =
        base::TimeDelta::FromSeconds(11);
    page_info1->time_since_last_navigation = base::TimeDelta::FromSeconds(21);
    PageInfoPtr page_info2(resource_coordinator::mojom::PageInfo::New());
    page_info2->ukm_source_id = second_source_id;
    page_info2->time_since_last_visibility_change =
        base::TimeDelta::FromSeconds(12);
    page_info2->time_since_last_navigation = base::TimeDelta::FromSeconds(22);
    process_info->page_infos.push_back(std::move(page_info1));
    process_info->page_infos.push_back(std::move(page_info2));

    process_infos.push_back(std::move(process_info));
  }
  return process_infos;
}

}  // namespace

class ProcessMemoryMetricsEmitterTest
    : public testing::TestWithParam<ProcessType> {
 public:
  ProcessMemoryMetricsEmitterTest() {}
  ~ProcessMemoryMetricsEmitterTest() override {}

 protected:
  void CheckMemoryUkmEntryMetrics(
      const std::vector<base::flat_map<const char*, int64_t>>& expected,
      size_t expected_total_memory_entries = 1u) {
    const auto& entries =
        test_ukm_recorder_.GetEntriesByName(UkmEntry::kEntryName);
    size_t i = 0;
    size_t total_memory_entries = 0;
    for (const auto* entry : entries) {
      if (test_ukm_recorder_.EntryHasMetric(
              entry, UkmEntry::kTotal2_PrivateMemoryFootprintName)) {
        total_memory_entries++;
        continue;
      }
      if (i >= expected.size()) {
        FAIL() << "Unexpected non-total entry.";
        continue;
      }
      for (const auto& kv : expected[i]) {
        test_ukm_recorder_.ExpectEntryMetric(entry, kv.first, kv.second);
      }
      i++;
    }
    EXPECT_EQ(expected_total_memory_entries, total_memory_entries);
    EXPECT_EQ(expected.size() + expected_total_memory_entries, entries.size());
  }

  base::test::ScopedTaskEnvironment scoped_task_environment_;
  ukm::TestAutoSetUkmRecorder test_ukm_recorder_;

 private:
  DISALLOW_COPY_AND_ASSIGN(ProcessMemoryMetricsEmitterTest);
};

TEST_P(ProcessMemoryMetricsEmitterTest, CollectsSingleProcessUKMs) {
  base::flat_map<const char*, int64_t> expected_metrics =
      GetExpectedProcessMetrics(GetParam());

  GlobalMemoryDumpPtr global_dump(
      memory_instrumentation::mojom::GlobalMemoryDump::New());
  PopulateMetrics(global_dump, GetParam(), expected_metrics);

  scoped_refptr<ProcessMemoryMetricsEmitterFake> emitter(
      new ProcessMemoryMetricsEmitterFake(test_ukm_recorder_));
  emitter->ReceivedProcessInfos(ProcessInfoVector());
  emitter->ReceivedMemoryDump(
      true, GlobalMemoryDump::MoveFrom(std::move(global_dump)));

  std::vector<base::flat_map<const char*, int64_t>> expected_entries;
  expected_entries.push_back(expected_metrics);
  CheckMemoryUkmEntryMetrics(expected_entries);
}

INSTANTIATE_TEST_CASE_P(SinglePtype,
                        ProcessMemoryMetricsEmitterTest,
                        testing::Values(ProcessType::BROWSER,
                                        ProcessType::RENDERER,
                                        ProcessType::GPU));

TEST_F(ProcessMemoryMetricsEmitterTest, CollectsExtensionProcessUKMs) {
  base::flat_map<const char*, int64_t> expected_metrics =
      GetExpectedRendererMetrics();
  expected_metrics["NumberOfExtensions"] = 1;
  expected_metrics["Uptime"] = 21;

  GlobalMemoryDumpPtr global_dump(
      memory_instrumentation::mojom::GlobalMemoryDump::New());
  PopulateRendererMetrics(global_dump, expected_metrics, 401);

  scoped_refptr<ProcessMemoryMetricsEmitterFake> emitter(
      new ProcessMemoryMetricsEmitterFake(test_ukm_recorder_));
  emitter->ReceivedProcessInfos(ProcessInfoVector());
  emitter->ReceivedMemoryDump(
      true, GlobalMemoryDump::MoveFrom(std::move(global_dump)));

  std::vector<base::flat_map<const char*, int64_t>> expected_entries;
  expected_entries.push_back(expected_metrics);
  CheckMemoryUkmEntryMetrics(expected_entries);
}

TEST_F(ProcessMemoryMetricsEmitterTest, CollectsManyProcessUKMsSingleDump) {
  std::vector<ProcessType> entries_ptypes = {
      ProcessType::BROWSER, ProcessType::RENDERER, ProcessType::GPU,
      ProcessType::GPU,     ProcessType::RENDERER, ProcessType::BROWSER,
  };

  GlobalMemoryDumpPtr global_dump(
      memory_instrumentation::mojom::GlobalMemoryDump::New());
  std::vector<base::flat_map<const char*, int64_t>> entries_metrics;
  for (const auto& ptype : entries_ptypes) {
    auto expected_metrics = GetExpectedProcessMetrics(ptype);
    PopulateMetrics(global_dump, ptype, expected_metrics);
    entries_metrics.push_back(expected_metrics);
  }

  scoped_refptr<ProcessMemoryMetricsEmitterFake> emitter(
      new ProcessMemoryMetricsEmitterFake(test_ukm_recorder_));
  emitter->ReceivedProcessInfos(ProcessInfoVector());
  emitter->ReceivedMemoryDump(
      true, GlobalMemoryDump::MoveFrom(std::move(global_dump)));

  CheckMemoryUkmEntryMetrics(entries_metrics);
}

TEST_F(ProcessMemoryMetricsEmitterTest, CollectsManyProcessUKMsManyDumps) {
  std::vector<std::vector<ProcessType>> entries_ptypes = {
      {ProcessType::BROWSER, ProcessType::RENDERER, ProcessType::GPU},
      {ProcessType::GPU, ProcessType::RENDERER, ProcessType::BROWSER},
  };

  std::vector<base::flat_map<const char*, int64_t>> entries_metrics;
  for (int i = 0; i < 2; ++i) {
    scoped_refptr<ProcessMemoryMetricsEmitterFake> emitter(
        new ProcessMemoryMetricsEmitterFake(test_ukm_recorder_));
    GlobalMemoryDumpPtr global_dump(
        memory_instrumentation::mojom::GlobalMemoryDump::New());
    for (const auto& ptype : entries_ptypes[i]) {
      auto expected_metrics = GetExpectedProcessMetrics(ptype);
      PopulateMetrics(global_dump, ptype, expected_metrics);
      expected_metrics.erase("TimeSinceLastVisible");
      entries_metrics.push_back(expected_metrics);
    }
    emitter->ReceivedProcessInfos(ProcessInfoVector());
    emitter->ReceivedMemoryDump(
        true, GlobalMemoryDump::MoveFrom(std::move(global_dump)));
  }

  CheckMemoryUkmEntryMetrics(entries_metrics, 2u);
}

TEST_F(ProcessMemoryMetricsEmitterTest, ReceiveProcessInfoFirst) {
  GlobalMemoryDumpPtr global_dump(
      memory_instrumentation::mojom::GlobalMemoryDump::New());
  base::flat_map<const char*, int64_t> expected_metrics =
      GetExpectedRendererMetrics();
  AddPageMetrics(expected_metrics);
  PopulateRendererMetrics(global_dump, expected_metrics, 201);

  scoped_refptr<ProcessMemoryMetricsEmitterFake> emitter(
      new ProcessMemoryMetricsEmitterFake(test_ukm_recorder_));
  emitter->ReceivedProcessInfos(GetProcessInfo(test_ukm_recorder_));
  emitter->ReceivedMemoryDump(
      true, GlobalMemoryDump::MoveFrom(std::move(global_dump)));

  auto entries = test_ukm_recorder_.GetEntriesByName(UkmEntry::kEntryName);
  ASSERT_EQ(entries.size(), 2u);
  int total_memory_entries = 0;
  for (const auto* const entry : entries) {
    if (test_ukm_recorder_.EntryHasMetric(
            entry, UkmEntry::kTotal2_PrivateMemoryFootprintName)) {
      total_memory_entries++;
    } else {
      test_ukm_recorder_.ExpectEntrySourceHasUrl(
          entry, GURL("http://www.url201.com/"));
    }
  }
  EXPECT_EQ(1, total_memory_entries);

  std::vector<base::flat_map<const char*, int64_t>> expected_entries;
  expected_entries.push_back(expected_metrics);
  CheckMemoryUkmEntryMetrics(expected_entries);
}

TEST_F(ProcessMemoryMetricsEmitterTest, ReceiveProcessInfoSecond) {
  GlobalMemoryDumpPtr global_dump(
      memory_instrumentation::mojom::GlobalMemoryDump::New());
  base::flat_map<const char*, int64_t> expected_metrics =
      GetExpectedRendererMetrics();
  AddPageMetrics(expected_metrics);
  PopulateRendererMetrics(global_dump, expected_metrics, 201);

  scoped_refptr<ProcessMemoryMetricsEmitterFake> emitter(
      new ProcessMemoryMetricsEmitterFake(test_ukm_recorder_));
  emitter->ReceivedMemoryDump(
      true, GlobalMemoryDump::MoveFrom(std::move(global_dump)));
  emitter->ReceivedProcessInfos(GetProcessInfo(test_ukm_recorder_));

  auto entries = test_ukm_recorder_.GetEntriesByName(UkmEntry::kEntryName);
  ASSERT_EQ(entries.size(), 2u);
  int total_memory_entries = 0;
  for (const auto* const entry : entries) {
    if (test_ukm_recorder_.EntryHasMetric(
            entry, UkmEntry::kTotal2_PrivateMemoryFootprintName)) {
      total_memory_entries++;
    } else {
      test_ukm_recorder_.ExpectEntrySourceHasUrl(
          entry, GURL("http://www.url201.com/"));
    }
  }
  EXPECT_EQ(1, total_memory_entries);

  std::vector<base::flat_map<const char*, int64_t>> expected_entries;
  expected_entries.push_back(expected_metrics);
  CheckMemoryUkmEntryMetrics(expected_entries);
}

TEST_F(ProcessMemoryMetricsEmitterTest, ProcessInfoHasTwoURLs) {
  GlobalMemoryDumpPtr global_dump(
      memory_instrumentation::mojom::GlobalMemoryDump::New());
  base::flat_map<const char*, int64_t> expected_metrics =
      GetExpectedRendererMetrics();
  PopulateRendererMetrics(global_dump, expected_metrics, 200);
  PopulateRendererMetrics(global_dump, expected_metrics, 201);
  PopulateRendererMetrics(global_dump, expected_metrics, 202);

  scoped_refptr<ProcessMemoryMetricsEmitterFake> emitter(
      new ProcessMemoryMetricsEmitterFake(test_ukm_recorder_));
  emitter->ReceivedMemoryDump(
      true, GlobalMemoryDump::MoveFrom(std::move(global_dump)));
  emitter->ReceivedProcessInfos(GetProcessInfo(test_ukm_recorder_));

  // Check that if there are two URLs, neither is emitted.
  auto entries = test_ukm_recorder_.GetEntriesByName(UkmEntry::kEntryName);
  int total_memory_entries = 0;
  int entries_with_urls = 0;
  for (const auto* const entry : entries) {
    if (test_ukm_recorder_.EntryHasMetric(
            entry, UkmEntry::kTotal2_PrivateMemoryFootprintName)) {
      total_memory_entries++;
    } else {
      if (test_ukm_recorder_.GetSourceForSourceId(entry->source_id)) {
        entries_with_urls++;
        test_ukm_recorder_.ExpectEntrySourceHasUrl(
            entry, GURL("http://www.url201.com/"));
      }
    }
  }
  EXPECT_EQ(4u, entries.size());
  EXPECT_EQ(1, total_memory_entries);
  EXPECT_EQ(1, entries_with_urls);
}
