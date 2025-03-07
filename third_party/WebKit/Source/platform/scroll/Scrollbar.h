/*
 * Copyright (C) 2004, 2006 Apple Computer, Inc.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef Scrollbar_h
#define Scrollbar_h

#include "platform/Timer.h"
#include "platform/graphics/paint/DisplayItem.h"
#include "platform/heap/Handle.h"
#include "platform/scroll/ScrollTypes.h"
#include "platform/scroll/ScrollbarThemeClient.h"
#include "platform/wtf/MathExtras.h"

namespace blink {

class CullRect;
class GraphicsContext;
class IntRect;
class PlatformChromeClient;
class ScrollableArea;
class ScrollbarTheme;
class WebGestureEvent;
class WebMouseEvent;

class PLATFORM_EXPORT Scrollbar : public GarbageCollectedFinalized<Scrollbar>,
                                  public ScrollbarThemeClient,
                                  public DisplayItemClient {
 public:
  static Scrollbar* Create(ScrollableArea* scrollable_area,
                           ScrollbarOrientation orientation,
                           ScrollbarControlSize size,
                           PlatformChromeClient* chrome_client) {
    return new Scrollbar(scrollable_area, orientation, size, chrome_client);
  }

  // Theme object ownership remains with the caller and it must outlive the
  // scrollbar.
  static Scrollbar* CreateForTesting(ScrollableArea* scrollable_area,
                                     ScrollbarOrientation orientation,
                                     ScrollbarControlSize size,
                                     ScrollbarTheme* theme) {
    return new Scrollbar(scrollable_area, orientation, size, nullptr, theme);
  }

  ~Scrollbar() override;

  // ScrollbarThemeClient implementation.
  int X() const override { return frame_rect_.X(); }
  int Y() const override { return frame_rect_.Y(); }
  int Width() const override { return frame_rect_.Width(); }
  int Height() const override { return frame_rect_.Height(); }
  IntSize Size() const override { return frame_rect_.Size(); }
  IntPoint Location() const override { return frame_rect_.Location(); }

  void SetFrameRect(const IntRect&);
  IntRect FrameRect() const override { return frame_rect_; }

  ScrollbarOverlayColorTheme GetScrollbarOverlayColorTheme() const override;
  void GetTickmarks(Vector<IntRect>&) const override;
  bool IsScrollableAreaActive() const override;

  IntPoint ConvertFromRootFrame(const IntPoint&) const override;

  bool IsCustomScrollbar() const override { return false; }
  ScrollbarOrientation Orientation() const override { return orientation_; }
  bool IsLeftSideVerticalScrollbar() const override;

  int Value() const override { return lroundf(current_pos_); }
  float CurrentPos() const override { return current_pos_; }
  int VisibleSize() const override { return visible_size_; }
  int TotalSize() const override { return total_size_; }
  int Maximum() const override { return total_size_ - visible_size_; }
  ScrollbarControlSize GetControlSize() const override { return control_size_; }

  ScrollbarPart PressedPart() const override { return pressed_part_; }
  ScrollbarPart HoveredPart() const override { return hovered_part_; }

  void StyleChanged() override {}
  void SetScrollbarsHiddenIfOverlay(bool) override;
  bool Enabled() const override { return enabled_; }
  void SetEnabled(bool) override;

  // This returns device-scale-factor-aware pixel value.
  // e.g. 15 in dsf=1.0, 30 in dsf=2.0.
  // This returns 0 for overlay scrollbars.
  // See also ScrolbarTheme::scrollbatThickness().
  int ScrollbarThickness() const;

  // Called by the ScrollableArea when the scroll offset changes.
  // Will trigger paint invalidation if required.
  void OffsetDidChange();

  virtual void DisconnectFromScrollableArea();
  ScrollableArea* GetScrollableArea() const { return scrollable_area_; }

  int PressedPos() const { return pressed_pos_; }

  virtual void SetHoveredPart(ScrollbarPart);
  virtual void SetPressedPart(ScrollbarPart);

  void SetProportion(int visible_size, int total_size);
  void SetPressedPos(int p) { pressed_pos_ = p; }

  void Paint(GraphicsContext&, const CullRect&) const;

  bool IsOverlayScrollbar() const override;
  bool ShouldParticipateInHitTesting();

  bool IsWindowActive() const;

  // Return if the gesture event was handled. |shouldUpdateCapture|
  // will be set to true if the handler should update the capture
  // state for this scrollbar.
  bool GestureEvent(const WebGestureEvent&, bool* should_update_capture);

  // These methods are used for platform scrollbars to give :hover feedback.
  // They will not get called when the mouse went down in a scrollbar, since it
  // is assumed the scrollbar will start
  // grabbing all events in that case anyway.
  void MouseMoved(const WebMouseEvent&);
  void MouseEntered();
  void MouseExited();

  // Used by some platform scrollbars to know when they've been released from
  // capture.
  void MouseUp(const WebMouseEvent&);
  void MouseDown(const WebMouseEvent&);

  ScrollbarTheme& GetTheme() const { return theme_; }

  IntRect ConvertToContainingEmbeddedContentView(const IntRect&) const;
  IntPoint ConvertFromContainingEmbeddedContentView(const IntPoint&) const;

  void MoveThumb(int pos, bool dragging_document = false);

  float ElasticOverscroll() const override { return elastic_overscroll_; }
  void SetElasticOverscroll(float elastic_overscroll) override {
    elastic_overscroll_ = elastic_overscroll;
  }

  // Use setNeedsPaintInvalidation to cause the scrollbar (or parts thereof)
  // to repaint.
  bool TrackNeedsRepaint() const { return track_needs_repaint_; }
  void ClearTrackNeedsRepaint() { track_needs_repaint_ = false; }
  bool ThumbNeedsRepaint() const { return thumb_needs_repaint_; }
  void ClearThumbNeedsRepaint() { thumb_needs_repaint_ = false; }

  // DisplayItemClient methods.
  String DebugName() const final {
    return orientation_ == kHorizontalScrollbar ? "HorizontalScrollbar"
                                                : "VerticalScrollbar";
  }
  LayoutRect VisualRect() const final { return visual_rect_; }

  virtual void SetVisualRect(const LayoutRect& r) { visual_rect_ = r; }

  // Marks the scrollbar as needing to be redrawn.
  //
  // If invalid parts are provided, then those parts will also be repainted.
  // Otherwise, the ScrollableArea may redraw using cached renderings of
  // individual parts. For instance, if the scrollbar is composited, the thumb
  // may be cached in a GPU texture (and is only guaranteed to be repainted if
  // ThumbPart is invalidated).
  //
  // Even if no parts are invalidated, the scrollbar may need to be redrawn
  // if, for instance, the thumb moves without changing the appearance of any
  // part.
  void SetNeedsPaintInvalidation(ScrollbarPart invalid_parts);

  // Promptly unregister from the theme manager + run finalizers of derived
  // Scrollbars.
  EAGERLY_FINALIZE();
  DECLARE_EAGER_FINALIZATION_OPERATOR_NEW();
  virtual void Trace(blink::Visitor*);

 protected:
  Scrollbar(ScrollableArea*,
            ScrollbarOrientation,
            ScrollbarControlSize,
            PlatformChromeClient* = nullptr,
            ScrollbarTheme* = nullptr);

  void AutoscrollTimerFired(TimerBase*);
  void StartTimerIfNeeded(double delay);
  void StopTimerIfNeeded();
  void AutoscrollPressedPart(double delay);
  ScrollDirectionPhysical PressedPartScrollDirectionPhysical();
  ScrollGranularity PressedPartScrollGranularity();

  Member<ScrollableArea> scrollable_area_;
  ScrollbarOrientation orientation_;
  ScrollbarControlSize control_size_;
  ScrollbarTheme& theme_;
  Member<PlatformChromeClient> chrome_client_;

  int visible_size_;
  int total_size_;
  float current_pos_;
  float drag_origin_;

  ScrollbarPart hovered_part_;
  ScrollbarPart pressed_part_;
  int pressed_pos_;
  float scroll_pos_;
  bool dragging_document_;
  int document_drag_pos_;

  bool enabled_;

  TaskRunnerTimer<Scrollbar> scroll_timer_;

  float elastic_overscroll_;

 private:
  void Invalidate() override { SetNeedsPaintInvalidation(kAllParts); }
  void InvalidateRect(const IntRect&) override {
    SetNeedsPaintInvalidation(kAllParts);
  }

  float ScrollableAreaCurrentPos() const;
  float ScrollableAreaTargetPos() const;
  bool ThumbWillBeUnderMouse() const;

  int theme_scrollbar_thickness_;
  bool track_needs_repaint_;
  bool thumb_needs_repaint_;
  LayoutRect visual_rect_;
  IntRect frame_rect_;
};

}  // namespace blink

#endif  // Scrollbar_h
