// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/css/properties/longhands/dominant_baseline.h"

namespace blink {
namespace CSSLonghand {

const CSSValue* DominantBaseline::CSSValueFromComputedStyleInternal(
    const ComputedStyle& style,
    const SVGComputedStyle& svg_style,
    const LayoutObject*,
    Node*,
    bool allow_visited_style) const {
  return CSSIdentifierValue::Create(svg_style.DominantBaseline());
}

}  // namespace CSSLonghand
}  // namespace blink
