// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/css/properties/shorthands/webkit_border_before.h"

#include "core/css/parser/CSSPropertyParserHelpers.h"
#include "core/style_property_shorthand.h"

namespace blink {
namespace CSSShorthand {

bool WebkitBorderBefore::ParseShorthand(
    bool important,
    CSSParserTokenRange& range,
    const CSSParserContext& context,
    const CSSParserLocalContext&,
    HeapVector<CSSPropertyValue, 256>& properties) const {
  return CSSPropertyParserHelpers::ConsumeShorthandGreedilyViaLonghands(
      webkitBorderBeforeShorthand(), important, context, range, properties);
}

}  // namespace CSSShorthand
}  // namespace blink
