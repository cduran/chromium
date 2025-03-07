/*
 * Copyright (C) 1999 Lars Knoll (knoll@kde.org)
 *           (C) 1999 Antti Koivisto (koivisto@kde.org)
 * Copyright (C) 2010 Apple Inc. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#include "core/html/HTMLUListElement.h"

#include "core/css_property_names.h"
#include "core/html_names.h"

namespace blink {

using namespace HTMLNames;

inline HTMLUListElement::HTMLUListElement(Document& document)
    : HTMLElement(ulTag, document) {}

DEFINE_NODE_FACTORY(HTMLUListElement)

bool HTMLUListElement::IsPresentationAttribute(
    const QualifiedName& name) const {
  if (name == typeAttr)
    return true;
  return HTMLElement::IsPresentationAttribute(name);
}

void HTMLUListElement::CollectStyleForPresentationAttribute(
    const QualifiedName& name,
    const AtomicString& value,
    MutableCSSPropertyValueSet* style) {
  if (name == typeAttr) {
    if (DeprecatedEqualIgnoringCase(value, "disc"))
      AddPropertyToPresentationAttributeStyle(style, CSSPropertyListStyleType,
                                              CSSValueDisc);
    else if (DeprecatedEqualIgnoringCase(value, "circle"))
      AddPropertyToPresentationAttributeStyle(style, CSSPropertyListStyleType,
                                              CSSValueCircle);
    else if (DeprecatedEqualIgnoringCase(value, "square"))
      AddPropertyToPresentationAttributeStyle(style, CSSPropertyListStyleType,
                                              CSSValueSquare);
    else if (DeprecatedEqualIgnoringCase(value, "none"))
      AddPropertyToPresentationAttributeStyle(style, CSSPropertyListStyleType,
                                              CSSValueNone);
  } else {
    HTMLElement::CollectStyleForPresentationAttribute(name, value, style);
  }
}

}  // namespace blink
