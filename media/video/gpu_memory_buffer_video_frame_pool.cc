// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/video/gpu_memory_buffer_video_frame_pool.h"

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <list>
#include <memory>
#include <utility>

#include "base/barrier_closure.h"
#include "base/bind.h"
#include "base/containers/circular_deque.h"
#include "base/containers/stack_container.h"
#include "base/location.h"
#include "base/macros.h"
#include "base/strings/stringprintf.h"
#include "base/time/default_tick_clock.h"
#include "base/trace_event/memory_dump_manager.h"
#include "base/trace_event/memory_dump_provider.h"
#include "base/trace_event/trace_event.h"
#include "gpu/GLES2/gl2extchromium.h"
#include "gpu/command_buffer/client/gles2_interface.h"
#include "media/base/bind_to_current_loop.h"
#include "media/video/gpu_video_accelerator_factories.h"
#include "third_party/libyuv/include/libyuv.h"
#include "ui/gfx/buffer_format_util.h"
#include "ui/gl/trace_util.h"

namespace media {

// Implementation of a pool of GpuMemoryBuffers used to back VideoFrames.
class GpuMemoryBufferVideoFramePool::PoolImpl
    : public base::RefCountedThreadSafe<
          GpuMemoryBufferVideoFramePool::PoolImpl>,
      public base::trace_event::MemoryDumpProvider {
 public:
  // |media_task_runner| is the media task runner associated with the
  // GL context provided by |gpu_factories|
  // |worker_task_runner| is a task runner used to asynchronously copy
  // video frame's planes.
  // |gpu_factories| is an interface to GPU related operation and can be
  // null if a GL context is not available.
  PoolImpl(const scoped_refptr<base::SingleThreadTaskRunner>& media_task_runner,
           const scoped_refptr<base::TaskRunner>& worker_task_runner,
           GpuVideoAcceleratorFactories* const gpu_factories)
      : media_task_runner_(media_task_runner),
        worker_task_runner_(worker_task_runner),
        gpu_factories_(gpu_factories),
        output_format_(GpuVideoAcceleratorFactories::OutputFormat::UNDEFINED),
        tick_clock_(base::DefaultTickClock::GetInstance()),
        in_shutdown_(false) {
    DCHECK(media_task_runner_);
    DCHECK(worker_task_runner_);
  }

  // Takes a software VideoFrame and calls |frame_ready_cb| with a VideoFrame
  // backed by native textures if possible.
  // The data contained in |video_frame| is copied into the returned frame
  // asynchronously posting tasks to |worker_task_runner_|, while
  // |frame_ready_cb| will be called on |media_task_runner_| once all the data
  // has been copied.
  void CreateHardwareFrame(const scoped_refptr<VideoFrame>& video_frame,
                           FrameReadyCB cb);

  bool OnMemoryDump(const base::trace_event::MemoryDumpArgs& args,
                    base::trace_event::ProcessMemoryDump* pmd) override;

  // Aborts any pending copies.
  void Abort();

  // Shuts down the frame pool and releases all frames in |frames_|.
  // Once this is called frames will no longer be inserted back into
  // |frames_|.
  void Shutdown();

  void SetTickClockForTesting(base::TickClock* tick_clock);

 private:
  friend class base::RefCountedThreadSafe<
      GpuMemoryBufferVideoFramePool::PoolImpl>;
  ~PoolImpl() override;

  // Resource to represent a plane.
  struct PlaneResource {
    gfx::Size size;
    std::unique_ptr<gfx::GpuMemoryBuffer> gpu_memory_buffer;
    unsigned texture_id = 0u;
    unsigned image_id = 0u;
    gpu::Mailbox mailbox;
  };

  // All the resources needed to compose a frame.
  // TODO(dalecurtis): The method of use marking used is very brittle
  // and prone to leakage. Switch this to pass around std::unique_ptr
  // such that callers own resources explicitly.
  struct FrameResources {
    explicit FrameResources(const gfx::Size& size) : size(size) {}
    void MarkUsed() {
      is_used_ = true;
      last_use_time_ = base::TimeTicks();
    }
    void MarkUnused(base::TimeTicks last_use_time) {
      is_used_ = false;
      last_use_time_ = last_use_time;
    }
    bool is_used() const { return is_used_; }
    base::TimeTicks last_use_time() const { return last_use_time_; }

    const gfx::Size size;
    PlaneResource plane_resources[VideoFrame::kMaxPlanes];

   private:
    bool is_used_ = true;
    base::TimeTicks last_use_time_;
  };

  // Struct to keep track of requested videoframe copies.
  struct VideoFrameCopyRequest {
    VideoFrameCopyRequest(scoped_refptr<VideoFrame> video_frame,
                          FrameReadyCB frame_ready_cb)
        : video_frame(video_frame), frame_ready_cb(std::move(frame_ready_cb)) {}
    scoped_refptr<VideoFrame> video_frame;
    FrameReadyCB frame_ready_cb;
  };

  // Start the copy of a video_frame on the worker_task_runner_.
  // It assumes there are currently no in-flight copies.
  void StartCopy(const scoped_refptr<VideoFrame>& video_frame);

  // Copy |video_frame| data into |frame_resources| and calls |frame_ready_cb|
  // when done.
  void CopyVideoFrameToGpuMemoryBuffers(
      const scoped_refptr<VideoFrame>& video_frame,
      FrameResources* frame_resources);

  // Called when all the data has been copied.
  void OnCopiesDone(const scoped_refptr<VideoFrame>& video_frame,
                    FrameResources* frame_resources);

  // Prepares GL resources, mailboxes and calls |frame_ready_cb| with the new
  // VideoFrame. This has to be run on |media_task_runner_| where
  // |frame_ready_cb| associated with video_frame will also be run.
  void BindAndCreateMailboxesHardwareFrameResources(
      const scoped_refptr<VideoFrame>& video_frame,
      FrameResources* frame_resources);

  // Return true if |resources| can be used to represent a frame for
  // specific |format| and |size|.
  static bool AreFrameResourcesCompatible(const FrameResources* resources,
                                          const gfx::Size& size) {
    return size == resources->size;
  }

  // Get the resources needed for a frame out of the pool, or create them if
  // necessary.
  // This also drops the LRU resources that can't be reuse for this frame.
  FrameResources* GetOrCreateFrameResources(
      const gfx::Size& size,
      GpuVideoAcceleratorFactories::OutputFormat format);

  // Callback called when a VideoFrame generated with GetFrameResources is no
  // longer referenced.
  // This must be called on the thread where |media_task_runner_| is current.
  void MailboxHoldersReleased(FrameResources* frame_resources,
                              const gpu::SyncToken& sync_token);

  // Delete resources. This has to be called on the thread where |task_runner|
  // is current.
  static void DeleteFrameResources(
      GpuVideoAcceleratorFactories* const gpu_factories,
      FrameResources* frame_resources);

  // Task runner associated to the GL context provided by |gpu_factories_|.
  const scoped_refptr<base::SingleThreadTaskRunner> media_task_runner_;
  // Task runner used to asynchronously copy planes.
  const scoped_refptr<base::TaskRunner> worker_task_runner_;

  // Interface to GPU related operations.
  GpuVideoAcceleratorFactories* const gpu_factories_;

  // Pool of resources.
  std::list<FrameResources*> resources_pool_;

  GpuVideoAcceleratorFactories::OutputFormat output_format_;

  // |tick_clock_| is always a DefaultTickClock outside of testing.
  base::TickClock* tick_clock_;

  // Queued up video frames for copies. The front is the currently
  // in-flight copy, new copies are added at the end.
  base::circular_deque<VideoFrameCopyRequest> frame_copy_requests_;
  bool in_shutdown_;

  DISALLOW_COPY_AND_ASSIGN(PoolImpl);
};

namespace {

// VideoFrame copies to GpuMemoryBuffers will be split in copies where the
// output size is |kBytesPerCopyTarget| bytes and run in parallel.
constexpr size_t kBytesPerCopyTarget = 1024 * 1024;  // 1MB

// Return the GpuMemoryBuffer format to use for a specific VideoPixelFormat
// and plane.
gfx::BufferFormat GpuMemoryBufferFormat(
    GpuVideoAcceleratorFactories::OutputFormat format,
    size_t plane) {
  switch (format) {
    case GpuVideoAcceleratorFactories::OutputFormat::I420:
      DCHECK_LE(plane, 2u);
      return gfx::BufferFormat::R_8;
    case GpuVideoAcceleratorFactories::OutputFormat::NV12_SINGLE_GMB:
      DCHECK_LE(plane, 1u);
      return gfx::BufferFormat::YUV_420_BIPLANAR;
    case GpuVideoAcceleratorFactories::OutputFormat::NV12_DUAL_GMB:
      DCHECK_LE(plane, 1u);
      return plane == 0 ? gfx::BufferFormat::R_8 : gfx::BufferFormat::RG_88;
    case GpuVideoAcceleratorFactories::OutputFormat::UYVY:
      DCHECK_EQ(0u, plane);
      return gfx::BufferFormat::UYVY_422;
    case GpuVideoAcceleratorFactories::OutputFormat::XR30:
      DCHECK_EQ(0u, plane);
      return gfx::BufferFormat::BGRX_1010102;
    case GpuVideoAcceleratorFactories::OutputFormat::XB30:
      DCHECK_EQ(0u, plane);
      return gfx::BufferFormat::RGBX_1010102;
    case GpuVideoAcceleratorFactories::OutputFormat::UNDEFINED:
      NOTREACHED();
      break;
  }
  return gfx::BufferFormat::BGRA_8888;
}

unsigned ImageInternalFormat(GpuVideoAcceleratorFactories::OutputFormat format,
                             size_t plane) {
  switch (format) {
    case GpuVideoAcceleratorFactories::OutputFormat::I420:
      DCHECK_LE(plane, 2u);
      return GL_RED_EXT;
    case GpuVideoAcceleratorFactories::OutputFormat::NV12_DUAL_GMB:
      DCHECK_LE(plane, 1u);
      return plane == 0 ? GL_RED_EXT : GL_RG_EXT;
    case GpuVideoAcceleratorFactories::OutputFormat::NV12_SINGLE_GMB:
      DCHECK_LE(plane, 1u);
      return GL_RGB_YCBCR_420V_CHROMIUM;
    case GpuVideoAcceleratorFactories::OutputFormat::UYVY:
      DCHECK_EQ(0u, plane);
      return GL_RGB_YCBCR_422_CHROMIUM;
    case GpuVideoAcceleratorFactories::OutputFormat::XR30:
    case GpuVideoAcceleratorFactories::OutputFormat::XB30:
      DCHECK_EQ(0u, plane);
      // Technically speaking we should say GL_RGB10_EXT, but that format is not
      // supported in OpenGLES.
      return GL_RGB10_A2_EXT;
    case GpuVideoAcceleratorFactories::OutputFormat::UNDEFINED:
      NOTREACHED();
      break;
  }
  return 0;
}

// The number of output planes to be copied in each iteration.
size_t PlanesPerCopy(GpuVideoAcceleratorFactories::OutputFormat format) {
  switch (format) {
    case GpuVideoAcceleratorFactories::OutputFormat::I420:
    case GpuVideoAcceleratorFactories::OutputFormat::UYVY:
      return 1;
    case GpuVideoAcceleratorFactories::OutputFormat::NV12_DUAL_GMB:
    case GpuVideoAcceleratorFactories::OutputFormat::NV12_SINGLE_GMB:
      return 2;
    case GpuVideoAcceleratorFactories::OutputFormat::XR30:
    case GpuVideoAcceleratorFactories::OutputFormat::XB30:
      return 3;
    case GpuVideoAcceleratorFactories::OutputFormat::UNDEFINED:
      NOTREACHED();
      break;
  }
  return 0;
}

VideoPixelFormat VideoFormat(
    GpuVideoAcceleratorFactories::OutputFormat format) {
  switch (format) {
    case GpuVideoAcceleratorFactories::OutputFormat::I420:
      return PIXEL_FORMAT_I420;
    case GpuVideoAcceleratorFactories::OutputFormat::NV12_SINGLE_GMB:
    case GpuVideoAcceleratorFactories::OutputFormat::NV12_DUAL_GMB:
      return PIXEL_FORMAT_NV12;
    case GpuVideoAcceleratorFactories::OutputFormat::UYVY:
      return PIXEL_FORMAT_UYVY;
    case GpuVideoAcceleratorFactories::OutputFormat::XR30:
      return PIXEL_FORMAT_ARGB;
    case GpuVideoAcceleratorFactories::OutputFormat::XB30:
      return PIXEL_FORMAT_RGB32;
    case GpuVideoAcceleratorFactories::OutputFormat::UNDEFINED:
      NOTREACHED();
      break;
  }
  return PIXEL_FORMAT_UNKNOWN;
}

// The number of output planes to be copied in each iteration.
size_t NumGpuMemoryBuffers(GpuVideoAcceleratorFactories::OutputFormat format) {
  switch (format) {
    case GpuVideoAcceleratorFactories::OutputFormat::I420:
      return 3;
    case GpuVideoAcceleratorFactories::OutputFormat::NV12_SINGLE_GMB:
      return 1;
    case GpuVideoAcceleratorFactories::OutputFormat::NV12_DUAL_GMB:
      return 2;
    case GpuVideoAcceleratorFactories::OutputFormat::UYVY:
      return 1;
    case GpuVideoAcceleratorFactories::OutputFormat::XR30:
    case GpuVideoAcceleratorFactories::OutputFormat::XB30:
      return 1;
    case GpuVideoAcceleratorFactories::OutputFormat::UNDEFINED:
      NOTREACHED();
      break;
  }
  NOTREACHED();
  return 0;
}

// The number of output rows to be copied in each iteration.
int RowsPerCopy(size_t plane, VideoPixelFormat format, int width) {
  int bytes_per_row = VideoFrame::RowBytes(plane, format, width);
  if (format == PIXEL_FORMAT_NV12) {
    DCHECK_EQ(0u, plane);
    bytes_per_row += VideoFrame::RowBytes(1, format, width);
  }
  // Copy an even number of lines, and at least one.
  return std::max<size_t>((kBytesPerCopyTarget / bytes_per_row) & ~1, 1);
}

void CopyRowsToI420Buffer(int first_row,
                          int rows,
                          int bytes_per_row,
                          size_t bit_depth,
                          const uint8_t* source,
                          int source_stride,
                          uint8_t* output,
                          int dest_stride,
                          base::OnceClosure done) {
  TRACE_EVENT2("media", "CopyRowsToI420Buffer", "bytes_per_row", bytes_per_row,
               "rows", rows);
  if (output) {
    DCHECK_NE(dest_stride, 0);
    DCHECK_LE(bytes_per_row, std::abs(dest_stride));
    DCHECK_LE(bytes_per_row, source_stride);
    DCHECK_GE(bit_depth, 8u);

    if (bit_depth == 8) {
      libyuv::CopyPlane(source + source_stride * first_row, source_stride,
                        output + dest_stride * first_row, dest_stride,
                        bytes_per_row, rows);
    } else {
      const int scale = 0x10000 >> (bit_depth - 8);
      libyuv::Convert16To8Plane(
          reinterpret_cast<const uint16*>(source + source_stride * first_row),
          source_stride / 2, output + dest_stride * first_row, dest_stride,
          scale, bytes_per_row, rows);
    }
  }
  std::move(done).Run();
}

void CopyRowsToNV12Buffer(int first_row,
                          int rows,
                          int bytes_per_row,
                          const scoped_refptr<VideoFrame>& source_frame,
                          uint8_t* dest_y,
                          int dest_stride_y,
                          uint8_t* dest_uv,
                          int dest_stride_uv,
                          base::OnceClosure done) {
  TRACE_EVENT2("media", "CopyRowsToNV12Buffer", "bytes_per_row", bytes_per_row,
               "rows", rows);
  if (dest_y && dest_uv) {
    DCHECK_NE(dest_stride_y, 0);
    DCHECK_NE(dest_stride_uv, 0);
    DCHECK_LE(bytes_per_row, std::abs(dest_stride_y));
    DCHECK_LE(bytes_per_row, std::abs(dest_stride_uv));
    DCHECK_EQ(0, first_row % 2);

    libyuv::I420ToNV12(
        source_frame->visible_data(VideoFrame::kYPlane) +
            first_row * source_frame->stride(VideoFrame::kYPlane),
        source_frame->stride(VideoFrame::kYPlane),
        source_frame->visible_data(VideoFrame::kUPlane) +
            first_row / 2 * source_frame->stride(VideoFrame::kUPlane),
        source_frame->stride(VideoFrame::kUPlane),
        source_frame->visible_data(VideoFrame::kVPlane) +
            first_row / 2 * source_frame->stride(VideoFrame::kVPlane),
        source_frame->stride(VideoFrame::kVPlane),
        dest_y + first_row * dest_stride_y, dest_stride_y,
        dest_uv + first_row / 2 * dest_stride_uv, dest_stride_uv, bytes_per_row,
        rows);
  }
  std::move(done).Run();
}

void CopyRowsToUYVYBuffer(int first_row,
                          int rows,
                          int width,
                          const scoped_refptr<VideoFrame>& source_frame,
                          uint8_t* output,
                          int dest_stride,
                          base::OnceClosure done) {
  TRACE_EVENT2("media", "CopyRowsToUYVYBuffer", "bytes_per_row", width * 2,
               "rows", rows);
  if (output) {
    DCHECK_NE(dest_stride, 0);
    DCHECK_LE(width, std::abs(dest_stride / 2));
    DCHECK_EQ(0, first_row % 2);
    libyuv::I420ToUYVY(
        source_frame->visible_data(VideoFrame::kYPlane) +
            first_row * source_frame->stride(VideoFrame::kYPlane),
        source_frame->stride(VideoFrame::kYPlane),
        source_frame->visible_data(VideoFrame::kUPlane) +
            first_row / 2 * source_frame->stride(VideoFrame::kUPlane),
        source_frame->stride(VideoFrame::kUPlane),
        source_frame->visible_data(VideoFrame::kVPlane) +
            first_row / 2 * source_frame->stride(VideoFrame::kVPlane),
        source_frame->stride(VideoFrame::kVPlane),
        output + first_row * dest_stride, dest_stride, width, rows);
  }
  std::move(done).Run();
}

void CopyRowsToRGB10Buffer(bool is_argb,
                           int first_row,
                           int rows,
                           int width,
                           const scoped_refptr<VideoFrame>& source_frame,
                           uint8_t* output,
                           int dest_stride,
                           base::OnceClosure done) {
  TRACE_EVENT2("media", "CopyRowsToXR30Buffer", "bytes_per_row", width * 2,
               "rows", rows);
  if (output) {
    DCHECK_NE(dest_stride, 0);
    DCHECK_LE(width, std::abs(dest_stride / 2));
    DCHECK_EQ(0, first_row % 2);

    int color_space = COLOR_SPACE_UNSPECIFIED;
    if (source_frame->metadata()->GetInteger(VideoFrameMetadata::COLOR_SPACE,
                                             &color_space)) {
      color_space = COLOR_SPACE_UNSPECIFIED;
    }
    const uint16_t* y_plane = reinterpret_cast<const uint16_t*>(
        source_frame->visible_data(VideoFrame::kYPlane) +
        first_row * source_frame->stride(VideoFrame::kYPlane));
    const size_t y_plane_stride = source_frame->stride(VideoFrame::kYPlane) / 2;
    const uint16_t* v_plane = reinterpret_cast<const uint16_t*>(
        source_frame->visible_data(VideoFrame::kVPlane) +
        first_row / 2 * source_frame->stride(VideoFrame::kVPlane));
    const size_t v_plane_stride = source_frame->stride(VideoFrame::kVPlane) / 2;
    const uint16_t* u_plane = reinterpret_cast<const uint16_t*>(
        source_frame->visible_data(VideoFrame::kUPlane) +
        first_row / 2 * source_frame->stride(VideoFrame::kUPlane));
    const size_t u_plane_stride = source_frame->stride(VideoFrame::kUPlane) / 2;
    uint8_t* dest_ar30 = output + first_row * dest_stride;

    switch (color_space) {
      case COLOR_SPACE_HD_REC709:
        if (is_argb) {
          libyuv::H010ToAR30(y_plane, y_plane_stride, u_plane, u_plane_stride,
                             v_plane, v_plane_stride, dest_ar30, dest_stride,
                             width, rows);
        } else {
          libyuv::H010ToAB30(y_plane, y_plane_stride, u_plane, u_plane_stride,
                             v_plane, v_plane_stride, dest_ar30, dest_stride,
                             width, rows);
        }
        break;
      case COLOR_SPACE_UNSPECIFIED:
      case COLOR_SPACE_JPEG:
      case COLOR_SPACE_SD_REC601:
        if (is_argb) {
          libyuv::I010ToAR30(y_plane, y_plane_stride, u_plane, u_plane_stride,
                             v_plane, v_plane_stride, dest_ar30, dest_stride,
                             width, rows);
        } else {
          libyuv::I010ToAB30(y_plane, y_plane_stride, u_plane, u_plane_stride,
                             v_plane, v_plane_stride, dest_ar30, dest_stride,
                             width, rows);
        }
        break;
    }
  }
  std::move(done).Run();
}

gfx::Size CodedSize(const scoped_refptr<VideoFrame>& video_frame,
                    GpuVideoAcceleratorFactories::OutputFormat output_format) {
  DCHECK(gfx::Rect(video_frame->coded_size())
             .Contains(video_frame->visible_rect()));
  DCHECK((video_frame->visible_rect().x() & 1) == 0);
  gfx::Size output;
  switch (output_format) {
    case GpuVideoAcceleratorFactories::OutputFormat::I420:
    case GpuVideoAcceleratorFactories::OutputFormat::NV12_SINGLE_GMB:
    case GpuVideoAcceleratorFactories::OutputFormat::NV12_DUAL_GMB:
      DCHECK((video_frame->visible_rect().y() & 1) == 0);
      output = gfx::Size((video_frame->visible_rect().width() + 1) & ~1,
                         (video_frame->visible_rect().height() + 1) & ~1);
      break;
    case GpuVideoAcceleratorFactories::OutputFormat::UYVY:
    case GpuVideoAcceleratorFactories::OutputFormat::XR30:
    case GpuVideoAcceleratorFactories::OutputFormat::XB30:
      output = gfx::Size((video_frame->visible_rect().width() + 1) & ~1,
                         video_frame->visible_rect().height());
      break;
    case GpuVideoAcceleratorFactories::OutputFormat::UNDEFINED:
      NOTREACHED();
  }
  DCHECK(gfx::Rect(video_frame->coded_size()).Contains(gfx::Rect(output)));
  return output;
}
}  // unnamed namespace

// Creates a VideoFrame backed by native textures starting from a software
// VideoFrame.
// The data contained in |video_frame| is copied into the VideoFrame passed to
// |frame_ready_cb|.
// This has to be called on the thread where |media_task_runner_| is current.
void GpuMemoryBufferVideoFramePool::PoolImpl::CreateHardwareFrame(
    const scoped_refptr<VideoFrame>& video_frame,
    FrameReadyCB frame_ready_cb) {
  DCHECK(media_task_runner_->BelongsToCurrentThread());
  // Lazily initialize output_format_ since VideoFrameOutputFormat() has to be
  // called on the media_thread while this object might be instantiated on any.
  if (output_format_ == GpuVideoAcceleratorFactories::OutputFormat::UNDEFINED) {
    output_format_ =
        gpu_factories_->VideoFrameOutputFormat(video_frame->BitDepth());
  }

  if (output_format_ == GpuVideoAcceleratorFactories::OutputFormat::UNDEFINED) {
    std::move(frame_ready_cb).Run(video_frame);
    return;
  }
  switch (video_frame->format()) {
    // Supported cases.
    case PIXEL_FORMAT_YV12:
    case PIXEL_FORMAT_I420:
    case PIXEL_FORMAT_YUV420P9:
    case PIXEL_FORMAT_YUV420P10:
    case PIXEL_FORMAT_YUV420P12:
      break;
    // Unsupported cases.
    case PIXEL_FORMAT_I420A:
    case PIXEL_FORMAT_I422:
    case PIXEL_FORMAT_I444:
    case PIXEL_FORMAT_NV12:
    case PIXEL_FORMAT_NV21:
    case PIXEL_FORMAT_UYVY:
    case PIXEL_FORMAT_YUY2:
    case PIXEL_FORMAT_ARGB:
    case PIXEL_FORMAT_XRGB:
    case PIXEL_FORMAT_RGB24:
    case PIXEL_FORMAT_RGB32:
    case PIXEL_FORMAT_MJPEG:
    case PIXEL_FORMAT_MT21:
    case PIXEL_FORMAT_YUV422P9:
    case PIXEL_FORMAT_YUV444P9:
    case PIXEL_FORMAT_YUV422P10:
    case PIXEL_FORMAT_YUV444P10:
    case PIXEL_FORMAT_YUV422P12:
    case PIXEL_FORMAT_YUV444P12:
    case PIXEL_FORMAT_Y16:
    case PIXEL_FORMAT_UNKNOWN:
      std::move(frame_ready_cb).Run(video_frame);
      return;
  }

  frame_copy_requests_.emplace_back(video_frame, std::move(frame_ready_cb));
  if (frame_copy_requests_.size() == 1u)
    StartCopy(video_frame);
}

bool GpuMemoryBufferVideoFramePool::PoolImpl::OnMemoryDump(
    const base::trace_event::MemoryDumpArgs& args,
    base::trace_event::ProcessMemoryDump* pmd) {
  const uint64_t tracing_process_id =
      base::trace_event::MemoryDumpManager::GetInstance()
          ->GetTracingProcessId();
  const int kImportance = 2;
  for (const FrameResources* frame_resources : resources_pool_) {
    for (const PlaneResource& plane_resource :
         frame_resources->plane_resources) {
      if (plane_resource.gpu_memory_buffer) {
        gfx::GpuMemoryBufferId buffer_id =
            plane_resource.gpu_memory_buffer->GetId();
        std::string dump_name = base::StringPrintf(
            "media/video_frame_memory/buffer_%d", buffer_id.id);
        base::trace_event::MemoryAllocatorDump* dump =
            pmd->CreateAllocatorDump(dump_name);
        size_t buffer_size_in_bytes = gfx::BufferSizeForBufferFormat(
            plane_resource.size, plane_resource.gpu_memory_buffer->GetFormat());
        dump->AddScalar(base::trace_event::MemoryAllocatorDump::kNameSize,
                        base::trace_event::MemoryAllocatorDump::kUnitsBytes,
                        buffer_size_in_bytes);
        dump->AddScalar("free_size",
                        base::trace_event::MemoryAllocatorDump::kUnitsBytes,
                        frame_resources->is_used() ? 0 : buffer_size_in_bytes);
        auto shared_memory_guid =
            plane_resource.gpu_memory_buffer->GetHandle().handle.GetGUID();
        if (!shared_memory_guid.is_empty()) {
          pmd->CreateSharedMemoryOwnershipEdge(dump->guid(), shared_memory_guid,
                                               kImportance);
        } else {
          auto shared_buffer_guid =
              plane_resource.gpu_memory_buffer->GetGUIDForTracing(
                  tracing_process_id);
          pmd->CreateSharedGlobalAllocatorDump(shared_buffer_guid);
          pmd->AddOwnershipEdge(dump->guid(), shared_buffer_guid, kImportance);
        }
      }
    }
  }
  return true;
}

void GpuMemoryBufferVideoFramePool::PoolImpl::Abort() {
  DCHECK(media_task_runner_->BelongsToCurrentThread());
  // Abort any pending copy requests. If one is already in flight, we can't do
  // anything about it.
  if (frame_copy_requests_.size() <= 1u)
    return;
  frame_copy_requests_.erase(frame_copy_requests_.begin() + 1,
                             frame_copy_requests_.end());
}

void GpuMemoryBufferVideoFramePool::PoolImpl::OnCopiesDone(
    const scoped_refptr<VideoFrame>& video_frame,
    FrameResources* frame_resources) {
  for (const auto& plane_resource : frame_resources->plane_resources) {
    if (plane_resource.gpu_memory_buffer) {
      plane_resource.gpu_memory_buffer->Unmap();
      plane_resource.gpu_memory_buffer->SetColorSpace(
          video_frame->ColorSpace());
    }
  }

  TRACE_EVENT_ASYNC_END0("media", "CopyVideoFrameToGpuMemoryBuffers",
                         video_frame->timestamp().InNanoseconds() /* id */);

  media_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&PoolImpl::BindAndCreateMailboxesHardwareFrameResources,
                     this, video_frame, frame_resources));
}

