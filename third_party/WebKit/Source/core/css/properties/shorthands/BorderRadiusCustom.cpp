// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/css/properties/shorthands/border_radius.h"

#include "core/css/CSSValuePair.h"
#include "core/css/parser/CSSParserContext.h"
#include "core/css/parser/CSSParserLocalContext.h"
#include "core/css/parser/CSSPropertyParserHelpers.h"
#include "core/css/properties/CSSParsingUtils.h"
#include "core/css/properties/ComputedStyleUtils.h"
#include "core/style/ComputedStyle.h"

namespace blink {
namespace CSSShorthand {

bool BorderRadius::ParseShorthand(
    bool important,
    CSSParserTokenRange& range,
    const CSSParserContext& context,
    const CSSParserLocalContext& local_context,
    HeapVector<CSSPropertyValue, 256>& properties) const {
  CSSValue* horizontal_radii[4] = {nullptr};
  CSSValue* vertical_radii[4] = {nullptr};

  if (!CSSParsingUtils::ConsumeRadii(horizontal_radii, vertical_radii, range,
                                     context.Mode(),
                                     local_context.UseAliasParsing()))
    return false;

  CSSPropertyParserHelpers::AddProperty(
      CSSPropertyBorderTopLeftRadius, CSSPropertyBorderRadius,
      *CSSValuePair::Create(horizontal_radii[0], vertical_radii[0],
                            CSSValuePair::kDropIdenticalValues),
      important, CSSPropertyParserHelpers::IsImplicitProperty::kNotImplicit,
      properties);
  CSSPropertyParserHelpers::AddProperty(
      CSSPropertyBorderTopRightRadius, CSSPropertyBorderRadius,
      *CSSValuePair::Create(horizontal_radii[1], vertical_radii[1],
                            CSSValuePair::kDropIdenticalValues),
      important, CSSPropertyParserHelpers::IsImplicitProperty::kNotImplicit,
      properties);
  CSSPropertyParserHelpers::AddProperty(
      CSSPropertyBorderBottomRightRadius, CSSPropertyBorderRadius,
      *CSSValuePair::Create(horizontal_radii[2], vertical_radii[2],
                            CSSValuePair::kDropIdenticalValues),
      important, CSSPropertyParserHelpers::IsImplicitProperty::kNotImplicit,
      properties);
  CSSPropertyParserHelpers::AddProperty(
      CSSPropertyBorderBottomLeftRadius, CSSPropertyBorderRadius,
      *CSSValuePair::Create(horizontal_radii[3], vertical_radii[3],
                            CSSValuePair::kDropIdenticalValues),
      important, CSSPropertyParserHelpers::IsImplicitProperty::kNotImplicit,
      properties);
  return true;
}

const CSSValue* BorderRadius::CSSValueFromComputedStyleInternal(
    const ComputedStyle& style,
    const SVGComputedStyle&,
    const LayoutObject*,
    Node* styled_node,
    bool allow_visited_style) const {
  return ComputedStyleUtils::ValueForBorderRadiusShorthand(style);
}

}  // namespace CSSShorthand
}  // namespace blink
