// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/android/synchronous_compositor_host.h"

#include <atomic>
#include <utility>

#include "base/command_line.h"
#include "base/containers/hash_tables.h"
#include "base/memory/ptr_util.h"
#include "base/memory/shared_memory.h"
#include "base/threading/thread_restrictions.h"
#include "base/trace_event/trace_event_argument.h"
#include "content/browser/android/synchronous_compositor_browser_filter.h"
#include "content/browser/android/synchronous_compositor_sync_call_bridge.h"
#include "content/browser/bad_message.h"
#include "content/browser/renderer_host/render_widget_host_view_android.h"
#include "content/browser/web_contents/web_contents_impl.h"
#include "content/common/android/sync_compositor_statics.h"
#include "content/common/input/sync_compositor_messages.h"
#include "content/public/browser/android/synchronous_compositor_client.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/common/content_features.h"
#include "content/public/common/content_switches.h"
#include "ipc/ipc_sender.h"
#include "mojo/public/cpp/bindings/strong_binding.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "third_party/skia/include/core/SkRect.h"
#include "ui/events/blink/did_overscroll_params.h"
#include "ui/gfx/skia_util.h"

namespace content {

class SynchronousCompositorLegacyChromeIPC
    : public mojom::SynchronousCompositor {
 public:
  SynchronousCompositorLegacyChromeIPC(IPC::Sender* sender, int routing_id)
      : sender_(sender), routing_id_(routing_id) {}

  void ComputeScroll(base::TimeTicks animation_time) override {
    sender_->Send(
        new SyncCompositorMsg_ComputeScroll(routing_id_, animation_time));
  }

  void DemandDrawHwAsync(
      const SyncCompositorDemandDrawHwParams& draw_params) override {
    sender_->Send(
        new SyncCompositorMsg_DemandDrawHwAsync(routing_id_, draw_params));
  }

  bool DemandDrawHw(
      const content::SyncCompositorDemandDrawHwParams& draw_params,
      content::SyncCompositorCommonRendererParams* out_result,
      uint32_t* out_layer_tree_frame_sink_id,
      base::Optional<viz::CompositorFrame>* out_frame) override {
    return sender_->Send(new SyncCompositorMsg_DemandDrawHw(
        routing_id_, draw_params, out_result, out_layer_tree_frame_sink_id,
        out_frame));
  }

  void DemandDrawHw(const SyncCompositorDemandDrawHwParams& params,
                    DemandDrawHwCallback callback) override {
    NOTREACHED();
  }

  bool SetSharedMemory(
      const content::SyncCompositorSetSharedMemoryParams& params,
      bool* out_success,
      content::SyncCompositorCommonRendererParams* out_result) override {
    return sender_->Send(new SyncCompositorMsg_SetSharedMemory(
        routing_id_, params, out_success, out_result));
  }

  void SetSharedMemory(const SyncCompositorSetSharedMemoryParams& params,
                       SetSharedMemoryCallback callback) override {
    NOTREACHED();
  }

  bool DemandDrawSw(
      const content::SyncCompositorDemandDrawSwParams& draw_params,
      content::SyncCompositorCommonRendererParams* out_result,
      base::Optional<viz::CompositorFrameMetadata>* out_meta_data) override {
    return sender_->Send(new SyncCompositorMsg_DemandDrawSw(
        routing_id_, draw_params, out_result, out_meta_data));
  }

  void DemandDrawSw(const SyncCompositorDemandDrawSwParams& params,
                    DemandDrawSwCallback callback) override {
    NOTREACHED();
  }

  void ZeroSharedMemory() override {
    sender_->Send(new SyncCompositorMsg_ZeroSharedMemory(routing_id_));
  }

  bool ZoomBy(
      float delta,
      const gfx::Point& anchor,
      content::SyncCompositorCommonRendererParams* out_result) override {
    return sender_->Send(
        new SyncCompositorMsg_ZoomBy(routing_id_, delta, anchor, out_result));
  }

  void ZoomBy(float zoom_delta,
              const gfx::Point& anchor,
              ZoomByCallback) override {
    NOTREACHED();
  }

  void SetMemoryPolicy(uint32_t bytes_limit) override {
    sender_->Send(
        new SyncCompositorMsg_SetMemoryPolicy(routing_id_, bytes_limit));
  }

  void ReclaimResources(
      uint32_t layer_tree_frame_sink_id,
      const std::vector<viz::ReturnedResource>& resources) override {
    sender_->Send(new SyncCompositorMsg_ReclaimResources(
        routing_id_, layer_tree_frame_sink_id, resources));
  }

  void SetScroll(const gfx::ScrollOffset& total_scroll_offset) override {
    sender_->Send(
        new SyncCompositorMsg_SetScroll(routing_id_, total_scroll_offset));
  }

  void BeginFrame(const viz::BeginFrameArgs& args) override {
    sender_->Send(new SyncCompositorMsg_BeginFrame(routing_id_, args));
  }

  void SetBeginFrameSourcePaused(bool paused) override {
    sender_->Send(
        new SyncCompositorMsg_SetBeginFramePaused(routing_id_, paused));
  }

 private:
  IPC::Sender* const sender_;
  int routing_id_;
};

// This class runs on the IO thread and is destroyed when the renderer
// side closes the mojo channel.
class SynchronousCompositorControlHost
    : public mojom::SynchronousCompositorControlHost {
 public:
  SynchronousCompositorControlHost(
      scoped_refptr<SynchronousCompositorSyncCallBridge> bridge,
      int process_id)
      : bridge_(std::move(bridge)), process_id_(process_id) {}

  ~SynchronousCompositorControlHost() override {
    bridge_->RemoteClosedOnIOThread();
  }

  static void Create(mojom::SynchronousCompositorControlHostRequest request,
                     scoped_refptr<SynchronousCompositorSyncCallBridge> bridge,
                     int process_id) {
    DCHECK_CURRENTLY_ON(BrowserThread::UI);
    BrowserThread::PostTask(
        BrowserThread::IO, FROM_HERE,
        base::BindOnce(&CreateOnIOThread, std::move(request), std::move(bridge),
                       process_id));
  }

  static void CreateOnIOThread(
      mojom::SynchronousCompositorControlHostRequest request,
      scoped_refptr<SynchronousCompositorSyncCallBridge> bridge,
      int process_id) {
    DCHECK_CURRENTLY_ON(BrowserThread::IO);
    mojo::MakeStrongBinding(std::make_unique<SynchronousCompositorControlHost>(
                                std::move(bridge), process_id),
                            std::move(request));
  }

  // SynchronousCompositorControlHost overrides.
  void ReturnFrame(uint32_t layer_tree_frame_sink_id,
                   base::Optional<viz::CompositorFrame> frame) override {
    if (!bridge_->ReceiveFrameOnIOThread(layer_tree_frame_sink_id,
                                         std::move(frame))) {
      bad_message::ReceivedBadMessage(
          process_id_, bad_message::SYNC_COMPOSITOR_NO_FUTURE_FRAME);
    }
  }

  void BeginFrameResponse(
      const content::SyncCompositorCommonRendererParams& params) override {
    if (!bridge_->BeginFrameResponseOnIOThread(params)) {
      bad_message::ReceivedBadMessage(
          process_id_, bad_message::SYNC_COMPOSITOR_NO_BEGIN_FRAME);
    }
  }

 private:
  scoped_refptr<SynchronousCompositorSyncCallBridge> bridge_;
  const int process_id_;
};

// static
std::unique_ptr<SynchronousCompositorHost> SynchronousCompositorHost::Create(
    RenderWidgetHostViewAndroid* rwhva) {
  if (!rwhva->synchronous_compositor_client())
    return nullptr;  // Not using sync compositing.

  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  bool use_in_proc_software_draw =
      command_line->HasSwitch(switches::kSingleProcess);
  return base::WrapUnique(new SynchronousCompositorHost(
      rwhva, use_in_proc_software_draw));
}

SynchronousCompositorHost::SynchronousCompositorHost(
    RenderWidgetHostViewAndroid* rwhva,
    bool use_in_proc_software_draw)
    : rwhva_(rwhva),
      client_(rwhva->synchronous_compositor_client()),
      process_id_(rwhva_->GetRenderWidgetHost()->GetProcess()->GetID()),
      routing_id_(rwhva_->GetRenderWidgetHost()->GetRoutingID()),
      use_mojo_(base::FeatureList::IsEnabled(features::kMojoInputMessages)),
      use_in_process_zero_copy_software_draw_(use_in_proc_software_draw),
      host_binding_(this),
      bytes_limit_(0u),
      renderer_param_version_(0u),
      need_animate_scroll_(false),
      need_invalidate_count_(0u),
      did_activate_pending_tree_count_(0u) {
  client_->DidInitializeCompositor(this, process_id_, routing_id_);
  bridge_ = new SynchronousCompositorSyncCallBridge(this);

  if (!use_mojo_) {
    bridge_->BindFilterOnUIThread();
    legacy_compositor_ = std::make_unique<SynchronousCompositorLegacyChromeIPC>(
        rwhva_->GetRenderWidgetHost(), routing_id_);
  }
}

SynchronousCompositorHost::~SynchronousCompositorHost() {
  client_->DidDestroyCompositor(this, process_id_, routing_id_);
  bridge_->HostDestroyedOnUIThread();
}

void SynchronousCompositorHost::InitMojo() {
  if (!use_mojo_)
    return;

  mojom::SynchronousCompositorControlHostPtr host_control;
  mojom::SynchronousCompositorControlHostRequest host_request =
      mojo::MakeRequest(&host_control);

  SynchronousCompositorControlHost::Create(std::move(host_request), bridge_,
                                           process_id_);
  mojom::SynchronousCompositorHostAssociatedPtr host;
  host_binding_.Bind(mojo::MakeRequest(&host));

  mojom::SynchronousCompositorAssociatedRequest compositor_request =
      mojo::MakeRequest(&sync_compositor_);

  rwhva_->host()->GetWidgetInputHandler()->AttachSynchronousCompositor(
      std::move(host_control), host.PassInterface(),
      std::move(compositor_request));
}

bool SynchronousCompositorHost::IsReadyForSynchronousCall() {
  bool res = bridge_->IsRemoteReadyOnUIThread();
  DCHECK(!res || GetSynchronousCompositor());
  return res;
}

bool SynchronousCompositorHost::OnMessageReceived(const IPC::Message& message) {
  bool handled = true;
  IPC_BEGIN_MESSAGE_MAP(SynchronousCompositorHost, message)
    IPC_MESSAGE_HANDLER(SyncCompositorHostMsg_LayerTreeFrameSinkCreated,
                        LayerTreeFrameSinkCreated)
    IPC_MESSAGE_HANDLER(SyncCompositorHostMsg_SetNeedsBeginFrames,
                        SetNeedsBeginFrames)
    IPC_MESSAGE_HANDLER(SyncCompositorHostMsg_UpdateState, UpdateState)
    IPC_MESSAGE_UNHANDLED(handled = false)
  IPC_END_MESSAGE_MAP()
  return handled;
}

scoped_refptr<SynchronousCompositor::FrameFuture>
SynchronousCompositorHost::DemandDrawHwAsync(
    const gfx::Size& viewport_size,
    const gfx::Rect& viewport_rect_for_tile_priority,
    const gfx::Transform& transform_for_tile_priority) {
  scoped_refptr<FrameFuture> frame_future = new FrameFuture();
  if (compute_scroll_needs_synchronous_draw_ || !allow_async_draw_) {
    allow_async_draw_ = allow_async_draw_ || IsReadyForSynchronousCall();
    compute_scroll_needs_synchronous_draw_ = false;
    auto frame_ptr = std::make_unique<Frame>();
    *frame_ptr = DemandDrawHw(viewport_size, viewport_rect_for_tile_priority,
                              transform_for_tile_priority);
    frame_future->SetFrame(std::move(frame_ptr));
    return frame_future;
  }

  SyncCompositorDemandDrawHwParams params(viewport_size,
                                          viewport_rect_for_tile_priority,
                                          transform_for_tile_priority);
  mojom::SynchronousCompositor* compositor = GetSynchronousCompositor();
  if (!bridge_->SetFrameFutureOnUIThread(frame_future)) {
    frame_future->SetFrame(nullptr);
  } else {
    DCHECK(compositor);
    compositor->DemandDrawHwAsync(params);
  }
  return frame_future;
}

SynchronousCompositor::Frame SynchronousCompositorHost::DemandDrawHw(
    const gfx::Size& viewport_size,
    const gfx::Rect& viewport_rect_for_tile_priority,
    const gfx::Transform& transform_for_tile_priority) {
  SyncCompositorDemandDrawHwParams params(viewport_size,
                                          viewport_rect_for_tile_priority,
                                          transform_for_tile_priority);
  uint32_t layer_tree_frame_sink_id;
  base::Optional<viz::CompositorFrame> compositor_frame;
  SyncCompositorCommonRendererParams common_renderer_params;

  {
    mojo::SyncCallRestrictions::ScopedAllowSyncCall allow_sync_call;
    base::ScopedAllowBaseSyncPrimitivesOutsideBlockingScope
        allow_base_sync_primitives;
    if (!IsReadyForSynchronousCall() ||
        !GetSynchronousCompositor()->DemandDrawHw(
            params, &common_renderer_params, &layer_tree_frame_sink_id,
            &compositor_frame)) {
      return SynchronousCompositor::Frame();
    }
  }

  UpdateState(common_renderer_params);

  if (!compositor_frame)
    return SynchronousCompositor::Frame();

  SynchronousCompositor::Frame frame;
  frame.frame.reset(new viz::CompositorFrame);
  frame.layer_tree_frame_sink_id = layer_tree_frame_sink_id;
  *frame.frame = std::move(*compositor_frame);
  UpdateFrameMetaData(frame.frame->metadata.Clone());
  return frame;
}

void SynchronousCompositorHost::UpdateFrameMetaData(
    viz::CompositorFrameMetadata frame_metadata) {
  rwhva_->SynchronousFrameMetadata(std::move(frame_metadata));
}

namespace {

class ScopedSetSkCanvas {
 public:
  explicit ScopedSetSkCanvas(SkCanvas* canvas) {
    SynchronousCompositorSetSkCanvas(canvas);
  }

