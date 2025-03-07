// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file contains routines for gathering resource statistics for processes
// running on the system.

#ifndef BASE_PROCESS_PROCESS_METRICS_H_
#define BASE_PROCESS_PROCESS_METRICS_H_

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>

#include "base/base_export.h"
#include "base/gtest_prod_util.h"
#include "base/macros.h"
#include "base/process/process_handle.h"
#include "base/time/time.h"
#include "base/values.h"
#include "build/build_config.h"

#if defined(OS_MACOSX)
#include <mach/mach.h>
#include "base/process/port_provider_mac.h"

#if !defined(OS_IOS)
#include <mach/mach_vm.h>
#endif
#endif

#if defined(OS_WIN)
#include "base/win/scoped_handle.h"
#include "base/win/windows_types.h"
#endif

namespace base {

// Full declaration is in process_metrics_iocounters.h.
struct IoCounters;

// Working Set (resident) memory usage broken down by
//
// On Windows:
// priv (private): These pages (kbytes) cannot be shared with any other process.
// shareable:      These pages (kbytes) can be shared with other processes under
//                 the right circumstances.
// shared :        These pages (kbytes) are currently shared with at least one
//                 other process.
//
// On Linux:
// priv:           Pages mapped only by this process.
// shared:         PSS or 0 if the kernel doesn't support this.
// shareable:      0

// On ChromeOS:
// priv:           Pages mapped only by this process.
// shared:         PSS or 0 if the kernel doesn't support this.
// shareable:      0
// swapped         Pages swapped out to zram.
//
// On macOS:
// priv:           Resident size (RSS) including shared memory. Warning: This
//                 does not include compressed size and does not always
//                 accurately account for shared memory due to things like
//                 copy-on-write. TODO(erikchen): Revamp this with something
//                 more accurate.
// shared:         0
// shareable:      0
//
struct WorkingSetKBytes {
  WorkingSetKBytes() : priv(0), shareable(0), shared(0) {}
  size_t priv;
  size_t shareable;
  size_t shared;
#if defined(OS_CHROMEOS)
  size_t swapped;
#endif
};

// Committed (resident + paged) memory usage broken down by
// private: These pages cannot be shared with any other process.
// mapped:  These pages are mapped into the view of a section (backed by
//          pagefile.sys)
// image:   These pages are mapped into the view of an image section (backed by
//          file system)
struct CommittedKBytes {
  CommittedKBytes() : priv(0), mapped(0), image(0) {}
  size_t priv;
  size_t mapped;
  size_t image;
};

#if defined(OS_LINUX) || defined(OS_ANDROID)
// Minor and major page fault counts since the process creation.
// Both counts are process-wide, and exclude child processes.
//
// minor: Number of page faults that didn't require disk IO.
// major: Number of page faults that required disk IO.
struct PageFaultCounts {
  int64_t minor;
  int64_t major;
};
#endif  // defined(OS_LINUX) || defined(OS_ANDROID)

// Convert a POSIX timeval to microseconds.
BASE_EXPORT int64_t TimeValToMicroseconds(const struct timeval& tv);

// Provides performance metrics for a specified process (CPU usage, memory and
// IO counters). Use CreateCurrentProcessMetrics() to get an instance for the
// current process, or CreateProcessMetrics() to get an instance for an
// arbitrary process. Then, access the information with the different get
// methods.
class BASE_EXPORT ProcessMetrics {
 public:
  ~ProcessMetrics();

  // Creates a ProcessMetrics for the specified process.
#if !defined(OS_MACOSX) || defined(OS_IOS)
  static std::unique_ptr<ProcessMetrics> CreateProcessMetrics(
      ProcessHandle process);
#else

  // The port provider needs to outlive the ProcessMetrics object returned by
  // this function. If NULL is passed as provider, the returned object
  // only returns valid metrics if |process| is the current process.
  static std::unique_ptr<ProcessMetrics> CreateProcessMetrics(
      ProcessHandle process,
      PortProvider* port_provider);
#endif  // !defined(OS_MACOSX) || defined(OS_IOS)