void GpuMemoryBufferVideoFramePool::PoolImpl::StartCopy(
    const scoped_refptr<VideoFrame>& video_frame) {
  DCHECK(media_task_runner_->BelongsToCurrentThread());
  DCHECK(!frame_copy_requests_.empty());

  const gfx::Size coded_size = CodedSize(video_frame, output_format_);
  // Acquire resources. Incompatible ones will be dropped from the pool.
  FrameResources* frame_resources =
      GetOrCreateFrameResources(coded_size, output_format_);
  if (!frame_resources) {
    std::move(frame_copy_requests_.front().frame_ready_cb).Run(video_frame);
    frame_copy_requests_.pop_front();
    return;
  }

  worker_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&PoolImpl::CopyVideoFrameToGpuMemoryBuffers,
                                this, video_frame, frame_resources));
}

// Copies |video_frame| into |frame_resources| asynchronously, posting n tasks
// that will be synchronized by a barrier.
// After the barrier is passed OnCopiesDone will be called.
void GpuMemoryBufferVideoFramePool::PoolImpl::CopyVideoFrameToGpuMemoryBuffers(
    const scoped_refptr<VideoFrame>& video_frame,
    FrameResources* frame_resources) {
  // Compute the number of tasks to post and create the barrier.
  const size_t num_planes = VideoFrame::NumPlanes(VideoFormat(output_format_));
  const size_t planes_per_copy = PlanesPerCopy(output_format_);
  const gfx::Size coded_size = CodedSize(video_frame, output_format_);
  size_t copies = 0;
  for (size_t i = 0; i < num_planes; i += planes_per_copy) {
    const int rows =
        VideoFrame::Rows(i, VideoFormat(output_format_), coded_size.height());
    const int rows_per_copy =
        RowsPerCopy(i, VideoFormat(output_format_), coded_size.width());
    copies += rows / rows_per_copy;
    if (rows % rows_per_copy)
      ++copies;
  }

  const base::RepeatingClosure barrier = base::BarrierClosure(
      copies, base::BindOnce(&PoolImpl::OnCopiesDone, this, video_frame,
                             frame_resources));

  // Map the buffers.
  for (size_t i = 0; i < NumGpuMemoryBuffers(output_format_); i++) {
    gfx::GpuMemoryBuffer* buffer =
        frame_resources->plane_resources[i].gpu_memory_buffer.get();

    if (!buffer || !buffer->Map()) {
      DLOG(ERROR) << "Could not get or Map() buffer";
      frame_resources->MarkUnused(tick_clock_->NowTicks());
      return;
    }
  }

  TRACE_EVENT_ASYNC_BEGIN0("media", "CopyVideoFrameToGpuMemoryBuffers",
                           video_frame->timestamp().InNanoseconds() /* id */);
  // Post all the async tasks.
  for (size_t i = 0; i < num_planes; i += planes_per_copy) {
    gfx::GpuMemoryBuffer* buffer =
        frame_resources->plane_resources[i].gpu_memory_buffer.get();
    const int rows =
        VideoFrame::Rows(i, VideoFormat(output_format_), coded_size.height());
    const int rows_per_copy =
        RowsPerCopy(i, VideoFormat(output_format_), coded_size.width());

    for (int row = 0; row < rows; row += rows_per_copy) {
      const int rows_to_copy = std::min(rows_per_copy, rows - row);
      switch (output_format_) {
        case GpuVideoAcceleratorFactories::OutputFormat::I420: {
          const int bytes_per_row = VideoFrame::RowBytes(
              i, VideoFormat(output_format_), coded_size.width());
          worker_task_runner_->PostTask(
              FROM_HERE,
              base::BindOnce(&CopyRowsToI420Buffer, row, rows_to_copy,
                             bytes_per_row, video_frame->BitDepth(),
                             video_frame->visible_data(i),
                             video_frame->stride(i),
                             static_cast<uint8_t*>(buffer->memory(0)),
                             buffer->stride(0), barrier));
          break;
        }
        case GpuVideoAcceleratorFactories::OutputFormat::NV12_SINGLE_GMB:
          worker_task_runner_->PostTask(
              FROM_HERE,
              base::BindOnce(
                  &CopyRowsToNV12Buffer, row, rows_to_copy, coded_size.width(),
                  video_frame, static_cast<uint8_t*>(buffer->memory(0)),
                  buffer->stride(0), static_cast<uint8_t*>(buffer->memory(1)),
                  buffer->stride(1), barrier));
          break;
        case GpuVideoAcceleratorFactories::OutputFormat::NV12_DUAL_GMB: {
          gfx::GpuMemoryBuffer* buffer2 =
              frame_resources->plane_resources[1].gpu_memory_buffer.get();
          worker_task_runner_->PostTask(
              FROM_HERE,
              base::BindOnce(
                  &CopyRowsToNV12Buffer, row, rows_to_copy, coded_size.width(),
                  video_frame, static_cast<uint8_t*>(buffer->memory(0)),
                  buffer->stride(0), static_cast<uint8_t*>(buffer2->memory(0)),
                  buffer2->stride(0), barrier));
          break;
        }

        case GpuVideoAcceleratorFactories::OutputFormat::UYVY:
          worker_task_runner_->PostTask(
              FROM_HERE,
              base::BindOnce(&CopyRowsToUYVYBuffer, row, rows_to_copy,
                             coded_size.width(), video_frame,
                             static_cast<uint8_t*>(buffer->memory(0)),
                             buffer->stride(0), barrier));
          break;

        case GpuVideoAcceleratorFactories::OutputFormat::XR30:
        case GpuVideoAcceleratorFactories::OutputFormat::XB30: {
          const bool is_argb = output_format_ ==
                               GpuVideoAcceleratorFactories::OutputFormat::XR30;
          worker_task_runner_->PostTask(
              FROM_HERE,
              base::BindOnce(&CopyRowsToRGB10Buffer, is_argb, row, rows_to_copy,
                             coded_size.width(), video_frame,
                             static_cast<uint8_t*>(buffer->memory(0)),
                             buffer->stride(0), barrier));
          break;
        }
        case GpuVideoAcceleratorFactories::OutputFormat::UNDEFINED:
          NOTREACHED();
      }
    }
  }
}

