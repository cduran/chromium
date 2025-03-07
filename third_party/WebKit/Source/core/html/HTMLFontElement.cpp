/*
 * Copyright (C) 1999 Lars Knoll (knoll@kde.org)
 *           (C) 1999 Antti Koivisto (koivisto@kde.org)
 *           (C) 2000 Simon Hausmann <hausmann@kde.org>
 * Copyright (C) 2003, 2006, 2008, 2010 Apple Inc. All rights reserved.
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
 */

#include "core/html/HTMLFontElement.h"

#include "core/css/CSSPropertyValueSet.h"
#include "core/css/CSSValueList.h"
#include "core/css/CSSValuePool.h"
#include "core/css/parser/CSSParser.h"
#include "core/css_property_names.h"
#include "core/css_value_keywords.h"
#include "core/dom/Document.h"
#include "core/html/parser/HTMLParserIdioms.h"
#include "core/html_names.h"
#include "platform/wtf/text/ParsingUtilities.h"
#include "platform/wtf/text/StringToNumber.h"

namespace blink {

using namespace cssvalue;
using namespace HTMLNames;

inline HTMLFontElement::HTMLFontElement(Document& document)
    : HTMLElement(fontTag, document) {}

DEFINE_NODE_FACTORY(HTMLFontElement)

// http://www.whatwg.org/specs/web-apps/current-work/multipage/rendering.html#fonts-and-colors
template <typename CharacterType>
static bool ParseFontSize(const CharacterType* characters,
                          unsigned length,
                          int& size) {
  // Step 1
  // Step 2
  const CharacterType* position = characters;
  const CharacterType* end = characters + length;

  // Step 3
  SkipWhile<CharacterType, IsHTMLSpace<CharacterType>>(position, end);

  // Step 4
  if (position == end)
    return false;
  DCHECK_LT(position, end);

  // Step 5
  enum { kRelativePlus, kRelativeMinus, kAbsolute } mode;

  switch (*position) {
    case '+':
      mode = kRelativePlus;
      ++position;
      break;
    case '-':
      mode = kRelativeMinus;
      ++position;
      break;
    default:
      mode = kAbsolute;
      break;
  }

  // Step 6
  const CharacterType* digits_start = position;
  SkipWhile<CharacterType, IsASCIIDigit>(position, end);

  // Step 7
  if (digits_start == position)
    return false;

  // Step 8
  int value = CharactersToInt(digits_start, position - digits_start,
                              WTF::NumberParsingOptions::kNone, nullptr);

  // Step 9
  if (mode == kRelativePlus)
    value += 3;
  else if (mode == kRelativeMinus)
    value = 3 - value;

  // Step 10
  if (value > 7)
    value = 7;

  // Step 11
  if (value < 1)
    value = 1;

  size = value;
  return true;
}

static bool ParseFontSize(const String& input, int& size) {
  if (input.IsEmpty())
    return false;

  if (input.Is8Bit())
    return ParseFontSize(input.Characters8(), input.length(), size);

  return ParseFontSize(input.Characters16(), input.length(), size);
}

static const CSSValueList* CreateFontFaceValueWithPool(
    const AtomicString& string,
    SecureContextMode secure_context_mode) {
  CSSValuePool::FontFaceValueCache::AddResult entry =
      CssValuePool().GetFontFaceCacheEntry(string);
  if (!entry.stored_value->value) {
    const CSSValue* parsed_value = CSSParser::ParseSingleValue(
        CSSPropertyFontFamily, string,
        StrictCSSParserContext(secure_context_mode));
    if (parsed_value && parsed_value->IsValueList())
      entry.stored_value->value = ToCSSValueList(parsed_value);
  }
  return entry.stored_value->value;
}

bool HTMLFontElement::CssValueFromFontSizeNumber(const String& s,
                                                 CSSValueID& size) {
  int num = 0;
  if (!ParseFontSize(s, num))
    return false;

  switch (num) {
    case 1:
      // FIXME: The spec says that we're supposed to use CSSValueXxSmall here.
      size = CSSValueXSmall;
      break;
    case 2:
      size = CSSValueSmall;
      break;
    case 3:
      size = CSSValueMedium;
      break;
    case 4:
      size = CSSValueLarge;
      break;
    case 5:
      size = CSSValueXLarge;
      break;
    case 6:
      size = CSSValueXxLarge;
      break;
    case 7:
      size = CSSValueWebkitXxxLarge;
      break;
    default:
      NOTREACHED();
  }
  return true;
}

bool HTMLFontElement::IsPresentationAttribute(const QualifiedName& name) const {
  if (name == sizeAttr || name == colorAttr || name == faceAttr)
    return true;
  return HTMLElement::IsPresentationAttribute(name);
}

void HTMLFontElement::CollectStyleForPresentationAttribute(
    const QualifiedName& name,
    const AtomicString& value,
    MutableCSSPropertyValueSet* style) {
  if (name == sizeAttr) {
    CSSValueID size = CSSValueInvalid;
    if (CssValueFromFontSizeNumber(value, size))
      AddPropertyToPresentationAttributeStyle(style, CSSPropertyFontSize, size);
  } else if (name == colorAttr) {
    AddHTMLColorToStyle(style, CSSPropertyColor, value);
  } else if (name == faceAttr && !value.IsEmpty()) {
    if (const CSSValueList* font_face_value = CreateFontFaceValueWithPool(
            value, GetDocument().GetSecureContextMode())) {
      style->SetProperty(
          CSSPropertyValue(GetCSSPropertyFontFamily(), *font_face_value));
    }
  } else {
    HTMLElement::CollectStyleForPresentationAttribute(name, value, style);
  }
}

}  // namespace blink