  ~ScopedSetSkCanvas() {
    SynchronousCompositorSetSkCanvas(nullptr);
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(ScopedSetSkCanvas);
};

}

bool SynchronousCompositorHost::DemandDrawSwInProc(SkCanvas* canvas) {
  mojo::SyncCallRestrictions::ScopedAllowSyncCall allow_sync_call;
  base::ScopedAllowBaseSyncPrimitivesOutsideBlockingScope
      allow_base_sync_primitives;
  SyncCompositorCommonRendererParams common_renderer_params;
  base::Optional<viz::CompositorFrameMetadata> metadata;
  ScopedSetSkCanvas set_sk_canvas(canvas);
  SyncCompositorDemandDrawSwParams params;  // Unused.
  if (!IsReadyForSynchronousCall() ||
      !GetSynchronousCompositor()->DemandDrawSw(params, &common_renderer_params,
                                                &metadata))
    return false;
  if (!metadata)
    return false;
  UpdateState(common_renderer_params);
  UpdateFrameMetaData(std::move(*metadata));
  return true;
}

class SynchronousCompositorHost::ScopedSendZeroMemory {
 public:
  ScopedSendZeroMemory(SynchronousCompositorHost* host) : host_(host) {}
  ~ScopedSendZeroMemory() { host_->SendZeroMemory(); }