void GpuMemoryBufferVideoFramePool::PoolImpl::
    BindAndCreateMailboxesHardwareFrameResources(
        const scoped_refptr<VideoFrame>& video_frame,
        FrameResources* frame_resources) {
  gpu::gles2::GLES2Interface* gles2 = gpu_factories_->ContextGL();
  if (!gles2) {
    frame_resources->MarkUnused(tick_clock_->NowTicks());
    std::move(frame_copy_requests_.front().frame_ready_cb).Run(video_frame);
    frame_copy_requests_.pop_front();
    return;
  }

  const gfx::Size coded_size = CodedSize(video_frame, output_format_);
  gpu::MailboxHolder mailbox_holders[VideoFrame::kMaxPlanes];
  // Set up the planes creating the mailboxes needed to refer to the textures.
  for (size_t i = 0; i < NumGpuMemoryBuffers(output_format_); i++) {
    PlaneResource& plane_resource = frame_resources->plane_resources[i];
    const gfx::BufferFormat buffer_format =
        GpuMemoryBufferFormat(output_format_, i);
    unsigned texture_target = gpu_factories_->ImageTextureTarget(buffer_format);
    // Bind the texture and create or rebind the image.
    gles2->BindTexture(texture_target, plane_resource.texture_id);
    if (plane_resource.gpu_memory_buffer && !plane_resource.image_id) {
      const size_t width = VideoFrame::Columns(i, VideoFormat(output_format_),
                                               coded_size.width());
      const size_t height =
          VideoFrame::Rows(i, VideoFormat(output_format_), coded_size.height());
      plane_resource.image_id = gles2->CreateImageCHROMIUM(
          plane_resource.gpu_memory_buffer->AsClientBuffer(), width, height,
          ImageInternalFormat(output_format_, i));
    } else if (plane_resource.image_id) {
      gles2->ReleaseTexImage2DCHROMIUM(texture_target, plane_resource.image_id);
    }
    if (plane_resource.image_id)
      gles2->BindTexImage2DCHROMIUM(texture_target, plane_resource.image_id);
    mailbox_holders[i] = gpu::MailboxHolder(plane_resource.mailbox,
                                            gpu::SyncToken(), texture_target);
  }

  // Insert a sync_token, this is needed to make sure that the textures the
  // mailboxes refer to will be used only after all the previous commands posted
  // in the command buffer have been processed.
  gpu::SyncToken sync_token;
  gles2->GenUnverifiedSyncTokenCHROMIUM(sync_token.GetData());
  for (size_t i = 0; i < NumGpuMemoryBuffers(output_format_); i++)
    mailbox_holders[i].sync_token = sync_token;

  VideoPixelFormat frame_format = VideoFormat(output_format_);

  // Create the VideoFrame backed by native textures.
  gfx::Size visible_size = video_frame->visible_rect().size();
  scoped_refptr<VideoFrame> frame = VideoFrame::WrapNativeTextures(
      frame_format, mailbox_holders, VideoFrame::ReleaseMailboxCB(), coded_size,
      gfx::Rect(visible_size), video_frame->natural_size(),
      video_frame->timestamp());

  if (!frame) {
    frame_resources->MarkUnused(tick_clock_->NowTicks());
    MailboxHoldersReleased(frame_resources, gpu::SyncToken());
    std::move(frame_copy_requests_.front().frame_ready_cb).Run(video_frame);
    frame_copy_requests_.pop_front();
    return;
  }
  frame->SetReleaseMailboxCB(
      base::BindOnce(&PoolImpl::MailboxHoldersReleased, this, frame_resources));

  frame->set_color_space(video_frame->ColorSpace());

  bool allow_overlay = false;
  switch (output_format_) {
    case GpuVideoAcceleratorFactories::OutputFormat::I420:
      allow_overlay =
          video_frame->metadata()->IsTrue(VideoFrameMetadata::ALLOW_OVERLAY);
      break;
    case GpuVideoAcceleratorFactories::OutputFormat::NV12_SINGLE_GMB:
    case GpuVideoAcceleratorFactories::OutputFormat::UYVY:
      allow_overlay = true;
      break;
    case GpuVideoAcceleratorFactories::OutputFormat::NV12_DUAL_GMB:
#if defined(OS_WIN)
      allow_overlay = true;
#endif
      break;
    case GpuVideoAcceleratorFactories::OutputFormat::XR30:
      allow_overlay = true;
      break;
    default:
      break;
  }
  frame->metadata()->SetBoolean(VideoFrameMetadata::ALLOW_OVERLAY,
                                allow_overlay);

  base::TimeTicks render_time;
  if (video_frame->metadata()->GetTimeTicks(VideoFrameMetadata::REFERENCE_TIME,
                                            &render_time)) {
    frame->metadata()->SetTimeTicks(VideoFrameMetadata::REFERENCE_TIME,
                                    render_time);
  }

  frame->metadata()->SetBoolean(VideoFrameMetadata::READ_LOCK_FENCES_ENABLED,
                                true);

  DCHECK(!frame_copy_requests_.empty());
  std::move(frame_copy_requests_.front().frame_ready_cb).Run(frame);
  frame_copy_requests_.pop_front();

  if (!frame_copy_requests_.empty()) {
    VideoFrameCopyRequest& copy_request = frame_copy_requests_.front();
    StartCopy(copy_request.video_frame);
  }
}

