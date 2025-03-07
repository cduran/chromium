// Copyright 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CC_TREES_LAYER_TREE_HOST_IMPL_H_
#define CC_TREES_LAYER_TREE_HOST_IMPL_H_

#include <stddef.h>

#include <bitset>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/callback.h"
#include "base/containers/flat_map.h"
#include "base/macros.h"
#include "base/sequenced_task_runner.h"
#include "base/time/time.h"
#include "cc/base/synced_property.h"
#include "cc/benchmarks/micro_benchmark_controller_impl.h"
#include "cc/cc_export.h"
#include "cc/input/browser_controls_offset_manager_client.h"
#include "cc/input/input_handler.h"
#include "cc/input/scrollbar_animation_controller.h"
#include "cc/layers/layer_collections.h"
#include "cc/resources/layer_tree_resource_provider.h"
#include "cc/resources/ui_resource_client.h"
#include "cc/scheduler/begin_frame_tracker.h"
#include "cc/scheduler/commit_earlyout_reason.h"
#include "cc/scheduler/draw_result.h"
#include "cc/scheduler/video_frame_controller.h"
#include "cc/tiles/decoded_image_tracker.h"
#include "cc/tiles/image_decode_cache.h"
#include "cc/tiles/tile_manager.h"
#include "cc/trees/frame_token_allocator.h"
#include "cc/trees/layer_tree_frame_sink_client.h"
#include "cc/trees/layer_tree_mutator.h"
#include "cc/trees/layer_tree_settings.h"
#include "cc/trees/managed_memory_policy.h"
#include "cc/trees/mutator_host_client.h"
#include "cc/trees/task_runner_provider.h"
#include "cc/trees/ukm_manager.h"
#include "components/viz/common/frame_sinks/begin_frame_args.h"
#include "components/viz/common/gpu/context_cache_controller.h"
#include "components/viz/common/quads/render_pass.h"
#include "components/viz/common/surfaces/local_surface_id.h"
#include "components/viz/common/surfaces/surface_id.h"
#include "ui/gfx/geometry/rect.h"

namespace gfx {
class ScrollOffset;
}

namespace viz {
class CompositorFrameMetadata;
}

namespace cc {
class BrowserControlsOffsetManager;
class LayerTreeFrameSink;
class DebugRectHistory;
class EvictionTilePriorityQueue;
class FrameRateCounter;
class ImageAnimationController;
class LayerImpl;
class LayerTreeImpl;
class MemoryHistory;
class MutatorEvents;
class MutatorHost;
class PageScaleAnimation;
class PendingTreeDurationHistogramTimer;
class PendingTreeRasterDurationHistogramTimer;
class RasterTilePriorityQueue;
class RasterBufferProvider;
class RenderFrameMetadata;
class RenderFrameMetadataObserver;
class RenderingStatsInstrumentation;
class ResourcePool;
class ScrollElasticityHelper;
class SwapPromise;
class SwapPromiseMonitor;
class SynchronousTaskGraphRunner;
class TaskGraphRunner;
class UIResourceBitmap;
struct ScrollAndScaleSet;
class Viewport;

using BeginFrameCallbackList = std::vector<base::Closure>;

enum class GpuRasterizationStatus {
  ON,
  ON_FORCED,
  OFF_DEVICE,
  OFF_VIEWPORT,
  MSAA_CONTENT,
};

enum class ImplThreadPhase {
  IDLE,
  INSIDE_IMPL_FRAME,
};

// LayerTreeHost->Proxy callback interface.
class LayerTreeHostImplClient {
 public:
  virtual void DidLoseLayerTreeFrameSinkOnImplThread() = 0;
  virtual void SetBeginFrameSource(viz::BeginFrameSource* source) = 0;
  virtual void DidReceiveCompositorFrameAckOnImplThread() = 0;
  virtual void OnCanDrawStateChanged(bool can_draw) = 0;
  virtual void NotifyReadyToActivate() = 0;
  virtual void NotifyReadyToDraw() = 0;
  // Please call these 2 functions through
  // LayerTreeHostImpl's SetNeedsRedraw() and SetNeedsOneBeginImplFrame().
  virtual void SetNeedsRedrawOnImplThread() = 0;
  virtual void SetNeedsOneBeginImplFrameOnImplThread() = 0;
  virtual void SetNeedsCommitOnImplThread() = 0;
  virtual void SetNeedsPrepareTilesOnImplThread() = 0;
  virtual void SetVideoNeedsBeginFrames(bool needs_begin_frames) = 0;
  virtual void PostAnimationEventsToMainThreadOnImplThread(
      std::unique_ptr<MutatorEvents> events) = 0;
  virtual bool IsInsideDraw() = 0;
  virtual void RenewTreePriority() = 0;
  virtual void PostDelayedAnimationTaskOnImplThread(const base::Closure& task,
                                                    base::TimeDelta delay) = 0;
  virtual void DidActivateSyncTree() = 0;
  virtual void WillPrepareTiles() = 0;
  virtual void DidPrepareTiles() = 0;

  // Called when page scale animation has completed on the impl thread.
  virtual void DidCompletePageScaleAnimationOnImplThread() = 0;

  // Called when output surface asks for a draw.
  virtual void OnDrawForLayerTreeFrameSink(bool resourceless_software_draw) = 0;