  // Creates a ProcessMetrics for the current process. This a cross-platform
  // convenience wrapper for CreateProcessMetrics().
  static std::unique_ptr<ProcessMetrics> CreateCurrentProcessMetrics();

  // Returns private and sharedusage, in bytes. Private bytes is the amount of
  // memory currently allocated to a process that cannot be shared. Returns
  // false on platform specific error conditions.  Note: |private_bytes|
  // returns 0 on unsupported OSes: prior to XP SP2.
  bool GetMemoryBytes(size_t* private_bytes, size_t* shared_bytes) const;
  // Fills a CommittedKBytes with both resident and paged
  // memory usage as per definition of CommittedBytes.
  void GetCommittedKBytes(CommittedKBytes* usage) const;
  // Fills a WorkingSetKBytes containing resident private and shared memory
  // usage in bytes, as per definition of WorkingSetBytes. Note that this
  // function is somewhat expensive on Windows (a few ms per process).
  bool GetWorkingSetKBytes(WorkingSetKBytes* ws_usage) const;

#if defined(OS_LINUX) || defined(OS_ANDROID)
  // Resident Set Size is a Linux/Android specific memory concept. Do not
  // attempt to extend this to other platforms.
  BASE_EXPORT size_t GetResidentSetSize() const;
#endif

#if defined(OS_MACOSX)
  // Fills both CommitedKBytes and WorkingSetKBytes in a single operation. This
  // is more efficient on Mac OS X, as the two can be retrieved with a single
  // system call.
  bool GetCommittedAndWorkingSetKBytes(CommittedKBytes* usage,
                                       WorkingSetKBytes* ws_usage) const;

  struct TaskVMInfo {
    // Only available on macOS 10.12+.
    // Anonymous, non-discardable memory, including non-volatile IOKit.
    // Measured in bytes.
    uint64_t phys_footprint = 0;

    // Anonymous, non-discardable, non-compressed memory, excluding IOKit.
    // Measured in bytes.
    uint64_t internal = 0;

    // Compressed memory measured in bytes.
    uint64_t compressed = 0;
  };
  TaskVMInfo GetTaskVMInfo() const;

  // Returns private, shared, and total resident bytes. |locked_bytes| refers to
  // bytes that must stay resident. |locked_bytes| only counts bytes locked by
  // this task, not bytes locked by the kernel.
  bool GetMemoryBytes(size_t* private_bytes,
                      size_t* shared_bytes,
                      size_t* resident_bytes,
                      size_t* locked_bytes) const;
#endif

  // Returns the percentage of time spent executing, across all threads of the
  // process, in the interval since the last time the method was called. Since
  // this considers the total execution time across all threads in a process,
  // the result can easily exceed 100% in multi-thread processes running on
  // multi-core systems. In general the result is therefore a value in the
  // range 0% to SysInfo::NumberOfProcessors() * 100%.
  //
  // To obtain the percentage of total available CPU resources consumed by this
  // process over the interval, the caller must divide by NumberOfProcessors().
  //
  // Since this API measures usage over an interval, it will return zero on the
  // first call, and an actual value only on the second and subsequent calls.
  double GetPlatformIndependentCPUUsage();

  // Returns the number of average idle cpu wakeups per second since the last
  // call.
  int GetIdleWakeupsPerSecond();

#if defined(OS_MACOSX)
  // Returns the number of average "package idle exits" per second, which have
  // a higher energy impact than a regular wakeup, since the last call.
  //
  // From the powermetrics man page:
  // "With the exception of some Mac Pro systems, Mac and
  // iOS systems are typically single package systems, wherein all CPUs are
  // part of a single processor complex (typically a single IC die) with shared
  // logic that can include (depending on system specifics) shared last level
  // caches, an integrated memory controller etc. When all CPUs in the package
  // are idle, the hardware can power-gate significant portions of the shared
  // logic in addition to each individual processor's logic, as well as take
  // measures such as placing DRAM in to self-refresh (also referred to as
  // auto-refresh), place interconnects into lower-power states etc"
  int GetPackageIdleWakeupsPerSecond();
#endif