 private:
  SynchronousCompositorHost* const host_;

  DISALLOW_COPY_AND_ASSIGN(ScopedSendZeroMemory);
};

struct SynchronousCompositorHost::SharedMemoryWithSize {
  base::SharedMemory shm;
  const size_t stride;
  const size_t buffer_size;

  SharedMemoryWithSize(size_t stride, size_t buffer_size)
      : stride(stride), buffer_size(buffer_size) {}

 private:
  DISALLOW_COPY_AND_ASSIGN(SharedMemoryWithSize);
};

bool SynchronousCompositorHost::DemandDrawSw(SkCanvas* canvas) {
  if (use_in_process_zero_copy_software_draw_)
    return DemandDrawSwInProc(canvas);

  SyncCompositorDemandDrawSwParams params;
  params.size = gfx::Size(canvas->getBaseLayerSize().width(),
                          canvas->getBaseLayerSize().height());
  SkIRect canvas_clip = canvas->getDeviceClipBounds();
  params.clip = gfx::SkIRectToRect(canvas_clip);
  params.transform.matrix() = canvas->getTotalMatrix();
  if (params.size.IsEmpty())
    return true;

  SkImageInfo info =
      SkImageInfo::MakeN32Premul(params.size.width(), params.size.height());
  DCHECK_EQ(kRGBA_8888_SkColorType, info.colorType());
  size_t stride = info.minRowBytes();
  size_t buffer_size = info.computeByteSize(stride);
  if (SkImageInfo::ByteSizeOverflowed(buffer_size))
    return false;

  SetSoftwareDrawSharedMemoryIfNeeded(stride, buffer_size);
  if (!software_draw_shm_)
    return false;

  base::Optional<viz::CompositorFrameMetadata> metadata;
  SyncCompositorCommonRendererParams common_renderer_params;
  {
    mojo::SyncCallRestrictions::ScopedAllowSyncCall allow_sync_call;
    base::ScopedAllowBaseSyncPrimitivesOutsideBlockingScope
        allow_base_sync_primitives;
    if (!IsReadyForSynchronousCall() ||
        !GetSynchronousCompositor()->DemandDrawSw(
            params, &common_renderer_params, &metadata)) {
      return false;
    }
  }
  ScopedSendZeroMemory send_zero_memory(this);
  if (!metadata)
    return false;

  UpdateState(common_renderer_params);
  UpdateFrameMetaData(std::move(*metadata));

  SkBitmap bitmap;
  if (!bitmap.installPixels(info, software_draw_shm_->shm.memory(), stride))
    return false;

  {
    TRACE_EVENT0("browser", "DrawBitmap");
    canvas->save();
    canvas->resetMatrix();
    canvas->drawBitmap(bitmap, 0, 0);
    canvas->restore();
  }

  return true;
}

void SynchronousCompositorHost::SetSoftwareDrawSharedMemoryIfNeeded(
    size_t stride,
    size_t buffer_size) {
  if (software_draw_shm_ && software_draw_shm_->stride == stride &&
      software_draw_shm_->buffer_size == buffer_size)
    return;
  software_draw_shm_.reset();
  std::unique_ptr<SharedMemoryWithSize> software_draw_shm(
      new SharedMemoryWithSize(stride, buffer_size));
  {
    TRACE_EVENT1("browser", "AllocateSharedMemory", "buffer_size", buffer_size);
    if (!software_draw_shm->shm.CreateAndMapAnonymous(buffer_size))
      return;
  }

  SyncCompositorSetSharedMemoryParams set_shm_params;
  set_shm_params.buffer_size = buffer_size;
  set_shm_params.shm_handle = software_draw_shm->shm.handle().Duplicate();
  if (!set_shm_params.shm_handle.IsValid())
    return;

  bool success = false;
  SyncCompositorCommonRendererParams common_renderer_params;
  {
    mojo::SyncCallRestrictions::ScopedAllowSyncCall allow_sync_call;
    base::ScopedAllowBaseSyncPrimitivesOutsideBlockingScope
        allow_base_sync_primitives;
    if (!IsReadyForSynchronousCall() ||
        !GetSynchronousCompositor()->SetSharedMemory(set_shm_params, &success,
                                                     &common_renderer_params) ||
        !success) {
      return;
    }
  }
  software_draw_shm_ = std::move(software_draw_shm);
  UpdateState(common_renderer_params);
}

void SynchronousCompositorHost::SendZeroMemory() {
  // No need to check return value.
  if (mojom::SynchronousCompositor* compositor = GetSynchronousCompositor())
    compositor->ZeroSharedMemory();
}

void SynchronousCompositorHost::ReturnResources(
    uint32_t layer_tree_frame_sink_id,
    const std::vector<viz::ReturnedResource>& resources) {
  DCHECK(!resources.empty());
  if (mojom::SynchronousCompositor* compositor = GetSynchronousCompositor())
    compositor->ReclaimResources(layer_tree_frame_sink_id, resources);
}

void SynchronousCompositorHost::SetMemoryPolicy(size_t bytes_limit) {
  if (bytes_limit_ == bytes_limit)
    return;

  bytes_limit_ = bytes_limit;
  if (mojom::SynchronousCompositor* compositor = GetSynchronousCompositor())
    compositor->SetMemoryPolicy(bytes_limit_);
}

void SynchronousCompositorHost::DidChangeRootLayerScrollOffset(
    const gfx::ScrollOffset& root_offset) {
  if (root_scroll_offset_ == root_offset)
    return;
  root_scroll_offset_ = root_offset;
  if (mojom::SynchronousCompositor* compositor = GetSynchronousCompositor())
    compositor->SetScroll(root_scroll_offset_);
}

void SynchronousCompositorHost::SynchronouslyZoomBy(float zoom_delta,
                                                    const gfx::Point& anchor) {
  SyncCompositorCommonRendererParams common_renderer_params;
  {
    mojo::SyncCallRestrictions::ScopedAllowSyncCall allow_sync_call;
    base::ScopedAllowBaseSyncPrimitivesOutsideBlockingScope
        allow_base_sync_primitives;
    if (!IsReadyForSynchronousCall() ||
        !GetSynchronousCompositor()->ZoomBy(zoom_delta, anchor,
                                            &common_renderer_params)) {
      return;
    }
  }
  UpdateState(common_renderer_params);
}

void SynchronousCompositorHost::OnComputeScroll(
    base::TimeTicks animation_time) {
  if (!need_animate_scroll_)
    return;
  need_animate_scroll_ = false;

  if (mojom::SynchronousCompositor* compositor = GetSynchronousCompositor())
    compositor->ComputeScroll(animation_time);
  compute_scroll_needs_synchronous_draw_ = true;
}

void SynchronousCompositorHost::DidOverscroll(
    const ui::DidOverscrollParams& over_scroll_params) {
  client_->DidOverscroll(this, over_scroll_params.accumulated_overscroll,
                         over_scroll_params.latest_overscroll_delta,
                         over_scroll_params.current_fling_velocity);
}

void SynchronousCompositorHost::BeginFrame(ui::WindowAndroid* window_android,
                                           const viz::BeginFrameArgs& args) {
  compute_scroll_needs_synchronous_draw_ = false;
  if (!bridge_->WaitAfterVSyncOnUIThread(window_android))
    return;
  mojom::SynchronousCompositor* compositor = GetSynchronousCompositor();
  DCHECK(compositor);
  compositor->BeginFrame(args);
}

void SynchronousCompositorHost::SetBeginFramePaused(bool paused) {
  begin_frame_paused_ = paused;
  if (mojom::SynchronousCompositor* compositor = GetSynchronousCompositor())
    compositor->SetBeginFrameSourcePaused(paused);
}

void SynchronousCompositorHost::SetNeedsBeginFrames(bool needs_begin_frames) {
  rwhva_->host()->SetNeedsBeginFrame(needs_begin_frames);
}

void SynchronousCompositorHost::LayerTreeFrameSinkCreated() {
  if (use_mojo_)
    bridge_->RemoteReady();

  // New LayerTreeFrameSink is not aware of state from Browser side. So need to
  // re-send all browser side state here.
  mojom::SynchronousCompositor* compositor = GetSynchronousCompositor();
  DCHECK(compositor);
  compositor->SetMemoryPolicy(bytes_limit_);

  if (begin_frame_paused_)
    compositor->SetBeginFrameSourcePaused(begin_frame_paused_);
}

void SynchronousCompositorHost::UpdateState(
    const SyncCompositorCommonRendererParams& params) {
  // Ignore if |renderer_param_version_| is newer than |params.version|. This
  // comparison takes into account when the unsigned int wraps.
  if ((renderer_param_version_ - params.version) < 0x80000000) {
    return;
  }
  renderer_param_version_ = params.version;
  need_animate_scroll_ = params.need_animate_scroll;
  root_scroll_offset_ = params.total_scroll_offset;

  if (need_invalidate_count_ != params.need_invalidate_count) {
    need_invalidate_count_ = params.need_invalidate_count;
    client_->PostInvalidate(this);
  }

  if (did_activate_pending_tree_count_ !=
      params.did_activate_pending_tree_count) {
    did_activate_pending_tree_count_ = params.did_activate_pending_tree_count;
    client_->DidUpdateContent(this);
  }

  // Ensure only valid values from compositor are sent to client.
  // Compositor has page_scale_factor set to 0 before initialization, so check
  // for that case here.
  if (params.page_scale_factor) {
    client_->UpdateRootLayerState(
        this, gfx::ScrollOffsetToVector2dF(params.total_scroll_offset),
        gfx::ScrollOffsetToVector2dF(params.max_scroll_offset),
        params.scrollable_size, params.page_scale_factor,
        params.min_page_scale_factor, params.max_page_scale_factor);
  }
}

SynchronousCompositorBrowserFilter* SynchronousCompositorHost::GetFilter() {
  return static_cast<RenderProcessHostImpl*>(
             rwhva_->GetRenderWidgetHost()->GetProcess())
      ->synchronous_compositor_filter();
}

RenderProcessHost* SynchronousCompositorHost::GetRenderProcessHost() {
  return rwhva_->GetRenderWidgetHost()->GetProcess();
}

mojom::SynchronousCompositor*
SynchronousCompositorHost::GetSynchronousCompositor() {
  if (legacy_compositor_)
    return legacy_compositor_.get();
  return sync_compositor_.get();
}

}  // namespace content