  virtual void NeedsImplSideInvalidation(
      bool needs_first_draw_on_activation) = 0;
  // Called when a requested image decode completes.
  virtual void NotifyImageDecodeRequestFinished() = 0;

  virtual void RequestBeginMainFrameNotExpected(bool new_state) = 0;

  // Called when a presentation time is requested. |source_frames| identifies
  // the frames that correspond to the request.
  virtual void DidPresentCompositorFrameOnImplThread(
      const std::vector<int>& source_frames,
      base::TimeTicks time,
      base::TimeDelta refresh,
      uint32_t flags) = 0;

 protected:
  virtual ~LayerTreeHostImplClient() {}
};

// LayerTreeHostImpl owns the LayerImpl trees as well as associated rendering
// state.
class CC_EXPORT LayerTreeHostImpl
    : public InputHandler,
      public TileManagerClient,
      public LayerTreeFrameSinkClient,
      public BrowserControlsOffsetManagerClient,
      public ScrollbarAnimationControllerClient,
      public VideoFrameControllerClient,
      public MutatorHostClient,
      public base::SupportsWeakPtr<LayerTreeHostImpl> {
 public:
  static std::unique_ptr<LayerTreeHostImpl> Create(
      const LayerTreeSettings& settings,
      LayerTreeHostImplClient* client,
      TaskRunnerProvider* task_runner_provider,
      RenderingStatsInstrumentation* rendering_stats_instrumentation,
      TaskGraphRunner* task_graph_runner,
      std::unique_ptr<MutatorHost> mutator_host,
      int id,
      scoped_refptr<base::SequencedTaskRunner> image_worker_task_runner);
  ~LayerTreeHostImpl() override;

  // InputHandler implementation
  void BindToClient(InputHandlerClient* client,
                    bool wheel_scroll_latching_enabled) override;
  InputHandler::ScrollStatus ScrollBegin(
      ScrollState* scroll_state,
      InputHandler::ScrollInputType type) override;
  InputHandler::ScrollStatus RootScrollBegin(
      ScrollState* scroll_state,
      InputHandler::ScrollInputType type) override;
  ScrollStatus ScrollAnimatedBegin(ScrollState* scroll_state) override;
  InputHandler::ScrollStatus ScrollAnimated(
      const gfx::Point& viewport_point,
      const gfx::Vector2dF& scroll_delta,
      base::TimeDelta delayed_by = base::TimeDelta()) override;
  void ApplyScroll(ScrollNode* scroll_node, ScrollState* scroll_state);
  InputHandlerScrollResult ScrollBy(ScrollState* scroll_state) override;
  void RequestUpdateForSynchronousInputHandler() override;
  void SetSynchronousInputHandlerRootScrollOffset(
      const gfx::ScrollOffset& root_offset) override;
  void ScrollEnd(ScrollState* scroll_state, bool should_snap = false) override;
  InputHandler::ScrollStatus FlingScrollBegin() override;

  void MouseDown() override;
  void MouseUp() override;
  void MouseMoveAt(const gfx::Point& viewport_point) override;
  void MouseLeave() override;

  void PinchGestureBegin() override;
  void PinchGestureUpdate(float magnify_delta,
                          const gfx::Point& anchor) override;
  void PinchGestureEnd(const gfx::Point& anchor, bool snap_to_min) override;
  void StartPageScaleAnimation(const gfx::Vector2d& target_offset,
                               bool anchor_point,
                               float page_scale,
                               base::TimeDelta duration);
  void SetNeedsAnimateInput() override;
  bool IsCurrentlyScrollingViewport() const override;
  bool IsCurrentlyScrollingLayerAt(
      const gfx::Point& viewport_point,
      InputHandler::ScrollInputType type) const override;
  EventListenerProperties GetEventListenerProperties(
      EventListenerClass event_class) const override;
  InputHandler::TouchStartOrMoveEventListenerType
  EventListenerTypeForTouchStartOrMoveAt(
      const gfx::Point& viewport_port,
      TouchAction* out_touch_action) override;
  std::unique_ptr<SwapPromiseMonitor> CreateLatencyInfoSwapPromiseMonitor(
      ui::LatencyInfo* latency) override;
  ScrollElasticityHelper* CreateScrollElasticityHelper() override;
  bool GetScrollOffsetForLayer(int layer_id,
                               gfx::ScrollOffset* offset) override;
  bool ScrollLayerTo(int layer_id, const gfx::ScrollOffset& offset) override;
  bool ScrollingShouldSwitchtoMainThread() override;

  // BrowserControlsOffsetManagerClient implementation.
  float TopControlsHeight() const override;
  float BottomControlsHeight() const override;
  void SetCurrentBrowserControlsShownRatio(float offset) override;
  float CurrentBrowserControlsShownRatio() const override;
  void DidChangeBrowserControlsPosition() override;
  bool HaveRootScrollLayer() const override;

  void UpdateViewportContainerSizes();

  void set_resourceless_software_draw_for_testing() {
    resourceless_software_draw_ = true;
  }

  struct CC_EXPORT FrameData {
    FrameData();
    ~FrameData();
    void AsValueInto(base::trace_event::TracedValue* value) const;

    std::vector<viz::SurfaceId> activation_dependencies;
    base::Optional<uint32_t> deadline_in_frames;
    bool use_default_lower_bound_deadline = false;
    std::vector<gfx::Rect> occluding_screen_space_rects;
    std::vector<gfx::Rect> non_occluding_screen_space_rects;
    viz::RenderPassList render_passes;
    const RenderSurfaceList* render_surface_list;
    LayerImplList will_draw_layers;
    bool has_no_damage;
    bool may_contain_video;
    viz::BeginFrameAck begin_frame_ack;

   private:
    DISALLOW_COPY_AND_ASSIGN(FrameData);
  };

  virtual void DidSendBeginMainFrame() {}
  virtual void BeginMainFrameAborted(
      CommitEarlyOutReason reason,
      std::vector<std::unique_ptr<SwapPromise>> swap_promises);
  virtual void ReadyToCommit() {}  // For tests.
  virtual void BeginCommit();
  virtual void CommitComplete();
  virtual void UpdateAnimationState(bool start_ready_animations);
  bool Mutate(base::TimeTicks monotonic_time);
  void ActivateAnimations();
  void Animate();
  void AnimatePendingTreeAfterCommit();
  void MainThreadHasStoppedFlinging();
  void DidAnimateScrollOffset();
  void SetFullViewportDamage();
  void SetViewportDamage(const gfx::Rect& damage_rect);

  // Analogous to a commit, this function is used to create a sync tree and
  // add impl-side invalidations to it.
  // virtual for testing.
  virtual void InvalidateContentOnImplSide();
  virtual void InvalidateLayerTreeFrameSink();

  void SetTreeLayerScrollOffsetMutated(ElementId element_id,
                                       LayerTreeImpl* tree,
                                       const gfx::ScrollOffset& scroll_offset);
  void SetNeedUpdateGpuRasterizationStatus();
  bool NeedUpdateGpuRasterizationStatusForTesting() const {
    return need_update_gpu_rasterization_status_;
  }

  // MutatorHostClient implementation.
  bool IsElementInList(ElementId element_id,
                       ElementListType list_type) const override;
  void SetMutatorsNeedCommit() override;
  void SetMutatorsNeedRebuildPropertyTrees() override;
  void SetElementFilterMutated(ElementId element_id,
                               ElementListType list_type,
                               const FilterOperations& filters) override;
  void SetElementOpacityMutated(ElementId element_id,
                                ElementListType list_type,
                                float opacity) override;
  void SetElementTransformMutated(ElementId element_id,
                                  ElementListType list_type,
                                  const gfx::Transform& transform) override;
  void SetElementScrollOffsetMutated(
      ElementId element_id,
      ElementListType list_type,
      const gfx::ScrollOffset& scroll_offset) override;
  void ElementIsAnimatingChanged(ElementId element_id,
                                 ElementListType list_type,
                                 const PropertyAnimationState& mask,
                                 const PropertyAnimationState& state) override;

  void ScrollOffsetAnimationFinished() override;
  gfx::ScrollOffset GetScrollOffsetForAnimation(
      ElementId element_id) const override;

  virtual bool PrepareTiles();

  // Returns DRAW_SUCCESS unless problems occured preparing the frame, and we
  // should try to avoid displaying the frame. If PrepareToDraw is called,
  // DidDrawAllLayers must also be called, regardless of whether DrawLayers is
  // called between the two.
  virtual DrawResult PrepareToDraw(FrameData* frame);
  virtual bool DrawLayers(FrameData* frame);
  // Must be called if and only if PrepareToDraw was called.
  void DidDrawAllLayers(const FrameData& frame);

  const LayerTreeSettings& settings() const { return settings_; }

  // Evict all textures by enforcing a memory policy with an allocation of 0.
  void EvictTexturesForTesting();

  // When blocking, this prevents client_->NotifyReadyToActivate() from being
  // called. When disabled, it calls client_->NotifyReadyToActivate()
  // immediately if any notifications had been blocked while blocking.
  virtual void BlockNotifyReadyToActivateForTesting(bool block);

  // Prevents notifying the |client_| when an impl side invalidation request is
  // made. When unblocked, the disabled request will immediately be called.
  virtual void BlockImplSideInvalidationRequestsForTesting(bool block);

  // Resets all of the trees to an empty state.
  void ResetTreesForTesting();

  size_t SourceAnimationFrameNumberForTesting() const;

  void RegisterScrollbarAnimationController(ElementId scroll_element_id,
                                            float initial_opacity);
  void UnregisterScrollbarAnimationController(ElementId scroll_element_id);
  ScrollbarAnimationController* ScrollbarAnimationControllerForElementId(
      ElementId scroll_element_id) const;
  void FlashAllScrollbars(bool did_scroll);

  DrawMode GetDrawMode() const;

  // TileManagerClient implementation.
  void NotifyReadyToActivate() override;
  void NotifyReadyToDraw() override;
  void NotifyAllTileTasksCompleted() override;
  void NotifyTileStateChanged(const Tile* tile) override;
  std::unique_ptr<RasterTilePriorityQueue> BuildRasterQueue(
      TreePriority tree_priority,
      RasterTilePriorityQueue::Type type) override;
  std::unique_ptr<EvictionTilePriorityQueue> BuildEvictionQueue(
      TreePriority tree_priority) override;
  void SetIsLikelyToRequireADraw(bool is_likely_to_require_a_draw) override;
  RasterColorSpace GetRasterColorSpace() const override;
  void RequestImplSideInvalidationForCheckerImagedTiles() override;
  size_t GetFrameIndexForImage(const PaintImage& paint_image,
                               WhichTree tree) const override;

  // ScrollbarAnimationControllerClient implementation.
  void PostDelayedScrollbarAnimationTask(const base::Closure& task,
                                         base::TimeDelta delay) override;
  void SetNeedsAnimateForScrollbarAnimation() override;
  void SetNeedsRedrawForScrollbarAnimation() override;
  ScrollbarSet ScrollbarsFor(ElementId scroll_element_id) const override;
  void DidChangeScrollbarVisibility() override;

  // VideoBeginFrameSource implementation.
  void AddVideoFrameController(VideoFrameController* controller) override;
  void RemoveVideoFrameController(VideoFrameController* controller) override;

  // LayerTreeFrameSinkClient implementation.
  void SetBeginFrameSource(viz::BeginFrameSource* source) override;
  void SetExternalTilePriorityConstraints(
      const gfx::Rect& viewport_rect,
      const gfx::Transform& transform) override;
  void DidLoseLayerTreeFrameSink() override;
  void DidReceiveCompositorFrameAck() override;
  void DidPresentCompositorFrame(uint32_t presentation_token,
                                 base::TimeTicks time,
                                 base::TimeDelta refresh,
                                 uint32_t flags) override;
  void DidDiscardCompositorFrame(uint32_t presentation_token) override;
  void ReclaimResources(
      const std::vector<viz::ReturnedResource>& resources) override;
  void SetMemoryPolicy(const ManagedMemoryPolicy& policy) override;
  void SetTreeActivationCallback(const base::Closure& callback) override;
  void OnDraw(const gfx::Transform& transform,
              const gfx::Rect& viewport,
              bool resourceless_software_draw) override;

  // Called from LayerTreeImpl.
  void OnCanDrawStateChangedForTree();

  // Implementation.
  int id() const { return id_; }
  bool CanDraw() const;
  LayerTreeFrameSink* layer_tree_frame_sink() const {
    return layer_tree_frame_sink_;
  }
  void ReleaseLayerTreeFrameSink();

  std::string LayerListAsJson() const;
  // TODO(pdr): This should be removed because there is no longer a tree
  // of layers, only a list.
  std::string LayerTreeAsJson() const;

  int RequestedMSAASampleCount() const;

  // TODO(danakj): Rename this, there is no renderer.
  virtual bool InitializeRenderer(LayerTreeFrameSink* layer_tree_frame_sink);
  TileManager* tile_manager() { return &tile_manager_; }

  void SetHasGpuRasterizationTrigger(bool flag);
  void SetContentHasSlowPaths(bool flag);
  void SetContentHasNonAAPaint(bool flag);
  void GetGpuRasterizationCapabilities(bool* gpu_rasterization_enabled,
                                       bool* gpu_rasterization_supported,
                                       int* max_msaa_samples,
                                       bool* supports_disable_msaa);
  bool use_gpu_rasterization() const { return use_gpu_rasterization_; }
  bool use_msaa() const { return use_msaa_; }

  GpuRasterizationStatus gpu_rasterization_status() const {
    return gpu_rasterization_status_;
  }

  bool create_low_res_tiling() const {
    return settings_.create_low_res_tiling && !use_gpu_rasterization_;
  }
  ResourcePool* resource_pool() { return resource_pool_.get(); }
  ImageDecodeCache* image_decode_cache() { return image_decode_cache_.get(); }
  ImageAnimationController* image_animation_controller() {
    return &image_animation_controller_;
  }

  virtual bool WillBeginImplFrame(const viz::BeginFrameArgs& args);
  virtual void DidFinishImplFrame();
  void DidNotProduceFrame(const viz::BeginFrameAck& ack);
  void DidModifyTilePriorities();

  LayerTreeImpl* active_tree() { return active_tree_.get(); }
  const LayerTreeImpl* active_tree() const { return active_tree_.get(); }
  LayerTreeImpl* pending_tree() { return pending_tree_.get(); }
  const LayerTreeImpl* pending_tree() const { return pending_tree_.get(); }
  LayerTreeImpl* recycle_tree() { return recycle_tree_.get(); }
  const LayerTreeImpl* recycle_tree() const { return recycle_tree_.get(); }
  // Returns the tree LTH synchronizes with.
  LayerTreeImpl* sync_tree() const {
    return CommitToActiveTree() ? active_tree_.get() : pending_tree_.get();
  }
  virtual void CreatePendingTree();
  virtual void ActivateSyncTree();

  // Shortcuts to layers/nodes on the active tree.
  LayerImpl* InnerViewportContainerLayer() const;
  LayerImpl* InnerViewportScrollLayer() const;
  ScrollNode* InnerViewportScrollNode() const;
  LayerImpl* OuterViewportContainerLayer() const;
  LayerImpl* OuterViewportScrollLayer() const;
  ScrollNode* OuterViewportScrollNode() const;
  ScrollNode* CurrentlyScrollingNode();
  const ScrollNode* CurrentlyScrollingNode() const;

  bool scroll_affects_scroll_handler() const {
    return scroll_affects_scroll_handler_;
  }
  void QueueSwapPromiseForMainThreadScrollUpdate(
      std::unique_ptr<SwapPromise> swap_promise);

  bool IsActivelyScrolling() const;

  virtual void SetVisible(bool visible);
  bool visible() const { return visible_; }

  void SetNeedsCommit() { client_->SetNeedsCommitOnImplThread(); }
  void SetNeedsOneBeginImplFrame();
  void SetNeedsRedraw();

  ManagedMemoryPolicy ActualManagedMemoryPolicy() const;

  void SetViewportSize(const gfx::Size& device_viewport_size);
  gfx::Size device_viewport_size() const { return device_viewport_size_; }

  void SetViewportVisibleRect(const gfx::Rect& visible_rect);
  gfx::Rect viewport_visible_rect() const { return viewport_visible_rect_; }

  const gfx::Transform& DrawTransform() const;

  std::unique_ptr<ScrollAndScaleSet> ProcessScrollDeltas();
  FrameRateCounter* fps_counter() { return fps_counter_.get(); }
  MemoryHistory* memory_history() { return memory_history_.get(); }
  DebugRectHistory* debug_rect_history() { return debug_rect_history_.get(); }
  LayerTreeResourceProvider* resource_provider() {
    return resource_provider_.get();
  }
  BrowserControlsOffsetManager* browser_controls_manager() {
    return browser_controls_offset_manager_.get();
  }
  const GlobalStateThatImpactsTilePriority& global_tile_state() {
    return global_tile_state_;
  }

  TaskRunnerProvider* task_runner_provider() const {
    return task_runner_provider_;
  }

  MutatorHost* mutator_host() const { return mutator_host_.get(); }

  void SetDebugState(const LayerTreeDebugState& new_debug_state);
  const LayerTreeDebugState& debug_state() const { return debug_state_; }

  gfx::Vector2dF accumulated_root_overscroll() const {
    return accumulated_root_overscroll_;
  }

  bool pinch_gesture_active() const { return pinch_gesture_active_; }

  void SetTreePriority(TreePriority priority);
  TreePriority GetTreePriority() const;

  // TODO(mithro): Remove this methods which exposes the internal
  // viz::BeginFrameArgs to external callers.
  virtual viz::BeginFrameArgs CurrentBeginFrameArgs() const;

  // Expected time between two begin impl frame calls.
  base::TimeDelta CurrentBeginFrameInterval() const;

  void AsValueWithFrameInto(FrameData* frame,
                            base::trace_event::TracedValue* value) const;
  std::unique_ptr<base::trace_event::ConvertableToTraceFormat> AsValueWithFrame(
      FrameData* frame) const;
  void ActivationStateAsValueInto(base::trace_event::TracedValue* value) const;

  bool page_scale_animation_active() const { return !!page_scale_animation_; }

  virtual void CreateUIResource(UIResourceId uid,
                                const UIResourceBitmap& bitmap);
  // Deletes a UI resource.  May safely be called more than once.
  virtual void DeleteUIResource(UIResourceId uid);
  void EvictAllUIResources();
  bool EvictedUIResourcesExist() const;

  virtual viz::ResourceId ResourceIdForUIResource(UIResourceId uid) const;

  virtual bool IsUIResourceOpaque(UIResourceId uid) const;

  struct UIResourceData {
    viz::ResourceId resource_id;
    bool opaque;
  };

  // Returns the amount of delta that can be applied to scroll_node, taking
  // page scale into account.
  gfx::Vector2dF ComputeScrollDelta(const ScrollNode& scroll_node,
                                    const gfx::Vector2dF& delta);

  void ScheduleMicroBenchmark(std::unique_ptr<MicroBenchmarkImpl> benchmark);

  viz::CompositorFrameMetadata MakeCompositorFrameMetadata();
  RenderFrameMetadata MakeRenderFrameMetadata();

  // Viewport rectangle and clip in device space.  These rects are used to
  // prioritize raster and determine what is submitted in a CompositorFrame.
  gfx::Rect DeviceViewport() const;
  // Viewport rect to be used for tiling prioritization instead of the
  // DeviceViewport().
  const gfx::Rect ViewportRectForTilePriority() const;

  // When a SwapPromiseMonitor is created on the impl thread, it calls
  // InsertSwapPromiseMonitor() to register itself with LayerTreeHostImpl.
  // When the monitor is destroyed, it calls RemoveSwapPromiseMonitor()
  // to unregister itself.
  void InsertSwapPromiseMonitor(SwapPromiseMonitor* monitor);
  void RemoveSwapPromiseMonitor(SwapPromiseMonitor* monitor);

  // TODO(weiliangc): Replace RequiresHighResToDraw with scheduler waits for
  // ReadyToDraw. crbug.com/469175
  void SetRequiresHighResToDraw() { requires_high_res_to_draw_ = true; }
  void ResetRequiresHighResToDraw() { requires_high_res_to_draw_ = false; }
  bool RequiresHighResToDraw() const { return requires_high_res_to_draw_; }

  // Only valid for synchronous (non-scheduled) single-threaded case.
  void SynchronouslyInitializeAllTiles();

  bool SupportsImplScrolling() const;
  bool CommitToActiveTree() const;

  // Virtual so tests can inject their own.
  virtual std::unique_ptr<RasterBufferProvider> CreateRasterBufferProvider();

  bool prepare_tiles_needed() const { return tile_priorities_dirty_; }

  gfx::Vector2dF ScrollSingleNode(ScrollNode* scroll_node,
                                  const gfx::Vector2dF& delta,
                                  const gfx::Point& viewport_point,
                                  bool is_direct_manipulation,
                                  ScrollTree* scroll_tree);

  base::SingleThreadTaskRunner* GetTaskRunner() const {
    DCHECK(task_runner_provider_);
    return task_runner_provider_->HasImplThread()
               ? task_runner_provider_->ImplThreadTaskRunner()
               : task_runner_provider_->MainThreadTaskRunner();
  }

  InputHandler::ScrollStatus TryScroll(const gfx::PointF& screen_space_point,
                                       InputHandler::ScrollInputType type,
                                       const ScrollTree& scroll_tree,
                                       ScrollNode* scroll_node) const;

  // Returns true if a scroll offset animation is created and false if we scroll
  // by the desired amount without an animation.
  bool ScrollAnimationCreate(ScrollNode* scroll_node,
                             const gfx::Vector2dF& scroll_amount,
                             base::TimeDelta delayed_by);

  void SetLayerTreeMutator(std::unique_ptr<LayerTreeMutator> mutator);

  LayerImpl* ViewportMainScrollLayer();

  void QueueImageDecode(int request_id, const PaintImage& image);
  std::vector<std::pair<int, bool>> TakeCompletedImageDecodeRequests();

  void DidNavigate();

  bool CanConsumeDelta(const ScrollNode& scroll_node,
                       const ScrollState& scroll_state);

  void UpdateImageDecodingHints(
      base::flat_map<PaintImage::Id, PaintImage::DecodingMode>
          decoding_mode_map);

  void InitializeUkm(std::unique_ptr<ukm::UkmRecorder> recorder);
  UkmManager* ukm_manager() { return ukm_manager_.get(); }

  void RenewTreePriorityForTesting() { client_->RenewTreePriority(); }

  void SetRenderFrameObserver(
      std::unique_ptr<RenderFrameMetadataObserver> observer);

 protected:
  LayerTreeHostImpl(
      const LayerTreeSettings& settings,
      LayerTreeHostImplClient* client,
      TaskRunnerProvider* task_runner_provider,
      RenderingStatsInstrumentation* rendering_stats_instrumentation,
      TaskGraphRunner* task_graph_runner,
      std::unique_ptr<MutatorHost> mutator_host,
      int id,
      scoped_refptr<base::SequencedTaskRunner> image_worker_task_runner);

  // Virtual for testing.
  virtual bool AnimateLayers(base::TimeTicks monotonic_time,
                             bool is_active_tree);

  bool is_likely_to_require_a_draw() const {
    return is_likely_to_require_a_draw_;
  }

  // Removes empty or orphan RenderPasses from the frame.
  static void RemoveRenderPasses(FrameData* frame);

  LayerTreeHostImplClient* client_;
  TaskRunnerProvider* task_runner_provider_;

  BeginFrameTracker current_begin_frame_tracker_;

 private:
  // Transforms viewport start point and scroll delta to local start point and
  // local delta, respectively. If the transformation of either the start or end
  // point of a scroll is clipped, the function returns false.
  bool CalculateLocalScrollDeltaAndStartPoint(
      const ScrollNode& scroll_node,
      const gfx::PointF& viewport_point,
      const gfx::Vector2dF& viewport_delta,
      const ScrollTree& scroll_tree,
      gfx::Vector2dF* out_local_scroll_delta,
      gfx::PointF* out_local_start_point = nullptr);
  gfx::Vector2dF ScrollNodeWithViewportSpaceDelta(
      ScrollNode* scroll_node,
      const gfx::PointF& viewport_point,
      const gfx::Vector2dF& viewport_delta,
      ScrollTree* scroll_tree);

  void CleanUpTileManagerResources();
  void CreateTileManagerResources();
  void ReleaseTreeResources();
  void ReleaseTileResources();
  void RecreateTileResources();

  void AnimateInternal();

  // The function is called to update state on the sync tree after a commit
  // finishes or after the sync tree was created to invalidate content on the
  // impl thread.
  void UpdateSyncTreeAfterCommitOrImplSideInvalidation();

  // Returns true if status changed.
  bool UpdateGpuRasterizationStatus();
  void UpdateTreeResourcesForGpuRasterizationIfNeeded();

  Viewport* viewport() const { return viewport_.get(); }

  InputHandler::ScrollStatus ScrollBeginImpl(
      ScrollState* scroll_state,
      ScrollNode* scrolling_node,
      InputHandler::ScrollInputType type);
  bool IsInitialScrollHitTestReliable(LayerImpl* layer, const gfx::PointF&);
  void DistributeScrollDelta(ScrollState* scroll_state);

  bool AnimatePageScale(base::TimeTicks monotonic_time);
  bool AnimateScrollbars(base::TimeTicks monotonic_time);
  bool AnimateBrowserControls(base::TimeTicks monotonic_time);

  void UpdateTileManagerMemoryPolicy(const ManagedMemoryPolicy& policy);

  // Returns true if the damage rect is non-empty. This check includes damage
  // from the HUD. Should only be called when the active tree's draw properties
  // are valid and after updating the damage.
  bool HasDamage() const;

  // This function should only be called from PrepareToDraw, as DidDrawAllLayers
  // must be called if this helper function is called.  Returns DRAW_SUCCESS if
  // the frame should be drawn.
  DrawResult CalculateRenderPasses(FrameData* frame);

  void ClearCurrentlyScrollingNode();

  ScrollNode* FindScrollNodeForDeviceViewportPoint(
      const gfx::PointF& device_viewport_point,
      InputHandler::ScrollInputType type,
      LayerImpl* layer_hit_by_point,
      bool* scroll_on_main_thread,
      uint32_t* main_thread_scrolling_reason) const;
  void StartScrollbarFadeRecursive(LayerImpl* layer);
  void SetManagedMemoryPolicy(const ManagedMemoryPolicy& policy);

  void MarkUIResourceNotEvicted(UIResourceId uid);
  void ClearUIResources();

  void NotifySwapPromiseMonitorsOfSetNeedsRedraw();
  void NotifySwapPromiseMonitorsOfForwardingToMainThread();

  void UpdateRootLayerStateForSynchronousInputHandler();

  bool ScrollAnimationUpdateTarget(ScrollNode* scroll_node,
                                   const gfx::Vector2dF& scroll_delta,
                                   base::TimeDelta delayed_by);

  void ScrollEndImpl(ScrollState* scroll_state);

  // Creates an animation curve and returns true if we need to update the
  // scroll position to a snap point. Otherwise returns false.
  bool SnapAtScrollEnd();

  void SetContextVisibility(bool is_visible);
  void ImageDecodeFinished(int request_id, bool decode_succeeded);

  // This function keeps track of sources of scrolls that are handled in the
  // compositor side. The information gets shared by the main thread as part of
  // the begin_main_frame_state. Finally Use counters are updated in the main
  // thread side to keep track of the frequency of scrolling with different
  // sources per page load. TODO(crbug.com/691886): Use GRC API to plumb the
  // scroll source info for Use Counters.
  void UpdateScrollSourceInfo(bool is_wheel_scroll);

  bool IsScrolledBy(LayerImpl* child, ScrollNode* ancestor);
  void ShowScrollbarsForImplScroll(ElementId element_id);

  // Copy any opacity values already in the active tree to the pending
  // tree, because the active tree value always takes precedence for scrollbars.
  void PushScrollbarOpacitiesFromActiveToPending();

  // Request an impl-side invalidation to animate an image.
  void RequestInvalidationForAnimatedImages();

  // Pushes state for image animations and checkerboarded images from the
  // pending to active tree. This is called during activation when a pending
  // tree exists, and during the commit if we are committing directly to the
  // active tree.
  void ActivateStateForImages();

  using UIResourceMap = std::unordered_map<UIResourceId, UIResourceData>;
  UIResourceMap ui_resource_map_;

  // Resources that were evicted by EvictAllUIResources. Resources are removed
  // from this when they are touched by a create or destroy from the UI resource
  // request queue.
  std::set<UIResourceId> evicted_ui_resources_;

  LayerTreeFrameSink* layer_tree_frame_sink_;

  // The following scoped variables must not outlive the
  // |layer_tree_frame_sink_|.
  // These should be transfered to viz::ContextCacheController's
  // ClientBecameNotVisible() before the output surface is destroyed.
  std::unique_ptr<viz::ContextCacheController::ScopedVisibility>
      compositor_context_visibility_;
  std::unique_ptr<viz::ContextCacheController::ScopedVisibility>
      worker_context_visibility_;

  std::unique_ptr<LayerTreeResourceProvider> resource_provider_;
  bool need_update_gpu_rasterization_status_;
  bool content_has_slow_paths_;
  bool content_has_non_aa_paint_;
  bool has_gpu_rasterization_trigger_;
  bool use_gpu_rasterization_;
  bool use_msaa_;
  GpuRasterizationStatus gpu_rasterization_status_;
  std::unique_ptr<RasterBufferProvider> raster_buffer_provider_;
  std::unique_ptr<ResourcePool> resource_pool_;
  std::unique_ptr<ImageDecodeCache> image_decode_cache_;

  GlobalStateThatImpactsTilePriority global_tile_state_;

  // Tree currently being drawn.
  std::unique_ptr<LayerTreeImpl> active_tree_;

  // In impl-side painting mode, tree with possibly incomplete rasterized
  // content. May be promoted to active by ActivateSyncTree().
  std::unique_ptr<LayerTreeImpl> pending_tree_;

  // In impl-side painting mode, inert tree with layers that can be recycled
  // by the next sync from the main thread.
  std::unique_ptr<LayerTreeImpl> recycle_tree_;

  InputHandlerClient* input_handler_client_;
  bool did_lock_scrolling_layer_;
  bool wheel_scrolling_;
  bool scroll_affects_scroll_handler_;
  ElementId scroll_element_id_mouse_currently_over_;
  ElementId scroll_element_id_mouse_currently_captured_;

  std::vector<std::unique_ptr<SwapPromise>>
      swap_promises_for_main_thread_scroll_update_;

  // An object to implement the ScrollElasticityHelper interface and
  // hold all state related to elasticity. May be NULL if never requested.
  std::unique_ptr<ScrollElasticityHelper> scroll_elasticity_helper_;

  bool tile_priorities_dirty_;

  const LayerTreeSettings settings_;
  LayerTreeDebugState debug_state_;
  bool visible_;
  ManagedMemoryPolicy cached_managed_memory_policy_;

  const bool is_synchronous_single_threaded_;
  TileManager tile_manager_;
  DecodedImageTracker decoded_image_tracker_;

  gfx::Vector2dF accumulated_root_overscroll_;

  // True iff some of the delta has been consumed for the current scroll
  // sequence on the specific axis.
  bool did_scroll_x_for_scroll_gesture_;
  bool did_scroll_y_for_scroll_gesture_;

  bool pinch_gesture_active_;
  bool pinch_gesture_end_should_clear_scrolling_node_;

  std::unique_ptr<BrowserControlsOffsetManager>
      browser_controls_offset_manager_;

  std::unique_ptr<PageScaleAnimation> page_scale_animation_;

  std::unique_ptr<FrameRateCounter> fps_counter_;
  std::unique_ptr<MemoryHistory> memory_history_;
  std::unique_ptr<DebugRectHistory> debug_rect_history_;

  // The maximum memory that would be used by the prioritized resource
  // manager, if there were no limit on memory usage.
  size_t max_memory_needed_bytes_;

  // Viewport size passed in from the main thread, in physical pixels.  This
  // value is the default size for all concepts of physical viewport (draw
  // viewport, scrolling viewport and device viewport), but it can be
  // overridden.
  gfx::Size device_viewport_size_;

  // Viewport clip rect passed in from the main thrad, in physical pixels.
  // This is used for out-of-process iframes whose size exceeds the window
  // in order to prevent full raster.
  gfx::Rect viewport_visible_rect_;

  // Optional top-level constraints that can be set by the LayerTreeFrameSink.
  // - external_transform_ applies a transform above the root layer
  // - external_viewport_ is used DrawProperties, tile management and
  // glViewport/window projection matrix.
  // - viewport_rect_for_tile_priority_ is the rect in view space used for
  // tiling priority.
  gfx::Transform external_transform_;
  gfx::Rect external_viewport_;
  gfx::Rect viewport_rect_for_tile_priority_;
  bool resourceless_software_draw_;

  gfx::Rect viewport_damage_rect_;

  std::unique_ptr<MutatorHost> mutator_host_;
  std::set<VideoFrameController*> video_frame_controllers_;

  // Map from scroll element ID to scrollbar animation controller.
  // There is one animation controller per pair of overlay scrollbars.
  std::unordered_map<ElementId,
                     std::unique_ptr<ScrollbarAnimationController>,
                     ElementIdHash>
      scrollbar_animation_controllers_;

  RenderingStatsInstrumentation* rendering_stats_instrumentation_;
  MicroBenchmarkControllerImpl micro_benchmark_controller_;
  std::unique_ptr<SynchronousTaskGraphRunner>
      single_thread_synchronous_task_graph_runner_;

  // Optional callback to notify of new tree activations.
  base::Closure tree_activation_callback_;

  TaskGraphRunner* task_graph_runner_;
  int id_;

  std::set<SwapPromiseMonitor*> swap_promise_monitor_;

  bool requires_high_res_to_draw_;
  bool is_likely_to_require_a_draw_;

  // TODO(danakj): Delete the LayerTreeFrameSink and all resources when
  // it's lost instead of having this bool.
  bool has_valid_layer_tree_frame_sink_;

  // If it is enabled in the LayerTreeSettings, we can check damage in
  // WillBeginImplFrame and abort early if there is no damage. We only check
  // damage in WillBeginImplFrame if a recent frame had no damage. We keep
  // track of this with |consecutive_frame_with_damage_count_|.
  int consecutive_frame_with_damage_count_;

  std::unique_ptr<Viewport> viewport_;

  std::unique_ptr<PendingTreeDurationHistogramTimer>
      pending_tree_duration_timer_;
  std::unique_ptr<PendingTreeRasterDurationHistogramTimer>
      pending_tree_raster_duration_timer_;

  // The element id of the scroll node to which scroll animations must latch.
  // This gets reset at ScrollAnimatedBegin, and updated the first time that a
  // scroll animation is created in ScrollAnimated. We need to use element ids
  // instead of node ids since they are stable across the property tree update
  // in SetPropertyTrees.
  ElementId scroll_animating_latched_element_id_;

  // These completion states to be transfered to the main thread when we
  // begin main frame. The pair represents a request id and the completion (ie
  // success) state.
  std::vector<std::pair<int, bool>> completed_image_decode_requests_;

  // These are used to transfer usage of touch and wheel scrolls to the main
  // thread.
  bool has_scrolled_by_wheel_;
  bool has_scrolled_by_touch_;

  bool touchpad_and_wheel_scroll_latching_enabled_;

  ImplThreadPhase impl_thread_phase_;

  ImageAnimationController image_animation_controller_;

  std::unique_ptr<UkmManager> ukm_manager_;

  // Provides RenderFrameMetadata to the Browser process upon the submission of
  // each CompositorFrame.
  std::unique_ptr<RenderFrameMetadataObserver> render_frame_metadata_observer_;
  FrameTokenAllocator frame_token_allocator_;

  // Maps from presentation_token set on CF to the source frame that requested
  // it. Presentation tokens are requested if the active tree has
  // request_presentation_time() set.
  base::flat_map<uint32_t, int> presentation_token_to_frame_;

  // If non-zero identifies the presentation-token added to the last CF. Reset
  // to zero when no more presentation tokens are in flight.
  uint32_t last_presentation_token_ = 0u;

  viz::LocalSurfaceId last_draw_local_surface_id_;

  const int default_color_space_id_;
  const gfx::ColorSpace default_color_space_;

  DISALLOW_COPY_AND_ASSIGN(LayerTreeHostImpl);
};

}  // namespace cc

#endif  // CC_TREES_LAYER_TREE_HOST_IMPL_H_
