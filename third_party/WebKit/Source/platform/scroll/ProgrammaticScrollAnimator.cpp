// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/scroll/ProgrammaticScrollAnimator.h"

#include <memory>
#include "platform/animation/CompositorKeyframeModel.h"
#include "platform/animation/CompositorScrollOffsetAnimationCurve.h"
#include "platform/geometry/IntSize.h"
#include "platform/graphics/GraphicsLayer.h"
#include "platform/scroll/ScrollableArea.h"
#include "platform/scroll/SmoothScrollSequencer.h"
#include "public/platform/Platform.h"
#include "public/platform/WebCompositorSupport.h"

namespace blink {

ProgrammaticScrollAnimator::ProgrammaticScrollAnimator(
    ScrollableArea* scrollable_area)
    : scrollable_area_(scrollable_area), start_time_(0.0) {}

ProgrammaticScrollAnimator::~ProgrammaticScrollAnimator() = default;

void ProgrammaticScrollAnimator::ResetAnimationState() {
  ScrollAnimatorCompositorCoordinator::ResetAnimationState();
  animation_curve_.reset();
  start_time_ = 0.0;
}

void ProgrammaticScrollAnimator::NotifyOffsetChanged(
    const ScrollOffset& offset) {
  ScrollType scroll_type =
      is_sequenced_scroll_ ? kSequencedScroll : kProgrammaticScroll;
  ScrollOffsetChanged(offset, scroll_type);
}

void ProgrammaticScrollAnimator::ScrollToOffsetWithoutAnimation(
    const ScrollOffset& offset,
    bool is_sequenced_scroll) {
  CancelAnimation();
  is_sequenced_scroll_ = is_sequenced_scroll;
  NotifyOffsetChanged(offset);
  is_sequenced_scroll_ = false;
  if (SmoothScrollSequencer* sequencer =
          GetScrollableArea()->GetSmoothScrollSequencer())
    sequencer->RunQueuedAnimations();
}

void ProgrammaticScrollAnimator::AnimateToOffset(const ScrollOffset& offset,
                                                 bool is_sequenced_scroll) {
  if (run_state_ == RunState::kPostAnimationCleanup)
    ResetAnimationState();

  start_time_ = 0.0;
  target_offset_ = offset;
  is_sequenced_scroll_ = is_sequenced_scroll;
  animation_curve_ = CompositorScrollOffsetAnimationCurve::Create(
      CompositorOffsetFromBlinkOffset(target_offset_),
      CompositorScrollOffsetAnimationCurve::kScrollDurationDeltaBased);

  scrollable_area_->RegisterForAnimation();
  if (!scrollable_area_->ScheduleAnimation()) {
    ResetAnimationState();
    NotifyOffsetChanged(offset);
  }
  run_state_ = RunState::kWaitingToSendToCompositor;
}

void ProgrammaticScrollAnimator::CancelAnimation() {
  DCHECK_NE(run_state_, RunState::kRunningOnCompositorButNeedsUpdate);
  ScrollAnimatorCompositorCoordinator::CancelAnimation();
}

void ProgrammaticScrollAnimator::TickAnimation(double monotonic_time) {
  if (run_state_ != RunState::kRunningOnMainThread)
    return;

  if (!start_time_)
    start_time_ = monotonic_time;
  double elapsed_time = monotonic_time - start_time_;
  bool is_finished = (elapsed_time > animation_curve_->Duration());
  ScrollOffset offset =
      BlinkOffsetFromCompositorOffset(animation_curve_->GetValue(elapsed_time));
  NotifyOffsetChanged(offset);

  if (is_finished) {
    run_state_ = RunState::kPostAnimationCleanup;
    AnimationFinished();
  } else if (!scrollable_area_->ScheduleAnimation()) {
    NotifyOffsetChanged(offset);
    ResetAnimationState();
  }
}

void ProgrammaticScrollAnimator::UpdateCompositorAnimations() {
  if (run_state_ == RunState::kPostAnimationCleanup) {
    // No special cleanup, simply reset animation state. We have this state
    // here because the state machine is shared with ScrollAnimator which
    // has to do some cleanup that requires the compositing state to be clean.
    return ResetAnimationState();
  }

  if (compositor_animation_id_ &&
      run_state_ != RunState::kRunningOnCompositor) {
    // If the current run state is WaitingToSendToCompositor but we have a
    // non-zero compositor animation id, there's a currently running
    // compositor animation that needs to be removed here before the new
    // animation is added below.
    DCHECK(run_state_ == RunState::kWaitingToCancelOnCompositor ||
           run_state_ == RunState::kWaitingToSendToCompositor);

    RemoveAnimation();

    compositor_animation_id_ = 0;
    compositor_animation_group_id_ = 0;
    if (run_state_ == RunState::kWaitingToCancelOnCompositor) {
      ResetAnimationState();
      return;
    }
  }

  if (run_state_ == RunState::kWaitingToSendToCompositor) {
    if (!element_id_)
      ReattachCompositorAnimationIfNeeded(
          GetScrollableArea()->GetCompositorAnimationTimeline());

    bool sent_to_compositor = false;

    // TODO(sunyunjia): Sequenced Smooth Scroll should also be able to
    // scroll on the compositor thread. We should send the ScrollType
    // information to the compositor thread.
    // crbug.com/730705
    if (!scrollable_area_->ShouldScrollOnMainThread() &&
        !is_sequenced_scroll_) {
      std::unique_ptr<CompositorKeyframeModel> animation =
          CompositorKeyframeModel::Create(
              *animation_curve_, CompositorTargetProperty::SCROLL_OFFSET, 0, 0);

      int animation_id = animation->Id();
      int animation_group_id = animation->Group();

      if (AddAnimation(std::move(animation))) {
        sent_to_compositor = true;
        run_state_ = RunState::kRunningOnCompositor;
        compositor_animation_id_ = animation_id;
        compositor_animation_group_id_ = animation_group_id;
      }
    }

    if (!sent_to_compositor) {
      run_state_ = RunState::kRunningOnMainThread;
      animation_curve_->SetInitialValue(
          CompositorOffsetFromBlinkOffset(scrollable_area_->GetScrollOffset()));
      if (!scrollable_area_->ScheduleAnimation()) {
        NotifyOffsetChanged(target_offset_);
        ResetAnimationState();
      }
    }
  }
}

void ProgrammaticScrollAnimator::LayerForCompositedScrollingDidChange(
    CompositorAnimationTimeline* timeline) {
  ReattachCompositorAnimationIfNeeded(timeline);

  // If the composited scrolling layer is lost during a composited animation,
  // continue the animation on the main thread.
  if (run_state_ == RunState::kRunningOnCompositor &&
      !scrollable_area_->LayerForScrolling()) {
    run_state_ = RunState::kRunningOnMainThread;
    compositor_animation_id_ = 0;
    compositor_animation_group_id_ = 0;
    animation_curve_->SetInitialValue(
        CompositorOffsetFromBlinkOffset(scrollable_area_->GetScrollOffset()));
    scrollable_area_->RegisterForAnimation();
    if (!scrollable_area_->ScheduleAnimation()) {
      ResetAnimationState();
      NotifyOffsetChanged(target_offset_);
    }
  }
}

void ProgrammaticScrollAnimator::NotifyCompositorAnimationFinished(
    int group_id) {
  DCHECK_NE(run_state_, RunState::kRunningOnCompositorButNeedsUpdate);
  ScrollAnimatorCompositorCoordinator::CompositorAnimationFinished(group_id);
  AnimationFinished();
}

void ProgrammaticScrollAnimator::AnimationFinished() {
  if (is_sequenced_scroll_) {
    is_sequenced_scroll_ = false;
    if (SmoothScrollSequencer* sequencer =
            GetScrollableArea()->GetSmoothScrollSequencer())
      sequencer->RunQueuedAnimations();
  }
}

void ProgrammaticScrollAnimator::Trace(blink::Visitor* visitor) {
  visitor->Trace(scrollable_area_);
  ScrollAnimatorCompositorCoordinator::Trace(visitor);
}

}  // namespace blink
