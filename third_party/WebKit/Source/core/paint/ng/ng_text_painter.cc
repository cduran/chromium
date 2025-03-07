// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/paint/ng/ng_text_painter.h"

#include "core/css_property_names.h"
#include "core/frame/Settings.h"
#include "core/layout/ng/inline/ng_physical_text_fragment.h"
#include "core/layout/ng/ng_unpositioned_float.h"
#include "core/paint/BoxPainter.h"
#include "core/paint/PaintInfo.h"
#include "core/style/ComputedStyle.h"
#include "core/style/ShadowList.h"
#include "platform/fonts/Font.h"
#include "platform/fonts/NGTextFragmentPaintInfo.h"
#include "platform/graphics/GraphicsContext.h"
#include "platform/graphics/GraphicsContextStateSaver.h"
#include "platform/graphics/paint/PaintController.h"
#include "platform/wtf/Assertions.h"
#include "platform/wtf/text/CharacterNames.h"

namespace blink {

void NGTextPainter::Paint(unsigned start_offset,
                          unsigned end_offset,
                          unsigned length,
                          const TextPaintStyle& text_style) {
  GraphicsContextStateSaver state_saver(graphics_context_, false);
  UpdateGraphicsContext(text_style, state_saver);
  // TODO(layout-dev): Handle combine text here or elsewhere.
  PaintInternal<kPaintText>(start_offset, end_offset, length);

  if (!emphasis_mark_.IsEmpty()) {
    if (text_style.emphasis_mark_color != text_style.fill_color)
      graphics_context_.SetFillColor(text_style.emphasis_mark_color);
    PaintInternal<kPaintEmphasisMark>(start_offset, end_offset, length);
  }
}

template <NGTextPainter::PaintInternalStep step>
void NGTextPainter::PaintInternalFragment(
    NGTextFragmentPaintInfo& fragment_paint_info,
    unsigned from,
    unsigned to) {
  DCHECK(from <= fragment_paint_info.text.length());
  DCHECK(to <= fragment_paint_info.text.length());

  fragment_paint_info.from = from;
  fragment_paint_info.to = to;

  if (step == kPaintEmphasisMark) {
    graphics_context_.DrawEmphasisMarks(
        font_, fragment_paint_info, emphasis_mark_,
        FloatPoint(text_origin_) + IntSize(0, emphasis_mark_offset_));
  } else {
    DCHECK(step == kPaintText);
    graphics_context_.DrawText(font_, fragment_paint_info,
                               FloatPoint(text_origin_));
    // TODO(npm): Check that there are non-whitespace characters. See
    // crbug.com/788444.
    graphics_context_.GetPaintController().SetTextPainted();
  }
}

template <NGTextPainter::PaintInternalStep Step>
void NGTextPainter::PaintInternal(unsigned start_offset,
                                  unsigned end_offset,
                                  unsigned truncation_point) {
  // TODO(layout-dev): We shouldn't be creating text fragments without text.
  if (!fragment_.TextShapeResult())
    return;

  NGTextFragmentPaintInfo paint_info = fragment_.PaintInfo();

  if (start_offset <= end_offset) {
    PaintInternalFragment<Step>(paint_info, start_offset, end_offset);
  } else {
    if (end_offset > 0)
      PaintInternalFragment<Step>(paint_info, ellipsis_offset_, end_offset);
    if (start_offset < truncation_point)
      PaintInternalFragment<Step>(paint_info, start_offset, truncation_point);
  }
}

void NGTextPainter::ClipDecorationsStripe(float upper,
                                          float stripe_width,
                                          float dilation) {
  if (!fragment_.Length() || !fragment_.TextShapeResult())
    return;

  NGTextFragmentPaintInfo fragment_paint_info = fragment_.PaintInfo();
  Vector<Font::TextIntercept> text_intercepts;
  font_.GetTextIntercepts(
      fragment_paint_info, graphics_context_.DeviceScaleFactor(),
      graphics_context_.FillFlags(),
      std::make_tuple(upper, upper + stripe_width), text_intercepts);

  DecorationsStripeIntercepts(upper, stripe_width, dilation, text_intercepts);
}

void NGTextPainter::PaintEmphasisMarkForCombinedText() {}

}  // namespace blink
