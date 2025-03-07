// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/css/properties/longhands/scroll_margin_block_end.h"

#include "core/css/parser/CSSParserContext.h"
#include "core/css/parser/CSSPropertyParserHelpers.h"
#include "core/css/properties/ComputedStyleUtils.h"
#include "core/style_property_shorthand.h"

namespace blink {
namespace CSSLonghand {

const CSSValue* ScrollMarginBlockEnd::ParseSingleValue(
    CSSParserTokenRange& range,
    const CSSParserContext& context,
    const CSSParserLocalContext&) const {
  return ConsumeLength(range, context.Mode(), kValueRangeAll,
                       CSSPropertyParserHelpers::UnitlessQuirk::kAllow);
}

}  // namespace CSSLonghand
}  // namespace blink