  // Retrieves accounting information for all I/O operations performed by the
  // process.
  // If IO information is retrieved successfully, the function returns true
  // and fills in the IO_COUNTERS passed in. The function returns false
  // otherwise.
  bool GetIOCounters(IoCounters* io_counters) const;

#if defined(OS_LINUX) || defined(OS_AIX) || defined(OS_ANDROID)
  // Returns the number of file descriptors currently open by the process, or
  // -1 on error.
  int GetOpenFdCount() const;

  // Returns the soft limit of file descriptors that can be opened by the
  // process, or -1 on error.
  int GetOpenFdSoftLimit() const;
#endif  // defined(OS_LINUX) || defined(OS_AIX) || defined(OS_ANDROID)

#if defined(OS_LINUX) || defined(OS_ANDROID)
  // Bytes of swap as reported by /proc/[pid]/status.
  uint64_t GetVmSwapBytes() const;

  // Minor and major page fault count as reported by /proc/[pid]/stat.
  // Returns true for success.
  bool GetPageFaultCounts(PageFaultCounts* counts) const;
#endif  // defined(OS_LINUX) || defined(OS_ANDROID)

  // Returns total memory usage of malloc.
  size_t GetMallocUsage();

 private:
#if !defined(OS_MACOSX) || defined(OS_IOS)
  explicit ProcessMetrics(ProcessHandle process);
#else
  ProcessMetrics(ProcessHandle process, PortProvider* port_provider);
#endif  // !defined(OS_MACOSX) || defined(OS_IOS)

#if defined(OS_LINUX) || defined(OS_ANDROID) | defined(OS_AIX)
  bool GetWorkingSetKBytesStatm(WorkingSetKBytes* ws_usage) const;
#endif

#if defined(OS_CHROMEOS)
  bool GetWorkingSetKBytesTotmaps(WorkingSetKBytes *ws_usage) const;
#endif

#if defined(OS_MACOSX) || defined(OS_LINUX) || defined(OS_AIX)
  int CalculateIdleWakeupsPerSecond(uint64_t absolute_idle_wakeups);
#endif
#if defined(OS_MACOSX)
  // The subset of wakeups that cause a "package exit" can be tracked on macOS.
  // See |GetPackageIdleWakeupsForSecond| comment for more info.
  int CalculatePackageIdleWakeupsPerSecond(
      uint64_t absolute_package_idle_wakeups);
#endif

#if defined(OS_WIN)
  win::ScopedHandle process_;
#else
  ProcessHandle process_;
#endif

  // Used to store the previous times and CPU usage counts so we can
  // compute the CPU usage between calls.
  TimeTicks last_cpu_time_;
  int64_t last_system_time_;

#if defined(OS_MACOSX) || defined(OS_LINUX) || defined(OS_AIX)
  // Same thing for idle wakeups.
  TimeTicks last_idle_wakeups_time_;
  uint64_t last_absolute_idle_wakeups_;
#endif

#if defined(OS_MACOSX)
  // And same thing for package idle exit wakeups.
  TimeTicks last_package_idle_wakeups_time_;
  uint64_t last_absolute_package_idle_wakeups_;
#endif

#if !defined(OS_IOS)
#if defined(OS_MACOSX)
  // Queries the port provider if it's set.
  mach_port_t TaskForPid(ProcessHandle process) const;

  PortProvider* port_provider_;
#elif defined(OS_POSIX)
  // Jiffie count at the last_cpu_time_ we updated.
  int last_cpu_;
#endif  // defined(OS_POSIX)
#endif  // !defined(OS_IOS)

