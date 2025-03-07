// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef TextDecorationOffset_h
#define TextDecorationOffset_h

#include "core/CoreExport.h"
#include "core/layout/TextDecorationOffsetBase.h"
#include "core/layout/api/LineLayoutItem.h"

namespace blink {

class ComputedStyle;
class InlineTextBox;

class CORE_EXPORT TextDecorationOffset : public TextDecorationOffsetBase {
  STACK_ALLOCATED();

 public:
  TextDecorationOffset(const ComputedStyle& style,
                       const InlineTextBox* inline_text_box,
                       LineLayoutItem decorating_box)
      : TextDecorationOffsetBase(style),
        inline_text_box_(inline_text_box),
        decorating_box_(decorating_box) {}
  ~TextDecorationOffset() = default;

  int ComputeUnderlineOffsetForUnder(float text_decoration_thickness,
                                     FontVerticalPositionType) const override;

 private:
  const InlineTextBox* inline_text_box_;
  LineLayoutItem decorating_box_;
};

}  // namespace blink

#endif  // TextDecorationOffset_h