// Destroy all the resources posting one task per FrameResources
// to the |media_task_runner_|.
GpuMemoryBufferVideoFramePool::PoolImpl::~PoolImpl() {
  DCHECK(in_shutdown_);
}

void GpuMemoryBufferVideoFramePool::PoolImpl::Shutdown() {
  DCHECK(media_task_runner_->BelongsToCurrentThread());
  // Clients don't care about copies once shutdown has started, so abort them.
  Abort();

  // Delete all the resources on the media thread.
  in_shutdown_ = true;
  for (auto* frame_resources : resources_pool_) {
    // Will be deleted later upon return to pool.
    if (frame_resources->is_used())
      continue;

    media_task_runner_->PostTask(
        FROM_HERE, base::Bind(&PoolImpl::DeleteFrameResources, gpu_factories_,
                              base::Owned(frame_resources)));
  }
  resources_pool_.clear();
}

void GpuMemoryBufferVideoFramePool::PoolImpl::SetTickClockForTesting(
    base::TickClock* tick_clock) {
  tick_clock_ = tick_clock;
}

// Tries to find the resources in the pool or create them.
// Incompatible resources will be dropped.
GpuMemoryBufferVideoFramePool::PoolImpl::FrameResources*
GpuMemoryBufferVideoFramePool::PoolImpl::GetOrCreateFrameResources(
    const gfx::Size& size,
    GpuVideoAcceleratorFactories::OutputFormat format) {
  DCHECK(media_task_runner_->BelongsToCurrentThread());

  auto it = resources_pool_.begin();
  while (it != resources_pool_.end()) {
    FrameResources* frame_resources = *it;
    if (!frame_resources->is_used()) {
      if (AreFrameResourcesCompatible(frame_resources, size)) {
        frame_resources->MarkUsed();
        return frame_resources;
      } else {
        resources_pool_.erase(it++);
        DeleteFrameResources(gpu_factories_, frame_resources);
        delete frame_resources;
      }
    } else {
      it++;
    }
  }

  // Create the resources.
  gpu::gles2::GLES2Interface* gles2 = gpu_factories_->ContextGL();
  if (!gles2)
    return nullptr;

  gles2->ActiveTexture(GL_TEXTURE0);
  FrameResources* frame_resources = new FrameResources(size);
  resources_pool_.push_back(frame_resources);
  for (size_t i = 0; i < NumGpuMemoryBuffers(output_format_); i++) {
    PlaneResource& plane_resource = frame_resources->plane_resources[i];
    const size_t width =
        VideoFrame::Columns(i, VideoFormat(format), size.width());
    const size_t height =
        VideoFrame::Rows(i, VideoFormat(format), size.height());
    plane_resource.size = gfx::Size(width, height);

    const gfx::BufferFormat buffer_format = GpuMemoryBufferFormat(format, i);
    plane_resource.gpu_memory_buffer = gpu_factories_->CreateGpuMemoryBuffer(
        plane_resource.size, buffer_format,
        gfx::BufferUsage::SCANOUT_CPU_READ_WRITE);

    unsigned texture_target = gpu_factories_->ImageTextureTarget(buffer_format);
    gles2->GenTextures(1, &plane_resource.texture_id);
    gles2->BindTexture(texture_target, plane_resource.texture_id);
    gles2->TexParameteri(texture_target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gles2->TexParameteri(texture_target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gles2->TexParameteri(texture_target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gles2->TexParameteri(texture_target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gles2->GenMailboxCHROMIUM(plane_resource.mailbox.name);
    gles2->ProduceTextureDirectCHROMIUM(plane_resource.texture_id,
                                        plane_resource.mailbox.name);
  }
  return frame_resources;
}

// static
void GpuMemoryBufferVideoFramePool::PoolImpl::DeleteFrameResources(
    GpuVideoAcceleratorFactories* const gpu_factories,
    FrameResources* frame_resources) {
  // TODO(dcastagna): As soon as the context lost is dealt with in media,
  // make sure that we won't execute this callback (use a weak pointer to
  // the old context).
  gpu::gles2::GLES2Interface* gles2 = gpu_factories->ContextGL();
  if (!gles2)
    return;

  for (PlaneResource& plane_resource : frame_resources->plane_resources) {
    if (plane_resource.image_id)
      gles2->DestroyImageCHROMIUM(plane_resource.image_id);
    if (plane_resource.texture_id)
      gles2->DeleteTextures(1, &plane_resource.texture_id);
  }
}

// Called when a VideoFrame is no longer referenced.
// Put back the resources in the pool.
void GpuMemoryBufferVideoFramePool::PoolImpl::MailboxHoldersReleased(
    FrameResources* frame_resources,
    const gpu::SyncToken& release_sync_token) {
  if (!media_task_runner_->BelongsToCurrentThread()) {
    media_task_runner_->PostTask(
        FROM_HERE, base::BindOnce(&PoolImpl::MailboxHoldersReleased, this,
                                  frame_resources, release_sync_token));
    return;
  }

  if (in_shutdown_) {
    DeleteFrameResources(gpu_factories_, frame_resources);
    delete frame_resources;
    return;
  }

  const base::TimeTicks now = tick_clock_->NowTicks();
  frame_resources->MarkUnused(now);
  auto it = resources_pool_.begin();
  while (it != resources_pool_.end()) {
    FrameResources* frame_resources = *it;

    constexpr base::TimeDelta kStaleFrameLimit =
        base::TimeDelta::FromSeconds(10);
    if (!frame_resources->is_used() &&
        now - frame_resources->last_use_time() > kStaleFrameLimit) {
      resources_pool_.erase(it++);
      DeleteFrameResources(gpu_factories_, frame_resources);
      delete frame_resources;
    } else {
      it++;
    }
  }
}

GpuMemoryBufferVideoFramePool::GpuMemoryBufferVideoFramePool() = default;

GpuMemoryBufferVideoFramePool::GpuMemoryBufferVideoFramePool(
    const scoped_refptr<base::SingleThreadTaskRunner>& media_task_runner,
    const scoped_refptr<base::TaskRunner>& worker_task_runner,
    GpuVideoAcceleratorFactories* gpu_factories)
    : pool_impl_(
          new PoolImpl(media_task_runner, worker_task_runner, gpu_factories)) {
  base::trace_event::MemoryDumpManager::GetInstance()->RegisterDumpProvider(
      pool_impl_.get(), "GpuMemoryBufferVideoFramePool", media_task_runner);
}

GpuMemoryBufferVideoFramePool::~GpuMemoryBufferVideoFramePool() {
  // May be nullptr in tests.
  if (!pool_impl_)
    return;

  pool_impl_->Shutdown();
  base::trace_event::MemoryDumpManager::GetInstance()->UnregisterDumpProvider(
      pool_impl_.get());
}

void GpuMemoryBufferVideoFramePool::MaybeCreateHardwareFrame(
    const scoped_refptr<VideoFrame>& video_frame,
    FrameReadyCB frame_ready_cb) {
  DCHECK(video_frame);
  pool_impl_->CreateHardwareFrame(video_frame, std::move(frame_ready_cb));
}

void GpuMemoryBufferVideoFramePool::Abort() {
  pool_impl_->Abort();
}

void GpuMemoryBufferVideoFramePool::SetTickClockForTesting(
    base::TickClock* tick_clock) {
  pool_impl_->SetTickClockForTesting(tick_clock);
}

}  // namespace media