  DISALLOW_COPY_AND_ASSIGN(ProcessMetrics);
};

// Returns the memory committed by the system in KBytes.
// Returns 0 if it can't compute the commit charge.
BASE_EXPORT size_t GetSystemCommitCharge();

// Returns the number of bytes in a memory page. Do not use this to compute
// the number of pages in a block of memory for calling mincore(). On some
// platforms, e.g. iOS, mincore() uses a different page size from what is
// returned by GetPageSize().
BASE_EXPORT size_t GetPageSize();

// Returns the maximum number of file descriptors that can be open by a process
// at once. If the number is unavailable, a conservative best guess is returned.
BASE_EXPORT size_t GetMaxFds();

#if defined(OS_POSIX) && !defined(OS_FUCHSIA)
// Sets the file descriptor soft limit to |max_descriptors| or the OS hard
// limit, whichever is lower.
BASE_EXPORT void SetFdLimit(unsigned int max_descriptors);
#endif  // defined(OS_POSIX)

#if defined(OS_WIN) || defined(OS_MACOSX) || defined(OS_LINUX) || \
    defined(OS_ANDROID) || defined(OS_AIX) || defined(OS_FUCHSIA)
// Data about system-wide memory consumption. Values are in KB. Available on
// Windows, Mac, Linux, Android and Chrome OS.
//
// Total memory are available on all platforms that implement
// GetSystemMemoryInfo(). Total/free swap memory are available on all platforms
// except on Mac. Buffers/cached/active_anon/inactive_anon/active_file/
// inactive_file/dirty/reclaimable/pswpin/pswpout/pgmajfault are available on
// Linux/Android/Chrome OS. Shmem/slab/gem_objects/gem_size are Chrome OS only.
// Speculative/file_backed/purgeable are Mac and iOS only.
// Free is absent on Windows (see "avail_phys" below).
struct BASE_EXPORT SystemMemoryInfoKB {
  SystemMemoryInfoKB();
  SystemMemoryInfoKB(const SystemMemoryInfoKB& other);

  // Serializes the platform specific fields to value.
  std::unique_ptr<DictionaryValue> ToValue() const;

