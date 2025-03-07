// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CSSParser_h
#define CSSParser_h

#include <memory>
#include "core/CoreExport.h"
#include "core/css/CSSPropertyValueSet.h"
#include "core/css/parser/CSSParserContext.h"
#include "core/css_property_names.h"

namespace blink {

class Color;
class CSSParserObserver;
class CSSSelectorList;
class Element;
class ImmutableCSSPropertyValueSet;
class StyleRuleBase;
class StyleRuleKeyframe;
class StyleSheetContents;
class CSSValue;
enum class SecureContextMode;

// This class serves as the public API for the css/parser subsystem
class CORE_EXPORT CSSParser {
  STATIC_ONLY(CSSParser);

 public:
  // As well as regular rules, allows @import and @namespace but not @charset
  static StyleRuleBase* ParseRule(const CSSParserContext*,
                                  StyleSheetContents*,
                                  const String&);
  static void ParseSheet(const CSSParserContext*,
                         StyleSheetContents*,
                         const String&,
                         bool defer_property_parsing = false);
  static CSSSelectorList ParseSelector(const CSSParserContext*,
                                       StyleSheetContents*,
                                       const String&);
  static CSSSelectorList ParsePageSelector(const CSSParserContext&,
                                           StyleSheetContents*,
                                           const String&);
  static bool ParseDeclarationList(const CSSParserContext*,
                                   MutableCSSPropertyValueSet*,
                                   const String&);

  static MutableCSSPropertyValueSet::SetResult ParseValue(
      MutableCSSPropertyValueSet*,
      CSSPropertyID unresolved_property,
      const String&,
      bool important,
      SecureContextMode);
  static MutableCSSPropertyValueSet::SetResult ParseValue(
      MutableCSSPropertyValueSet*,
      CSSPropertyID unresolved_property,
      const String&,
      bool important,
      SecureContextMode,
      StyleSheetContents*);

  static MutableCSSPropertyValueSet::SetResult ParseValueForCustomProperty(
      MutableCSSPropertyValueSet*,
      const AtomicString& property_name,
      const PropertyRegistry*,
      const String& value,
      bool important,
      SecureContextMode,
      StyleSheetContents*,
      bool is_animation_tainted);

  // This is for non-shorthands only
  static const CSSValue* ParseSingleValue(CSSPropertyID,
                                          const String&,
                                          const CSSParserContext*);

  static const CSSValue* ParseFontFaceDescriptor(CSSPropertyID,
                                                 const String&,
                                                 const CSSParserContext*);

  static ImmutableCSSPropertyValueSet* ParseInlineStyleDeclaration(
      const String&,
      Element*);

  static std::unique_ptr<Vector<double>> ParseKeyframeKeyList(const String&);
  static StyleRuleKeyframe* ParseKeyframeRule(const CSSParserContext*,
                                              const String&);

  static bool ParseSupportsCondition(const String&, SecureContextMode);

  // The color will only be changed when string contains a valid CSS color, so
  // callers can set it to a default color and ignore the boolean result.
  static bool ParseColor(Color&, const String&, bool strict = false);
  static bool ParseSystemColor(Color&, const String&);

  static void ParseSheetForInspector(const CSSParserContext*,
                                     StyleSheetContents*,
                                     const String&,
                                     CSSParserObserver&);
  static void ParseDeclarationListForInspector(const CSSParserContext*,
                                               const String&,
                                               CSSParserObserver&);

 private:
  static MutableCSSPropertyValueSet::SetResult ParseValue(
      MutableCSSPropertyValueSet*,
      CSSPropertyID unresolved_property,
      const String&,
      bool important,
      const CSSParserContext*);
};

}  // namespace blink

#endif  // CSSParser_h
