// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/base/ime/composition_text.h"

#include <stddef.h>

#include "base/strings/utf_string_conversions.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ui {

TEST(CompositionTextTest, CopyTest) {
  const base::string16 kSampleText = base::UTF8ToUTF16("Sample Text");
  const ImeTextSpan kSampleUnderline1(
      ImeTextSpan::Type::kComposition, 10, 20, SK_ColorBLACK,
      ImeTextSpan::Thickness::kThin, SK_ColorTRANSPARENT);

  const ImeTextSpan kSampleUnderline2(
      ImeTextSpan::Type::kComposition, 11, 21, SK_ColorBLACK,
      ImeTextSpan::Thickness::kThick, SK_ColorTRANSPARENT);

  const ImeTextSpan kSampleUnderline3(
      ImeTextSpan::Type::kComposition, 12, 22, SK_ColorRED,
      ImeTextSpan::Thickness::kThin, SK_ColorTRANSPARENT);

  // Make CompositionText
  CompositionText text;
  text.text = kSampleText;
  text.ime_text_spans.push_back(kSampleUnderline1);
  text.ime_text_spans.push_back(kSampleUnderline2);
  text.ime_text_spans.push_back(kSampleUnderline3);
  text.selection.set_start(30);
  text.selection.set_end(40);

  CompositionText text2 = text;

  EXPECT_EQ(text.text, text2.text);
  EXPECT_EQ(text.ime_text_spans.size(), text2.ime_text_spans.size());
  for (size_t i = 0; i < text.ime_text_spans.size(); ++i) {
    EXPECT_EQ(text.ime_text_spans[i].start_offset,
              text2.ime_text_spans[i].start_offset);
    EXPECT_EQ(text.ime_text_spans[i].end_offset,
              text2.ime_text_spans[i].end_offset);
    EXPECT_EQ(text.ime_text_spans[i].underline_color,
              text2.ime_text_spans[i].underline_color);
    EXPECT_EQ(text.ime_text_spans[i].thickness,
              text2.ime_text_spans[i].thickness);
    EXPECT_EQ(text.ime_text_spans[i].background_color,
              text2.ime_text_spans[i].background_color);
  }

  EXPECT_EQ(text.selection.start(), text2.selection.start());
  EXPECT_EQ(text.selection.end(), text2.selection.end());
}

}  // namespace ui