  int total = 0;

#if !defined(OS_WIN)
  int free = 0;
#endif

#if defined(OS_WIN)
  // "This is the amount of physical memory that can be immediately reused
  // without having to write its contents to disk first. It is the sum of the
  // size of the standby, free, and zero lists." (MSDN).
  // Standby: not modified pages of physical ram (file-backed memory) that are
  // not actively being used.
  int avail_phys = 0;
#endif

#if defined(OS_LINUX) || defined(OS_ANDROID) || defined(OS_AIX)
  // This provides an estimate of available memory as described here:
  // https://git.kernel.org/cgit/linux/kernel/git/torvalds/linux.git/commit/?id=34e431b0ae398fc54ea69ff85ec700722c9da773
  // NOTE: this is ONLY valid in kernels 3.14 and up.  Its value will always
  // be 0 in earlier kernel versions.
  // Note: it includes _all_ file-backed memory (active + inactive).
  int available = 0;
#endif

#if !defined(OS_MACOSX)
  int swap_total = 0;
  int swap_free = 0;
#endif

#if defined(OS_ANDROID) || defined(OS_LINUX) || defined(OS_AIX) || \
    defined(OS_FUCHSIA)
  int buffers = 0;
  int cached = 0;
  int active_anon = 0;
  int inactive_anon = 0;
  int active_file = 0;
  int inactive_file = 0;
  int dirty = 0;
  int reclaimable = 0;
#endif  // defined(OS_ANDROID) || defined(OS_LINUX) || defined(OS_AIX) ||
        // defined(OS_FUCHSIA)

#if defined(OS_CHROMEOS)
  int shmem = 0;
  int slab = 0;
  // Gem data will be -1 if not supported.
  int gem_objects = -1;
  long long gem_size = -1;
#endif  // defined(OS_CHROMEOS)

#if defined(OS_MACOSX)
  int speculative = 0;
  int file_backed = 0;
  int purgeable = 0;
#endif  // defined(OS_MACOSX)
};

// On Linux/Android/Chrome OS, system-wide memory consumption data is parsed
// from /proc/meminfo and /proc/vmstat. On Windows/Mac, it is obtained using
// system API calls.
//
// Fills in the provided |meminfo| structure. Returns true on success.
// Exposed for memory debugging widget.
BASE_EXPORT bool GetSystemMemoryInfo(SystemMemoryInfoKB* meminfo);

#endif  // defined(OS_WIN) || defined(OS_MACOSX) || defined(OS_LINUX) ||
        // defined(OS_ANDROID) || defined(OS_AIX) || defined(OS_FUCHSIA)

#if defined(OS_LINUX) || defined(OS_ANDROID) || defined(OS_AIX)
// Parse the data found in /proc/<pid>/stat and return the sum of the
// CPU-related ticks.  Returns -1 on parse error.
// Exposed for testing.
BASE_EXPORT int ParseProcStatCPU(StringPiece input);

// Get the number of threads of |process| as available in /proc/<pid>/stat.
// This should be used with care as no synchronization with running threads is
// done. This is mostly useful to guarantee being single-threaded.
// Returns 0 on failure.
BASE_EXPORT int GetNumberOfThreads(ProcessHandle process);

// /proc/self/exe refers to the current executable.
BASE_EXPORT extern const char kProcSelfExe[];

// Parses a string containing the contents of /proc/meminfo
// returns true on success or false for a parsing error
// Exposed for testing.
BASE_EXPORT bool ParseProcMeminfo(StringPiece input,
                                  SystemMemoryInfoKB* meminfo);

// Data from /proc/vmstat.
struct BASE_EXPORT VmStatInfo {
  // Serializes the platform specific fields to value.
  std::unique_ptr<DictionaryValue> ToValue() const;

  unsigned long pswpin = 0;
  unsigned long pswpout = 0;
  unsigned long pgmajfault = 0;
};

// Retrieves data from /proc/vmstat about system-wide vm operations.
// Fills in the provided |vmstat| structure. Returns true on success.
BASE_EXPORT bool GetVmStatInfo(VmStatInfo* vmstat);

// Parses a string containing the contents of /proc/vmstat
// returns true on success or false for a parsing error
// Exposed for testing.
BASE_EXPORT bool ParseProcVmstat(StringPiece input, VmStatInfo* vmstat);

// Data from /proc/diskstats about system-wide disk I/O.
struct BASE_EXPORT SystemDiskInfo {
  SystemDiskInfo();
  SystemDiskInfo(const SystemDiskInfo& other);

  // Serializes the platform specific fields to value.
  std::unique_ptr<Value> ToValue() const;

  uint64_t reads = 0;
  uint64_t reads_merged = 0;
  uint64_t sectors_read = 0;
  uint64_t read_time = 0;
  uint64_t writes = 0;
  uint64_t writes_merged = 0;
  uint64_t sectors_written = 0;
  uint64_t write_time = 0;
  uint64_t io = 0;
  uint64_t io_time = 0;
  uint64_t weighted_io_time = 0;
};

// Checks whether the candidate string is a valid disk name, [hsv]d[a-z]+
// for a generic disk or mmcblk[0-9]+ for the MMC case.
// Names of disk partitions (e.g. sda1) are not valid.
BASE_EXPORT bool IsValidDiskName(StringPiece candidate);

// Retrieves data from /proc/diskstats about system-wide disk I/O.
// Fills in the provided |diskinfo| structure. Returns true on success.
BASE_EXPORT bool GetSystemDiskInfo(SystemDiskInfo* diskinfo);

// Returns the amount of time spent in user space since boot across all CPUs.
BASE_EXPORT TimeDelta GetUserCpuTimeSinceBoot();

#endif  // defined(OS_LINUX) || defined(OS_ANDROID)

#if defined(OS_CHROMEOS)
// Data from files in directory /sys/block/zram0 about ZRAM usage.
struct BASE_EXPORT SwapInfo {
  SwapInfo()
      : num_reads(0),
        num_writes(0),
        compr_data_size(0),
        orig_data_size(0),
        mem_used_total(0) {
  }

  // Serializes the platform specific fields to value.
  std::unique_ptr<Value> ToValue() const;

  uint64_t num_reads = 0;
  uint64_t num_writes = 0;
  uint64_t compr_data_size = 0;
  uint64_t orig_data_size = 0;
  uint64_t mem_used_total = 0;
};

// Parses a string containing the contents of /sys/block/zram0/mm_stat.
// This should be used for the new ZRAM sysfs interfaces.
// Returns true on success or false for a parsing error.
// Exposed for testing.
BASE_EXPORT bool ParseZramMmStat(StringPiece mm_stat_data, SwapInfo* swap_info);

// Parses a string containing the contents of /sys/block/zram0/stat
// This should be used for the new ZRAM sysfs interfaces.
// Returns true on success or false for a parsing error.
// Exposed for testing.
BASE_EXPORT bool ParseZramStat(StringPiece stat_data, SwapInfo* swap_info);

// In ChromeOS, reads files from /sys/block/zram0 that contain ZRAM usage data.
// Fills in the provided |swap_data| structure.
// Returns true on success or false for a parsing error.
BASE_EXPORT bool GetSwapInfo(SwapInfo* swap_info);
#endif  // defined(OS_CHROMEOS)

// Collects and holds performance metrics for system memory and disk.
// Provides functionality to retrieve the data on various platforms and
// to serialize the stored data.
class SystemMetrics {
 public:
  SystemMetrics();

  static SystemMetrics Sample();

  // Serializes the system metrics to value.
  std::unique_ptr<Value> ToValue() const;

 private:
  FRIEND_TEST_ALL_PREFIXES(SystemMetricsTest, SystemMetrics);

  size_t committed_memory_;
#if defined(OS_LINUX) || defined(OS_ANDROID)
  SystemMemoryInfoKB memory_info_;
  VmStatInfo vmstat_info_;
  SystemDiskInfo disk_info_;
#endif
#if defined(OS_CHROMEOS)
  SwapInfo swap_info_;
#endif
};

#if defined(OS_MACOSX) && !defined(OS_IOS)
enum class MachVMRegionResult {
  // There were no more memory regions between |address| and the end of the
  // virtual address space.
  Finished,

  // All output parameters are invalid.
  Error,

  // All output parameters are filled in.
  Success
};

// Returns info on the first memory region at or after |address|, including
// resident memory and share mode. On Success, |size| reflects the size of the
// memory region.
// |size| and |info| are output parameters, only valid on Success.
// |address| is an in-out parameter, than represents both the address to start
// looking, and the start address of the memory region.
BASE_EXPORT MachVMRegionResult GetTopInfo(mach_port_t task,
                                          mach_vm_size_t* size,
                                          mach_vm_address_t* address,
                                          vm_region_top_info_data_t* info);

// Returns info on the first memory region at or after |address|, including
// protection values. On Success, |size| reflects the size of the
// memory region.
// Returns info on the first memory region at or after |address|, including
// resident memory and share mode.
// |size| and |info| are output parameters, only valid on Success.
BASE_EXPORT MachVMRegionResult GetBasicInfo(mach_port_t task,
                                            mach_vm_size_t* size,
                                            mach_vm_address_t* address,
                                            vm_region_basic_info_64* info);
#endif  // defined(OS_MACOSX) && !defined(OS_IOS)

}  // namespace base

#endif  // BASE_PROCESS_PROCESS_METRICS_H_
