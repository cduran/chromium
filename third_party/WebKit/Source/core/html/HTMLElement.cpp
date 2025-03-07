/*
 * Copyright (C) 1999 Lars Knoll (knoll@kde.org)
 *           (C) 1999 Antti Koivisto (koivisto@kde.org)
 * Copyright (C) 2004-2008, 2013, 2014 Apple Inc. All rights reserved.
 * Copyright (C) 2009 Torch Mobile Inc. All rights reserved.
 * (http://www.torchmobile.com/)
 * Copyright (C) 2011 Motorola Mobility. All rights reserved.
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

#include "core/html/HTMLElement.h"

#include "bindings/core/v8/ExceptionState.h"
#include "bindings/core/v8/ScriptEventListener.h"
#include "core/css/CSSColorValue.h"
#include "core/css/CSSMarkup.h"
#include "core/css/CSSPropertyValueSet.h"
#include "core/css/StyleChangeReason.h"
#include "core/css_property_names.h"
#include "core/css_value_keywords.h"
#include "core/dom/DocumentFragment.h"
#include "core/dom/ElementShadow.h"
#include "core/dom/ElementTraversal.h"
#include "core/dom/ExceptionCode.h"
#include "core/dom/FlatTreeTraversal.h"
#include "core/dom/NodeComputedStyle.h"
#include "core/dom/NodeTraversal.h"
#include "core/dom/ShadowRoot.h"
#include "core/dom/Text.h"
#include "core/dom/events/EventListener.h"
#include "core/editing/EditingUtilities.h"
#include "core/editing/serializers/Serialization.h"
#include "core/editing/spellcheck/SpellChecker.h"
#include "core/events/KeyboardEvent.h"
#include "core/frame/Settings.h"
#include "core/frame/UseCounter.h"
#include "core/frame/csp/ContentSecurityPolicy.h"
#include "core/html/HTMLBRElement.h"
#include "core/html/HTMLDimension.h"
#include "core/html/HTMLFrameOwnerElement.h"
#include "core/html/HTMLTemplateElement.h"
#include "core/html/forms/HTMLFormElement.h"
#include "core/html/forms/HTMLInputElement.h"
#include "core/html/parser/HTMLParserIdioms.h"
#include "core/html_names.h"
#include "core/input_type_names.h"
#include "core/layout/AdjustForAbsoluteZoom.h"
#include "core/layout/LayoutBoxModelObject.h"
#include "core/layout/LayoutObject.h"
#include "core/mathml_names.h"
#include "core/page/SpatialNavigation.h"
#include "core/svg/SVGSVGElement.h"
#include "core/xml_names.h"
#include "platform/Language.h"
#include "platform/text/BidiResolver.h"
#include "platform/text/BidiTextRun.h"
#include "platform/text/TextRunIterator.h"
#include "platform/wtf/StdLibExtras.h"
#include "platform/wtf/text/CString.h"

namespace blink {

using namespace cssvalue;
using namespace HTMLNames;
using namespace WTF;

using namespace std;

using AttributeChangedFunction =
    void (HTMLElement::*)(const Element::AttributeModificationParams& params);

struct AttributeTriggers {
  const QualifiedName& attribute;
  WebFeature web_feature;
  const AtomicString& event;
  AttributeChangedFunction function;
};

namespace {

// https://w3c.github.io/editing/execCommand.html#editing-host
bool IsEditingHost(const Node& node) {
  if (!node.IsHTMLElement())
    return false;
  String normalized_value = ToHTMLElement(node).contentEditable();
  if (normalized_value == "true" || normalized_value == "plaintext-only")
    return true;
  return node.GetDocument().InDesignMode() &&
         node.GetDocument().documentElement() == &node;
}

// https://w3c.github.io/editing/execCommand.html#editable
bool IsEditable(const Node& node) {
  if (IsEditingHost(node))
    return false;
  if (node.IsHTMLElement() && ToHTMLElement(node).contentEditable() == "false")
    return false;
  if (!node.parentNode())
    return false;
  if (!IsEditingHost(*node.parentNode()) && !IsEditable(*node.parentNode()))
    return false;
  if (node.IsHTMLElement())
    return true;
  if (IsSVGSVGElement(node))
    return true;
  if (node.IsElementNode() && ToElement(node).HasTagName(MathMLNames::mathTag))
    return true;
  return !node.IsElementNode() && node.parentNode()->IsHTMLElement();
}

const WebFeature kNoWebFeature = static_cast<WebFeature>(0);

}  // anonymous namespace

DEFINE_ELEMENT_FACTORY_WITH_TAGNAME(HTMLElement);

String HTMLElement::DebugNodeName() const {
  if (GetDocument().IsHTMLDocument()) {
    return TagQName().HasPrefix() ? Element::nodeName().UpperASCII()
                                  : TagQName().LocalName().UpperASCII();
  }
  return Element::nodeName();
}

String HTMLElement::nodeName() const {
  // localNameUpper may intern and cache an AtomicString.
  DCHECK(IsMainThread());

  // FIXME: Would be nice to have an atomicstring lookup based off uppercase
  // chars that does not have to copy the string on a hit in the hash.
  // FIXME: We should have a way to detect XHTML elements and replace the
  // hasPrefix() check with it.
  if (GetDocument().IsHTMLDocument()) {
    if (!TagQName().HasPrefix())
      return TagQName().LocalNameUpper();
    return Element::nodeName().UpperASCII();
  }
  return Element::nodeName();
}

bool HTMLElement::ShouldSerializeEndTag() const {
  // See https://www.w3.org/TR/DOM-Parsing/
  if (HasTagName(areaTag) || HasTagName(baseTag) || HasTagName(basefontTag) ||
      HasTagName(bgsoundTag) || HasTagName(brTag) || HasTagName(colTag) ||
      HasTagName(embedTag) || HasTagName(frameTag) || HasTagName(hrTag) ||
      HasTagName(imgTag) || HasTagName(inputTag) || HasTagName(keygenTag) ||
      HasTagName(linkTag) || HasTagName(metaTag) || HasTagName(paramTag) ||
      HasTagName(sourceTag) || HasTagName(trackTag) || HasTagName(wbrTag))
    return false;
  return true;
}

static inline CSSValueID UnicodeBidiAttributeForDirAuto(HTMLElement* element) {
  if (element->HasTagName(preTag) || element->HasTagName(textareaTag))
    return CSSValueWebkitPlaintext;
  // FIXME: For bdo element, dir="auto" should result in "bidi-override isolate"
  // but we don't support having multiple values in unicode-bidi yet.
  // See https://bugs.webkit.org/show_bug.cgi?id=73164.
  return CSSValueWebkitIsolate;
}

unsigned HTMLElement::ParseBorderWidthAttribute(
    const AtomicString& value) const {
  unsigned border_width = 0;
  if (value.IsEmpty() || !ParseHTMLNonNegativeInteger(value, border_width)) {
    if (HasTagName(tableTag) && !value.IsNull())
      return 1;
  }
  return border_width;
}

void HTMLElement::ApplyBorderAttributeToStyle(
    const AtomicString& value,
    MutableCSSPropertyValueSet* style) {
  AddPropertyToPresentationAttributeStyle(style, CSSPropertyBorderWidth,
                                          ParseBorderWidthAttribute(value),
                                          CSSPrimitiveValue::UnitType::kPixels);
  AddPropertyToPresentationAttributeStyle(style, CSSPropertyBorderStyle,
                                          CSSValueSolid);
}

void HTMLElement::MapLanguageAttributeToLocale(
    const AtomicString& value,
    MutableCSSPropertyValueSet* style) {
  if (!value.IsEmpty()) {
    // Have to quote so the locale id is treated as a string instead of as a CSS
    // keyword.
    AddPropertyToPresentationAttributeStyle(style, CSSPropertyWebkitLocale,
                                            SerializeString(value));

    // FIXME: Remove the following UseCounter code when we collect enough
    // data.
    UseCounter::Count(GetDocument(), WebFeature::kLangAttribute);
    if (IsHTMLHtmlElement(*this))
      UseCounter::Count(GetDocument(), WebFeature::kLangAttributeOnHTML);
    else if (IsHTMLBodyElement(*this))
      UseCounter::Count(GetDocument(), WebFeature::kLangAttributeOnBody);
    String html_language = value.GetString();
    size_t first_separator = html_language.find('-');
    if (first_separator != kNotFound)
      html_language = html_language.Left(first_separator);
    String ui_language = DefaultLanguage();
    first_separator = ui_language.find('-');
    if (first_separator != kNotFound)
      ui_language = ui_language.Left(first_separator);
    first_separator = ui_language.find('_');
    if (first_separator != kNotFound)
      ui_language = ui_language.Left(first_separator);
    if (!DeprecatedEqualIgnoringCase(html_language, ui_language)) {
      UseCounter::Count(GetDocument(),
                        WebFeature::kLangAttributeDoesNotMatchToUILocale);
    }
  } else {
    // The empty string means the language is explicitly unknown.
    AddPropertyToPresentationAttributeStyle(style, CSSPropertyWebkitLocale,
                                            CSSValueAuto);
  }
}

bool HTMLElement::IsPresentationAttribute(const QualifiedName& name) const {
  if (name == alignAttr || name == contenteditableAttr || name == hiddenAttr ||
      name == langAttr || name.Matches(XMLNames::langAttr) ||
      name == draggableAttr || name == dirAttr)
    return true;
  return Element::IsPresentationAttribute(name);
}

static inline bool IsValidDirAttribute(const AtomicString& value) {
  return DeprecatedEqualIgnoringCase(value, "auto") ||
         DeprecatedEqualIgnoringCase(value, "ltr") ||
         DeprecatedEqualIgnoringCase(value, "rtl");
}

void HTMLElement::CollectStyleForPresentationAttribute(
    const QualifiedName& name,
    const AtomicString& value,
    MutableCSSPropertyValueSet* style) {
  if (name == alignAttr) {
    if (DeprecatedEqualIgnoringCase(value, "middle"))
      AddPropertyToPresentationAttributeStyle(style, CSSPropertyTextAlign,
                                              CSSValueCenter);
    else
      AddPropertyToPresentationAttributeStyle(style, CSSPropertyTextAlign,
                                              value);
  } else if (name == contenteditableAttr) {
    if (value.IsEmpty() || DeprecatedEqualIgnoringCase(value, "true")) {
      AddPropertyToPresentationAttributeStyle(
          style, CSSPropertyWebkitUserModify, CSSValueReadWrite);
      AddPropertyToPresentationAttributeStyle(style, CSSPropertyWordWrap,
                                              CSSValueBreakWord);
      AddPropertyToPresentationAttributeStyle(style, CSSPropertyWebkitLineBreak,
                                              CSSValueAfterWhiteSpace);
      UseCounter::Count(GetDocument(), WebFeature::kContentEditableTrue);
      if (HasTagName(htmlTag)) {
        UseCounter::Count(GetDocument(),
                          WebFeature::kContentEditableTrueOnHTML);
      }
    } else if (DeprecatedEqualIgnoringCase(value, "plaintext-only")) {
      AddPropertyToPresentationAttributeStyle(
          style, CSSPropertyWebkitUserModify, CSSValueReadWritePlaintextOnly);
      AddPropertyToPresentationAttributeStyle(style, CSSPropertyWordWrap,
                                              CSSValueBreakWord);
      AddPropertyToPresentationAttributeStyle(style, CSSPropertyWebkitLineBreak,
                                              CSSValueAfterWhiteSpace);
      UseCounter::Count(GetDocument(),
                        WebFeature::kContentEditablePlainTextOnly);
    } else if (DeprecatedEqualIgnoringCase(value, "false")) {
      AddPropertyToPresentationAttributeStyle(
          style, CSSPropertyWebkitUserModify, CSSValueReadOnly);
    }
  } else if (name == hiddenAttr) {
    AddPropertyToPresentationAttributeStyle(style, CSSPropertyDisplay,
                                            CSSValueNone);
  } else if (name == draggableAttr) {
    UseCounter::Count(GetDocument(), WebFeature::kDraggableAttribute);
    if (DeprecatedEqualIgnoringCase(value, "true")) {
      AddPropertyToPresentationAttributeStyle(style, CSSPropertyWebkitUserDrag,
                                              CSSValueElement);
      AddPropertyToPresentationAttributeStyle(style, CSSPropertyUserSelect,
                                              CSSValueNone);
    } else if (DeprecatedEqualIgnoringCase(value, "false")) {
      AddPropertyToPresentationAttributeStyle(style, CSSPropertyWebkitUserDrag,
                                              CSSValueNone);
    }
  } else if (name == dirAttr) {
    if (DeprecatedEqualIgnoringCase(value, "auto")) {
      AddPropertyToPresentationAttributeStyle(
          style, CSSPropertyUnicodeBidi, UnicodeBidiAttributeForDirAuto(this));
    } else {
      if (IsValidDirAttribute(value))
        AddPropertyToPresentationAttributeStyle(style, CSSPropertyDirection,
                                                value);
      else if (IsHTMLBodyElement(*this))
        AddPropertyToPresentationAttributeStyle(style, CSSPropertyDirection,
                                                "ltr");
      if (!HasTagName(bdiTag) && !HasTagName(bdoTag) && !HasTagName(outputTag))
        AddPropertyToPresentationAttributeStyle(style, CSSPropertyUnicodeBidi,
                                                CSSValueIsolate);
    }
  } else if (name.Matches(XMLNames::langAttr)) {
    MapLanguageAttributeToLocale(value, style);
  } else if (name == langAttr) {
    // xml:lang has a higher priority than lang.
    if (!FastHasAttribute(XMLNames::langAttr))
      MapLanguageAttributeToLocale(value, style);
  } else {
    Element::CollectStyleForPresentationAttribute(name, value, style);
  }
}

// static
AttributeTriggers* HTMLElement::TriggersForAttributeName(
    const QualifiedName& attr_name) {
  const AtomicString& kNoEvent = g_null_atom;
  static AttributeTriggers attribute_triggers[] = {
      {dirAttr, kNoWebFeature, kNoEvent, &HTMLElement::OnDirAttrChanged},
      {inertAttr, WebFeature::kInertAttribute, kNoEvent,
       &HTMLElement::OnInertAttrChanged},
      {langAttr, kNoWebFeature, kNoEvent, &HTMLElement::OnLangAttrChanged},
      {nonceAttr, kNoWebFeature, kNoEvent, &HTMLElement::OnNonceAttrChanged},
      {tabindexAttr, kNoWebFeature, kNoEvent,
       &HTMLElement::OnTabIndexAttrChanged},
      {XMLNames::langAttr, kNoWebFeature, kNoEvent,
       &HTMLElement::OnXMLLangAttrChanged},

      {onabortAttr, kNoWebFeature, EventTypeNames::abort, nullptr},
      {onanimationendAttr, kNoWebFeature, EventTypeNames::animationend,
       nullptr},
      {onanimationiterationAttr, kNoWebFeature,
       EventTypeNames::animationiteration, nullptr},
      {onanimationstartAttr, kNoWebFeature, EventTypeNames::animationstart,
       nullptr},
      {onauxclickAttr, kNoWebFeature, EventTypeNames::auxclick, nullptr},
      {onbeforecopyAttr, kNoWebFeature, EventTypeNames::beforecopy, nullptr},
      {onbeforecutAttr, kNoWebFeature, EventTypeNames::beforecut, nullptr},
      {onbeforepasteAttr, kNoWebFeature, EventTypeNames::beforepaste, nullptr},
      {onblurAttr, kNoWebFeature, EventTypeNames::blur, nullptr},
      {oncancelAttr, kNoWebFeature, EventTypeNames::cancel, nullptr},
      {oncanplayAttr, kNoWebFeature, EventTypeNames::canplay, nullptr},
      {oncanplaythroughAttr, kNoWebFeature, EventTypeNames::canplaythrough,
       nullptr},
      {onchangeAttr, kNoWebFeature, EventTypeNames::change, nullptr},
      {onclickAttr, kNoWebFeature, EventTypeNames::click, nullptr},
      {oncloseAttr, kNoWebFeature, EventTypeNames::close, nullptr},
      {oncontextmenuAttr, kNoWebFeature, EventTypeNames::contextmenu, nullptr},
      {oncopyAttr, kNoWebFeature, EventTypeNames::copy, nullptr},
      {oncuechangeAttr, kNoWebFeature, EventTypeNames::cuechange, nullptr},
      {oncutAttr, kNoWebFeature, EventTypeNames::cut, nullptr},
      {ondblclickAttr, kNoWebFeature, EventTypeNames::dblclick, nullptr},
      {ondragAttr, kNoWebFeature, EventTypeNames::drag, nullptr},
      {ondragendAttr, kNoWebFeature, EventTypeNames::dragend, nullptr},
      {ondragenterAttr, kNoWebFeature, EventTypeNames::dragenter, nullptr},
      {ondragleaveAttr, kNoWebFeature, EventTypeNames::dragleave, nullptr},
      {ondragoverAttr, kNoWebFeature, EventTypeNames::dragover, nullptr},
      {ondragstartAttr, kNoWebFeature, EventTypeNames::dragstart, nullptr},
      {ondropAttr, kNoWebFeature, EventTypeNames::drop, nullptr},
      {ondurationchangeAttr, kNoWebFeature, EventTypeNames::durationchange,
       nullptr},
      {onemptiedAttr, kNoWebFeature, EventTypeNames::emptied, nullptr},
      {onendedAttr, kNoWebFeature, EventTypeNames::ended, nullptr},
      {onerrorAttr, kNoWebFeature, EventTypeNames::error, nullptr},
      {onfocusAttr, kNoWebFeature, EventTypeNames::focus, nullptr},
      {onfocusinAttr, kNoWebFeature, EventTypeNames::focusin, nullptr},
      {onfocusoutAttr, kNoWebFeature, EventTypeNames::focusout, nullptr},
      {ongotpointercaptureAttr, kNoWebFeature,
       EventTypeNames::gotpointercapture, nullptr},
      {oninputAttr, kNoWebFeature, EventTypeNames::input, nullptr},
      {oninvalidAttr, kNoWebFeature, EventTypeNames::invalid, nullptr},
      {onkeydownAttr, kNoWebFeature, EventTypeNames::keydown, nullptr},
      {onkeypressAttr, kNoWebFeature, EventTypeNames::keypress, nullptr},
      {onkeyupAttr, kNoWebFeature, EventTypeNames::keyup, nullptr},
      {onloadAttr, kNoWebFeature, EventTypeNames::load, nullptr},
      {onloadeddataAttr, kNoWebFeature, EventTypeNames::loadeddata, nullptr},
      {onloadedmetadataAttr, kNoWebFeature, EventTypeNames::loadedmetadata,
       nullptr},
      {onloadstartAttr, kNoWebFeature, EventTypeNames::loadstart, nullptr},
      {onlostpointercaptureAttr, kNoWebFeature,
       EventTypeNames::lostpointercapture, nullptr},
      {onmousedownAttr, kNoWebFeature, EventTypeNames::mousedown, nullptr},
      {onmouseenterAttr, kNoWebFeature, EventTypeNames::mouseenter, nullptr},
      {onmouseleaveAttr, kNoWebFeature, EventTypeNames::mouseleave, nullptr},
      {onmousemoveAttr, kNoWebFeature, EventTypeNames::mousemove, nullptr},
      {onmouseoutAttr, kNoWebFeature, EventTypeNames::mouseout, nullptr},
      {onmouseoverAttr, kNoWebFeature, EventTypeNames::mouseover, nullptr},
      {onmouseupAttr, kNoWebFeature, EventTypeNames::mouseup, nullptr},
      {onmousewheelAttr, kNoWebFeature, EventTypeNames::mousewheel, nullptr},
      {onpasteAttr, kNoWebFeature, EventTypeNames::paste, nullptr},
      {onpauseAttr, kNoWebFeature, EventTypeNames::pause, nullptr},
      {onplayAttr, kNoWebFeature, EventTypeNames::play, nullptr},
      {onplayingAttr, kNoWebFeature, EventTypeNames::playing, nullptr},
      {onpointercancelAttr, kNoWebFeature, EventTypeNames::pointercancel,
       nullptr},
      {onpointerdownAttr, kNoWebFeature, EventTypeNames::pointerdown, nullptr},
      {onpointerenterAttr, kNoWebFeature, EventTypeNames::pointerenter,
       nullptr},
      {onpointerleaveAttr, kNoWebFeature, EventTypeNames::pointerleave,
       nullptr},
      {onpointermoveAttr, kNoWebFeature, EventTypeNames::pointermove, nullptr},
      {onpointeroutAttr, kNoWebFeature, EventTypeNames::pointerout, nullptr},
      {onpointeroverAttr, kNoWebFeature, EventTypeNames::pointerover, nullptr},
      {onpointerupAttr, kNoWebFeature, EventTypeNames::pointerup, nullptr},
      {onprogressAttr, kNoWebFeature, EventTypeNames::progress, nullptr},
      {onratechangeAttr, kNoWebFeature, EventTypeNames::ratechange, nullptr},
      {onresetAttr, kNoWebFeature, EventTypeNames::reset, nullptr},
      {onresizeAttr, kNoWebFeature, EventTypeNames::resize, nullptr},
      {onscrollAttr, kNoWebFeature, EventTypeNames::scroll, nullptr},
      {onseekedAttr, kNoWebFeature, EventTypeNames::seeked, nullptr},
      {onseekingAttr, kNoWebFeature, EventTypeNames::seeking, nullptr},
      {onselectAttr, kNoWebFeature, EventTypeNames::select, nullptr},
      {onselectstartAttr, kNoWebFeature, EventTypeNames::selectstart, nullptr},
      {onstalledAttr, kNoWebFeature, EventTypeNames::stalled, nullptr},
      {onsubmitAttr, kNoWebFeature, EventTypeNames::submit, nullptr},
      {onsuspendAttr, kNoWebFeature, EventTypeNames::suspend, nullptr},
      {ontimeupdateAttr, kNoWebFeature, EventTypeNames::timeupdate, nullptr},
      {ontoggleAttr, kNoWebFeature, EventTypeNames::toggle, nullptr},
      {ontouchcancelAttr, kNoWebFeature, EventTypeNames::touchcancel, nullptr},
      {ontouchendAttr, kNoWebFeature, EventTypeNames::touchend, nullptr},
      {ontouchmoveAttr, kNoWebFeature, EventTypeNames::touchmove, nullptr},
      {ontouchstartAttr, kNoWebFeature, EventTypeNames::touchstart, nullptr},
      {ontransitionendAttr, kNoWebFeature, EventTypeNames::webkitTransitionEnd,
       nullptr},
      {onvolumechangeAttr, kNoWebFeature, EventTypeNames::volumechange,
       nullptr},
      {onwaitingAttr, kNoWebFeature, EventTypeNames::waiting, nullptr},
      {onwebkitanimationendAttr, kNoWebFeature,
       EventTypeNames::webkitAnimationEnd, nullptr},
      {onwebkitanimationiterationAttr, kNoWebFeature,
       EventTypeNames::webkitAnimationIteration, nullptr},
      {onwebkitanimationstartAttr, kNoWebFeature,
       EventTypeNames::webkitAnimationStart, nullptr},
      {onwebkitfullscreenchangeAttr, kNoWebFeature,
       EventTypeNames::webkitfullscreenchange, nullptr},
      {onwebkitfullscreenerrorAttr, kNoWebFeature,
       EventTypeNames::webkitfullscreenerror, nullptr},
      {onwebkittransitionendAttr, kNoWebFeature,
       EventTypeNames::webkitTransitionEnd, nullptr},
      {onwheelAttr, kNoWebFeature, EventTypeNames::wheel, nullptr},

      {aria_activedescendantAttr, WebFeature::kARIAActiveDescendantAttribute,
       kNoEvent, nullptr},
      {aria_atomicAttr, WebFeature::kARIAAtomicAttribute, kNoEvent, nullptr},
      {aria_autocompleteAttr, WebFeature::kARIAAutocompleteAttribute, kNoEvent,
       nullptr},
      {aria_busyAttr, WebFeature::kARIABusyAttribute, kNoEvent, nullptr},
      {aria_checkedAttr, WebFeature::kARIACheckedAttribute, kNoEvent, nullptr},
      {aria_colcountAttr, WebFeature::kARIAColCountAttribute, kNoEvent,
       nullptr},
      {aria_colindexAttr, WebFeature::kARIAColIndexAttribute, kNoEvent,
       nullptr},
      {aria_colspanAttr, WebFeature::kARIAColSpanAttribute, kNoEvent, nullptr},
      {aria_controlsAttr, WebFeature::kARIAControlsAttribute, kNoEvent,
       nullptr},
      {aria_currentAttr, WebFeature::kARIACurrentAttribute, kNoEvent, nullptr},
      {aria_describedbyAttr, WebFeature::kARIADescribedByAttribute, kNoEvent,
       nullptr},
      {aria_detailsAttr, WebFeature::kARIADetailsAttribute, kNoEvent, nullptr},
      {aria_disabledAttr, WebFeature::kARIADisabledAttribute, kNoEvent,
       nullptr},
      {aria_dropeffectAttr, WebFeature::kARIADropEffectAttribute, kNoEvent,
       nullptr},
      {aria_errormessageAttr, WebFeature::kARIAErrorMessageAttribute, kNoEvent,
       nullptr},
      {aria_expandedAttr, WebFeature::kARIAExpandedAttribute, kNoEvent,
       nullptr},
      {aria_flowtoAttr, WebFeature::kARIAFlowToAttribute, kNoEvent, nullptr},
      {aria_grabbedAttr, WebFeature::kARIAGrabbedAttribute, kNoEvent, nullptr},
      {aria_haspopupAttr, WebFeature::kARIAHasPopupAttribute, kNoEvent,
       nullptr},
      {aria_helpAttr, WebFeature::kARIAHelpAttribute, kNoEvent, nullptr},
      {aria_hiddenAttr, WebFeature::kARIAHiddenAttribute, kNoEvent, nullptr},
      {aria_invalidAttr, WebFeature::kARIAInvalidAttribute, kNoEvent, nullptr},
      {aria_keyshortcutsAttr, WebFeature::kARIAKeyShortcutsAttribute, kNoEvent,
       nullptr},
      {aria_labelAttr, WebFeature::kARIALabelAttribute, kNoEvent, nullptr},
      {aria_labeledbyAttr, WebFeature::kARIALabeledByAttribute, kNoEvent,
       nullptr},
      {aria_labelledbyAttr, WebFeature::kARIALabelledByAttribute, kNoEvent,
       nullptr},
      {aria_levelAttr, WebFeature::kARIALevelAttribute, kNoEvent, nullptr},
      {aria_liveAttr, WebFeature::kARIALiveAttribute, kNoEvent, nullptr},
      {aria_modalAttr, WebFeature::kARIAModalAttribute, kNoEvent, nullptr},
      {aria_multilineAttr, WebFeature::kARIAMultilineAttribute, kNoEvent,
       nullptr},
      {aria_multiselectableAttr, WebFeature::kARIAMultiselectableAttribute,
       kNoEvent, nullptr},
      {aria_orientationAttr, WebFeature::kARIAOrientationAttribute, kNoEvent,
       nullptr},
      {aria_ownsAttr, WebFeature::kARIAOwnsAttribute, kNoEvent, nullptr},
      {aria_placeholderAttr, WebFeature::kARIAPlaceholderAttribute, kNoEvent,
       nullptr},
      {aria_posinsetAttr, WebFeature::kARIAPosInSetAttribute, kNoEvent,
       nullptr},
      {aria_pressedAttr, WebFeature::kARIAPressedAttribute, kNoEvent, nullptr},
      {aria_readonlyAttr, WebFeature::kARIAReadOnlyAttribute, kNoEvent,
       nullptr},
      {aria_relevantAttr, WebFeature::kARIARelevantAttribute, kNoEvent,
       nullptr},
      {aria_requiredAttr, WebFeature::kARIARequiredAttribute, kNoEvent,
       nullptr},
      {aria_roledescriptionAttr, WebFeature::kARIARoleDescriptionAttribute,
       kNoEvent, nullptr},
      {aria_rowcountAttr, WebFeature::kARIARowCountAttribute, kNoEvent,
       nullptr},
      {aria_rowindexAttr, WebFeature::kARIARowIndexAttribute, kNoEvent,
       nullptr},
      {aria_rowspanAttr, WebFeature::kARIARowSpanAttribute, kNoEvent, nullptr},
      {aria_selectedAttr, WebFeature::kARIASelectedAttribute, kNoEvent,
       nullptr},
      {aria_setsizeAttr, WebFeature::kARIASetSizeAttribute, kNoEvent, nullptr},
      {aria_sortAttr, WebFeature::kARIASortAttribute, kNoEvent, nullptr},
      {aria_valuemaxAttr, WebFeature::kARIAValueMaxAttribute, kNoEvent,
       nullptr},
      {aria_valueminAttr, WebFeature::kARIAValueMinAttribute, kNoEvent,
       nullptr},
      {aria_valuenowAttr, WebFeature::kARIAValueNowAttribute, kNoEvent,
       nullptr},
      {aria_valuetextAttr, WebFeature::kARIAValueTextAttribute, kNoEvent,
       nullptr},
      {autocapitalizeAttr, WebFeature::kAutocapitalizeAttribute, kNoEvent,
       nullptr},
  };

  using AttributeToTriggerIndexMap = HashMap<QualifiedName, int>;
  DEFINE_STATIC_LOCAL(AttributeToTriggerIndexMap,
                      attribute_to_trigger_index_map, ());
  if (!attribute_to_trigger_index_map.size()) {
    for (size_t i = 0; i < arraysize(attribute_triggers); ++i)
      attribute_to_trigger_index_map.insert(attribute_triggers[i].attribute, i);
  }

  auto iter = attribute_to_trigger_index_map.find(attr_name);
  if (iter != attribute_to_trigger_index_map.end())
    return &attribute_triggers[iter->value];
  return nullptr;
}

// static
const AtomicString& HTMLElement::EventNameForAttributeName(
    const QualifiedName& attr_name) {
  AttributeTriggers* triggers = TriggersForAttributeName(attr_name);
  if (triggers)
    return triggers->event;
  return g_null_atom;
}

void HTMLElement::AttributeChanged(const AttributeModificationParams& params) {
  Element::AttributeChanged(params);
  if (params.reason != AttributeModificationReason::kDirectly)
    return;
  // adjustedFocusedElementInTreeScope() is not trivial. We should check
  // attribute names, then call adjustedFocusedElementInTreeScope().
  if (params.name == hiddenAttr && !params.new_value.IsNull()) {
    if (AdjustedFocusedElementInTreeScope() == this)
      blur();
  } else if (params.name == contenteditableAttr) {
    if (GetDocument().GetFrame()) {
      GetDocument()
          .GetFrame()
          ->GetSpellChecker()
          .RemoveSpellingAndGrammarMarkers(
              *this, SpellChecker::ElementsType::kOnlyNonEditable);
    }
    if (AdjustedFocusedElementInTreeScope() != this)
      return;
    // The attribute change may cause supportsFocus() to return false
    // for the element which had focus.
    //
    // TODO(tkent): We should avoid updating style.  We'd like to check only
    // DOM-level focusability here.
    GetDocument().UpdateStyleAndLayoutTreeForNode(this);
    if (!SupportsFocus())
      blur();
  }
}

void HTMLElement::ParseAttribute(const AttributeModificationParams& params) {
  AttributeTriggers* triggers = TriggersForAttributeName(params.name);
  if (!triggers)
    return;

  if (triggers->event != g_null_atom) {
    SetAttributeEventListener(
        triggers->event,
        CreateAttributeEventListener(this, params.name, params.new_value,
                                     EventParameterName()));
  }

  if (triggers->web_feature != kNoWebFeature) {
    // Count usage of attributes but ignore attributes in user agent shadow DOM.
    if (ShadowRoot* shadow = ContainingShadowRoot()) {
      if (shadow->IsUserAgent())
        UseCounter::Count(GetDocument(), triggers->web_feature);
    }
  }

  if (triggers->function)
    ((*this).*(triggers->function))(params);
}

DocumentFragment* HTMLElement::TextToFragment(const String& text,
                                              ExceptionState& exception_state) {
  DocumentFragment* fragment = DocumentFragment::Create(GetDocument());
  unsigned i, length = text.length();
  UChar c = 0;
  for (unsigned start = 0; start < length;) {
    // Find next line break.
    for (i = start; i < length; i++) {
      c = text[i];
      if (c == '\r' || c == '\n')
        break;
    }

    fragment->AppendChild(
        Text::Create(GetDocument(), text.Substring(start, i - start)),
        exception_state);
    if (exception_state.HadException())
      return nullptr;

    if (c == '\r' || c == '\n') {
      fragment->AppendChild(HTMLBRElement::Create(GetDocument()),
                            exception_state);
      if (exception_state.HadException())
        return nullptr;
      // Make sure \r\n doesn't result in two line breaks.
      if (c == '\r' && i + 1 < length && text[i + 1] == '\n')
        i++;
    }

    start = i + 1;  // Character after line break.
  }

  return fragment;
}

void HTMLElement::setInnerText(const String& text,
                               ExceptionState& exception_state) {
  // FIXME: This doesn't take whitespace collapsing into account at all.

  if (!text.Contains('\n') && !text.Contains('\r')) {
    if (text.IsEmpty()) {
      RemoveChildren();
      return;
    }
    ReplaceChildrenWithText(this, text, exception_state);
    return;
  }

  // FIXME: Do we need to be able to detect preserveNewline style even when
  // there's no layoutObject?
  // FIXME: Can the layoutObject be out of date here? Do we need to call
  // updateStyleIfNeeded?  For example, for the contents of textarea elements
  // that are display:none?
  LayoutObject* r = GetLayoutObject();
  if (r && r->Style()->PreserveNewline()) {
    if (!text.Contains('\r')) {
      ReplaceChildrenWithText(this, text, exception_state);
      return;
    }
    String text_with_consistent_line_breaks = text;
    text_with_consistent_line_breaks.Replace("\r\n", "\n");
    text_with_consistent_line_breaks.Replace('\r', '\n');
    ReplaceChildrenWithText(this, text_with_consistent_line_breaks,
                            exception_state);
    return;
  }

  // Add text nodes and <br> elements.
  DocumentFragment* fragment = TextToFragment(text, exception_state);
  if (!exception_state.HadException())
    ReplaceChildrenWithFragment(this, fragment, exception_state);
}

void HTMLElement::setOuterText(const String& text,
                               ExceptionState& exception_state) {
  ContainerNode* parent = parentNode();
  if (!parent) {
    exception_state.ThrowDOMException(kNoModificationAllowedError,
                                      "The element has no parent.");
    return;
  }

  Node* prev = previousSibling();
  Node* next = nextSibling();
  Node* new_child = nullptr;

  // Convert text to fragment with <br> tags instead of linebreaks if needed.
  if (text.Contains('\r') || text.Contains('\n'))
    new_child = TextToFragment(text, exception_state);
  else
    new_child = Text::Create(GetDocument(), text);

  // textToFragment might cause mutation events.
  if (!parentNode())
    exception_state.ThrowDOMException(kHierarchyRequestError,
                                      "The element has no parent.");

  if (exception_state.HadException())
    return;

  parent->ReplaceChild(new_child, this, exception_state);

  Node* node = next ? next->previousSibling() : nullptr;
  if (!exception_state.HadException() && node && node->IsTextNode())
    MergeWithNextTextNode(ToText(node), exception_state);

  if (!exception_state.HadException() && prev && prev->IsTextNode())
    MergeWithNextTextNode(ToText(prev), exception_state);
}

void HTMLElement::ApplyAlignmentAttributeToStyle(
    const AtomicString& alignment,
    MutableCSSPropertyValueSet* style) {
  // Vertical alignment with respect to the current baseline of the text
  // right or left means floating images.
  CSSValueID float_value = CSSValueInvalid;
  CSSValueID vertical_align_value = CSSValueInvalid;

  if (DeprecatedEqualIgnoringCase(alignment, "absmiddle")) {
    vertical_align_value = CSSValueMiddle;
  } else if (DeprecatedEqualIgnoringCase(alignment, "absbottom")) {
    vertical_align_value = CSSValueBottom;
  } else if (DeprecatedEqualIgnoringCase(alignment, "left")) {
    float_value = CSSValueLeft;
    vertical_align_value = CSSValueTop;
  } else if (DeprecatedEqualIgnoringCase(alignment, "right")) {
    float_value = CSSValueRight;
    vertical_align_value = CSSValueTop;
  } else if (DeprecatedEqualIgnoringCase(alignment, "top")) {
    vertical_align_value = CSSValueTop;
  } else if (DeprecatedEqualIgnoringCase(alignment, "middle")) {
    vertical_align_value = CSSValueWebkitBaselineMiddle;
  } else if (DeprecatedEqualIgnoringCase(alignment, "center")) {
    vertical_align_value = CSSValueMiddle;
  } else if (DeprecatedEqualIgnoringCase(alignment, "bottom")) {
    vertical_align_value = CSSValueBaseline;
  } else if (DeprecatedEqualIgnoringCase(alignment, "texttop")) {
    vertical_align_value = CSSValueTextTop;
  }

  if (float_value != CSSValueInvalid)
    AddPropertyToPresentationAttributeStyle(style, CSSPropertyFloat,
                                            float_value);

  if (vertical_align_value != CSSValueInvalid)
    AddPropertyToPresentationAttributeStyle(style, CSSPropertyVerticalAlign,
                                            vertical_align_value);
}

bool HTMLElement::HasCustomFocusLogic() const {
  return false;
}

String HTMLElement::contentEditable() const {
  const AtomicString& value = FastGetAttribute(contenteditableAttr);

  if (value.IsNull())
    return "inherit";
  if (value.IsEmpty() || DeprecatedEqualIgnoringCase(value, "true"))
    return "true";
  if (DeprecatedEqualIgnoringCase(value, "false"))
    return "false";
  if (DeprecatedEqualIgnoringCase(value, "plaintext-only"))
    return "plaintext-only";

  return "inherit";
}

void HTMLElement::setContentEditable(const String& enabled,
                                     ExceptionState& exception_state) {
  if (DeprecatedEqualIgnoringCase(enabled, "true"))
    setAttribute(contenteditableAttr, "true");
  else if (DeprecatedEqualIgnoringCase(enabled, "false"))
    setAttribute(contenteditableAttr, "false");
  else if (DeprecatedEqualIgnoringCase(enabled, "plaintext-only"))
    setAttribute(contenteditableAttr, "plaintext-only");
  else if (DeprecatedEqualIgnoringCase(enabled, "inherit"))
    removeAttribute(contenteditableAttr);
  else
    exception_state.ThrowDOMException(kSyntaxError,
                                      "The value provided ('" + enabled +
                                          "') is not one of 'true', 'false', "
                                          "'plaintext-only', or 'inherit'.");
}

const AtomicString& HTMLElement::autocapitalize() const {
  DEFINE_STATIC_LOCAL(const AtomicString, kOff, ("off"));
  DEFINE_STATIC_LOCAL(const AtomicString, kNone, ("none"));
  DEFINE_STATIC_LOCAL(const AtomicString, kCharacters, ("characters"));
  DEFINE_STATIC_LOCAL(const AtomicString, kWords, ("words"));
  DEFINE_STATIC_LOCAL(const AtomicString, kSentences, ("sentences"));

  const AtomicString& value = FastGetAttribute(autocapitalizeAttr);
  if (value.IsEmpty())
    return g_empty_atom;

  if (EqualIgnoringASCIICase(value, kNone) ||
      EqualIgnoringASCIICase(value, kOff))
    return kNone;
  if (EqualIgnoringASCIICase(value, kCharacters))
    return kCharacters;
  if (EqualIgnoringASCIICase(value, kWords))
    return kWords;
  // "sentences", "on", or an invalid value
  return kSentences;
}

void HTMLElement::setAutocapitalize(const AtomicString& value) {
  setAttribute(autocapitalizeAttr, value);
}

bool HTMLElement::isContentEditableForBinding() const {
  return IsEditingHost(*this) || IsEditable(*this);
}

bool HTMLElement::draggable() const {
  return DeprecatedEqualIgnoringCase(getAttribute(draggableAttr), "true");
}

void HTMLElement::setDraggable(bool value) {
  setAttribute(draggableAttr, value ? "true" : "false");
}

bool HTMLElement::spellcheck() const {
  return IsSpellCheckingEnabled();
}

void HTMLElement::setSpellcheck(bool enable) {
  setAttribute(spellcheckAttr, enable ? "true" : "false");
}

void HTMLElement::click() {
  DispatchSimulatedClick(nullptr, kSendNoEvents,
                         SimulatedClickCreationScope::kFromScript);
}

void HTMLElement::AccessKeyAction(bool send_mouse_events) {
  DispatchSimulatedClick(
      nullptr, send_mouse_events ? kSendMouseUpDownEvents : kSendNoEvents);
}

String HTMLElement::title() const {
  return FastGetAttribute(titleAttr);
}

int HTMLElement::tabIndex() const {
  if (SupportsFocus())
    return Element::tabIndex();
  return -1;
}

TranslateAttributeMode HTMLElement::GetTranslateAttributeMode() const {
  const AtomicString& value = getAttribute(translateAttr);

  if (value == g_null_atom)
    return kTranslateAttributeInherit;
  if (DeprecatedEqualIgnoringCase(value, "yes") ||
      DeprecatedEqualIgnoringCase(value, ""))
    return kTranslateAttributeYes;
  if (DeprecatedEqualIgnoringCase(value, "no"))
    return kTranslateAttributeNo;

  return kTranslateAttributeInherit;
}

bool HTMLElement::translate() const {
  for (const HTMLElement* element = this; element;
       element = Traversal<HTMLElement>::FirstAncestor(*element)) {
    TranslateAttributeMode mode = element->GetTranslateAttributeMode();
    if (mode != kTranslateAttributeInherit) {
      DCHECK(mode == kTranslateAttributeYes || mode == kTranslateAttributeNo);
      return mode == kTranslateAttributeYes;
    }
  }

  // Default on the root element is translate=yes.
  return true;
}

void HTMLElement::setTranslate(bool enable) {
  setAttribute(translateAttr, enable ? "yes" : "no");
}

// Returns the conforming 'dir' value associated with the state the attribute is
// in (in its canonical case), if any, or the empty string if the attribute is
// in a state that has no associated keyword value or if the attribute is not in
// a defined state (e.g. the attribute is missing and there is no missing value
// default).
// http://www.whatwg.org/specs/web-apps/current-work/multipage/common-dom-interfaces.html#limited-to-only-known-values
static inline const AtomicString& ToValidDirValue(const AtomicString& value) {
  DEFINE_STATIC_LOCAL(const AtomicString, ltr_value, ("ltr"));
  DEFINE_STATIC_LOCAL(const AtomicString, rtl_value, ("rtl"));
  DEFINE_STATIC_LOCAL(const AtomicString, auto_value, ("auto"));

  if (DeprecatedEqualIgnoringCase(value, ltr_value))
    return ltr_value;
  if (DeprecatedEqualIgnoringCase(value, rtl_value))
    return rtl_value;
  if (DeprecatedEqualIgnoringCase(value, auto_value))
    return auto_value;
  return g_null_atom;
}

const AtomicString& HTMLElement::dir() {
  return ToValidDirValue(FastGetAttribute(dirAttr));
}

void HTMLElement::setDir(const AtomicString& value) {
  setAttribute(dirAttr, value);
}

HTMLFormElement* HTMLElement::FindFormAncestor() const {
  return Traversal<HTMLFormElement>::FirstAncestor(*this);
}

static inline bool ElementAffectsDirectionality(const Node* node) {
  return node->IsHTMLElement() && (IsHTMLBDIElement(ToHTMLElement(*node)) ||
                                   ToHTMLElement(*node).hasAttribute(dirAttr));
}

void HTMLElement::ChildrenChanged(const ChildrenChange& change) {
  Element::ChildrenChanged(change);
  AdjustDirectionalityIfNeededAfterChildrenChanged(change);
}

bool HTMLElement::HasDirectionAuto() const {
  // <bdi> defaults to dir="auto"
  // https://html.spec.whatwg.org/multipage/semantics.html#the-bdi-element
  const AtomicString& direction = FastGetAttribute(dirAttr);
  return (IsHTMLBDIElement(*this) && direction == g_null_atom) ||
         DeprecatedEqualIgnoringCase(direction, "auto");
}

TextDirection HTMLElement::DirectionalityIfhasDirAutoAttribute(
    bool& is_auto) const {
  is_auto = HasDirectionAuto();
  if (!is_auto)
    return TextDirection::kLtr;
  return Directionality();
}

TextDirection HTMLElement::Directionality(
    Node** strong_directionality_text_node) const {
  if (auto* input_element = ToHTMLInputElementOrNull(*this)) {
    bool has_strong_directionality;
    TextDirection text_direction = DetermineDirectionality(
        input_element->value(), &has_strong_directionality);
    if (strong_directionality_text_node) {
      *strong_directionality_text_node =
          has_strong_directionality
              ? const_cast<HTMLInputElement*>(input_element)
              : nullptr;
    }
    return text_direction;
  }

  Node* node = FlatTreeTraversal::FirstChild(*this);
  while (node) {
    // Skip bdi, script, style and text form controls.
    if (DeprecatedEqualIgnoringCase(node->nodeName(), "bdi") ||
        IsHTMLScriptElement(*node) || IsHTMLStyleElement(*node) ||
        (node->IsElementNode() && ToElement(node)->IsTextControl()) ||
        (node->IsElementNode() &&
         ToElement(node)->ShadowPseudoId() == "-webkit-input-placeholder")) {
      node = FlatTreeTraversal::NextSkippingChildren(*node, this);
      continue;
    }

    // Skip elements with valid dir attribute
    if (node->IsElementNode()) {
      AtomicString dir_attribute_value =
          ToElement(node)->FastGetAttribute(dirAttr);
      if (IsValidDirAttribute(dir_attribute_value)) {
        node = FlatTreeTraversal::NextSkippingChildren(*node, this);
        continue;
      }
    }

    if (node->IsTextNode()) {
      bool has_strong_directionality;
      TextDirection text_direction = DetermineDirectionality(
          node->textContent(true), &has_strong_directionality);
      if (has_strong_directionality) {
        if (strong_directionality_text_node)
          *strong_directionality_text_node = node;
        return text_direction;
      }
    }
    node = FlatTreeTraversal::Next(*node, this);
  }
  if (strong_directionality_text_node)
    *strong_directionality_text_node = nullptr;
  return TextDirection::kLtr;
}

bool HTMLElement::SelfOrAncestorHasDirAutoAttribute() const {
  // TODO(esprehn): Storing this state in the computed style is bad, we
  // should be able to answer questions about the shape of the DOM and the
  // text contained inside it without having style.
  if (const ComputedStyle* style = GetComputedStyle())
    return style->SelfOrAncestorHasDirAutoAttribute();
  return false;
}

void HTMLElement::AdjustDirectionalityIfNeededAfterChildAttributeChanged(
    Element* child) {
  DCHECK(SelfOrAncestorHasDirAutoAttribute());
  const ComputedStyle* style = GetComputedStyle();
  if (style && style->Direction() != Directionality()) {
    for (Element* element_to_adjust = this; element_to_adjust;
         element_to_adjust =
             FlatTreeTraversal::ParentElement(*element_to_adjust)) {
      if (ElementAffectsDirectionality(element_to_adjust)) {
        element_to_adjust->SetNeedsStyleRecalc(
            kLocalStyleChange, StyleChangeReasonForTracing::Create(
                                   StyleChangeReason::kWritingModeChange));
        return;
      }
    }
  }
}

void HTMLElement::CalculateAndAdjustDirectionality() {
  TextDirection text_direction = Directionality();
  const ComputedStyle* style = GetComputedStyle();
  if (style && style->Direction() != text_direction)
    SetNeedsStyleRecalc(kLocalStyleChange,
                        StyleChangeReasonForTracing::Create(
                            StyleChangeReason::kWritingModeChange));
}

void HTMLElement::AdjustDirectionalityIfNeededAfterChildrenChanged(
    const ChildrenChange& change) {
  if (!SelfOrAncestorHasDirAutoAttribute())
    return;

  UpdateDistribution();

  for (Element* element_to_adjust = this; element_to_adjust;
       element_to_adjust =
           FlatTreeTraversal::ParentElement(*element_to_adjust)) {
    if (ElementAffectsDirectionality(element_to_adjust)) {
      ToHTMLElement(element_to_adjust)->CalculateAndAdjustDirectionality();
      return;
    }
  }
}

Node::InsertionNotificationRequest HTMLElement::InsertedInto(
    ContainerNode* insertion_point) {
  // Process the superclass first to ensure that `InActiveDocument()` is
  // updated.
  Element::InsertedInto(insertion_point);

  if (GetDocument().GetContentSecurityPolicy()->HasHeaderDeliveredPolicy() &&
      InActiveDocument() && FastHasAttribute(nonceAttr)) {
    setAttribute(nonceAttr, g_empty_atom);
  }

  return kInsertionDone;
}

void HTMLElement::AddHTMLLengthToStyle(MutableCSSPropertyValueSet* style,
                                       CSSPropertyID property_id,
                                       const String& value,
                                       AllowPercentage allow_percentage) {
  HTMLDimension dimension;
  if (!ParseDimensionValue(value, dimension))
    return;
  if (property_id == CSSPropertyWidth &&
      (dimension.IsPercentage() || dimension.IsRelative())) {
    UseCounter::Count(GetDocument(), WebFeature::kHTMLElementDeprecatedWidth);
  }
  if (dimension.IsRelative())
    return;
  if (dimension.IsPercentage() && allow_percentage != kAllowPercentageValues)
    return;
  CSSPrimitiveValue::UnitType unit =
      dimension.IsPercentage() ? CSSPrimitiveValue::UnitType::kPercentage
                               : CSSPrimitiveValue::UnitType::kPixels;
  AddPropertyToPresentationAttributeStyle(style, property_id, dimension.Value(),
                                          unit);
}

static RGBA32 ParseColorStringWithCrazyLegacyRules(const String& color_string) {
  // Per spec, only look at the first 128 digits of the string.
  const size_t kMaxColorLength = 128;
  // We'll pad the buffer with two extra 0s later, so reserve two more than the
  // max.
  Vector<char, kMaxColorLength + 2> digit_buffer;

  size_t i = 0;
  // Skip a leading #.
  if (color_string[0] == '#')
    i = 1;

  // Grab the first 128 characters, replacing non-hex characters with 0.
  // Non-BMP characters are replaced with "00" due to them appearing as two
  // "characters" in the String.
  for (; i < color_string.length() && digit_buffer.size() < kMaxColorLength;
       i++) {
    if (!IsASCIIHexDigit(color_string[i]))
      digit_buffer.push_back('0');
    else
      digit_buffer.push_back(color_string[i]);
  }

  if (!digit_buffer.size())
    return Color::kBlack;

  // Pad the buffer out to at least the next multiple of three in size.
  digit_buffer.push_back('0');
  digit_buffer.push_back('0');

  if (digit_buffer.size() < 6)
    return MakeRGB(ToASCIIHexValue(digit_buffer[0]),
                   ToASCIIHexValue(digit_buffer[1]),
                   ToASCIIHexValue(digit_buffer[2]));

  // Split the digits into three components, then search the last 8 digits of
  // each component.
  DCHECK_GE(digit_buffer.size(), 6u);
  size_t component_length = digit_buffer.size() / 3;
  size_t component_search_window_length = min<size_t>(component_length, 8);
  size_t red_index = component_length - component_search_window_length;
  size_t green_index = component_length * 2 - component_search_window_length;
  size_t blue_index = component_length * 3 - component_search_window_length;
  // Skip digits until one of them is non-zero, or we've only got two digits
  // left in the component.
  while (digit_buffer[red_index] == '0' && digit_buffer[green_index] == '0' &&
         digit_buffer[blue_index] == '0' &&
         (component_length - red_index) > 2) {
    red_index++;
    green_index++;
    blue_index++;
  }
  DCHECK_LT(red_index + 1, component_length);
  DCHECK_GE(green_index, component_length);
  DCHECK_LT(green_index + 1, component_length * 2);
  DCHECK_GE(blue_index, component_length * 2);
  SECURITY_DCHECK(blue_index + 1 < digit_buffer.size());

  int red_value =
      ToASCIIHexValue(digit_buffer[red_index], digit_buffer[red_index + 1]);
  int green_value =
      ToASCIIHexValue(digit_buffer[green_index], digit_buffer[green_index + 1]);
  int blue_value =
      ToASCIIHexValue(digit_buffer[blue_index], digit_buffer[blue_index + 1]);
  return MakeRGB(red_value, green_value, blue_value);
}

// Color parsing that matches HTML's "rules for parsing a legacy color value"
bool HTMLElement::ParseColorWithLegacyRules(const String& attribute_value,
                                            Color& parsed_color) {
  // An empty string doesn't apply a color. (One containing only whitespace
  // does, which is why this check occurs before stripping.)
  if (attribute_value.IsEmpty())
    return false;

  String color_string = attribute_value.StripWhiteSpace();

  // "transparent" doesn't apply a color either.
  if (DeprecatedEqualIgnoringCase(color_string, "transparent"))
    return false;

  // If the string is a 3/6-digit hex color or a named CSS color, use that.
  // Apply legacy rules otherwise. Note color.setFromString() accepts 4/8-digit
  // hex color, so restrict its use with length checks here to support legacy
  // HTML attributes.

  bool success = false;
  if ((color_string.length() == 4 || color_string.length() == 7) &&
      color_string[0] == '#')
    success = parsed_color.SetFromString(color_string);
  if (!success)
    success = parsed_color.SetNamedColor(color_string);
  if (!success) {
    parsed_color.SetRGB(ParseColorStringWithCrazyLegacyRules(color_string));
    success = true;
  }

  return success;
}

void HTMLElement::AddHTMLColorToStyle(MutableCSSPropertyValueSet* style,
                                      CSSPropertyID property_id,
                                      const String& attribute_value) {
  Color parsed_color;
  if (!ParseColorWithLegacyRules(attribute_value, parsed_color))
    return;

  style->SetProperty(property_id, *CSSColorValue::Create(parsed_color.Rgb()));
}

bool HTMLElement::IsInteractiveContent() const {
  return false;
}

void HTMLElement::DefaultEventHandler(Event* event) {
  if (event->type() == EventTypeNames::keypress && event->IsKeyboardEvent()) {
    HandleKeypressEvent(ToKeyboardEvent(event));
    if (event->DefaultHandled())
      return;
  }

  Element::DefaultEventHandler(event);
}

bool HTMLElement::MatchesReadOnlyPseudoClass() const {
  return !MatchesReadWritePseudoClass();
}

bool HTMLElement::MatchesReadWritePseudoClass() const {
  if (FastHasAttribute(contenteditableAttr)) {
    const AtomicString& value = FastGetAttribute(contenteditableAttr);

    if (value.IsEmpty() || DeprecatedEqualIgnoringCase(value, "true") ||
        DeprecatedEqualIgnoringCase(value, "plaintext-only"))
      return true;
    if (DeprecatedEqualIgnoringCase(value, "false"))
      return false;
    // All other values should be treated as "inherit".
  }

  return parentElement() && HasEditableStyle(*parentElement());
}

void HTMLElement::HandleKeypressEvent(KeyboardEvent* event) {
  if (!IsSpatialNavigationEnabled(GetDocument().GetFrame()) || !SupportsFocus())
    return;
  GetDocument().UpdateStyleAndLayoutTree();
  // if the element is a text form control (like <input type=text> or
  // <textarea>) or has contentEditable attribute on, we should enter a space or
  // newline even in spatial navigation mode instead of handling it as a "click"
  // action.
  if (IsTextControl() || HasEditableStyle(*this))
    return;
  int char_code = event->charCode();
  if (char_code == '\r' || char_code == ' ') {
    DispatchSimulatedClick(event);
    event->SetDefaultHandled();
  }
}

const AtomicString& HTMLElement::EventParameterName() {
  DEFINE_STATIC_LOCAL(const AtomicString, event_string, ("event"));
  return event_string;
}

int HTMLElement::offsetLeftForBinding() {
  GetDocument().EnsurePaintLocationDataValidForNode(this);
  Element* offset_parent = unclosedOffsetParent();
  if (LayoutBoxModelObject* layout_object = GetLayoutBoxModelObject())
    return AdjustForAbsoluteZoom::AdjustLayoutUnit(
               LayoutUnit(layout_object->PixelSnappedOffsetLeft(offset_parent)),
               layout_object->StyleRef())
        .Round();
  return 0;
}

int HTMLElement::offsetTopForBinding() {
  GetDocument().EnsurePaintLocationDataValidForNode(this);
  Element* offset_parent = unclosedOffsetParent();
  if (LayoutBoxModelObject* layout_object = GetLayoutBoxModelObject())
    return AdjustForAbsoluteZoom::AdjustLayoutUnit(
               LayoutUnit(layout_object->PixelSnappedOffsetTop(offset_parent)),
               layout_object->StyleRef())
        .Round();
  return 0;
}

int HTMLElement::offsetWidthForBinding() {
  GetDocument().EnsurePaintLocationDataValidForNode(this);
  Element* offset_parent = unclosedOffsetParent();
  if (LayoutBoxModelObject* layout_object = GetLayoutBoxModelObject())
    return AdjustForAbsoluteZoom::AdjustLayoutUnit(
               LayoutUnit(
                   layout_object->PixelSnappedOffsetWidth(offset_parent)),
               layout_object->StyleRef())
        .Round();
  return 0;
}

DISABLE_CFI_PERF
int HTMLElement::offsetHeightForBinding() {
  GetDocument().EnsurePaintLocationDataValidForNode(this);
  Element* offset_parent = unclosedOffsetParent();
  if (LayoutBoxModelObject* layout_object = GetLayoutBoxModelObject())
    return AdjustForAbsoluteZoom::AdjustLayoutUnit(
               LayoutUnit(
                   layout_object->PixelSnappedOffsetHeight(offset_parent)),
               layout_object->StyleRef())
        .Round();
  return 0;
}

Element* HTMLElement::unclosedOffsetParent() {
  GetDocument().UpdateStyleAndLayoutIgnorePendingStylesheetsForNode(this);

  LayoutObject* layout_object = GetLayoutObject();
  if (!layout_object)
    return nullptr;

  return layout_object->OffsetParent(this);
}

void HTMLElement::OnDirAttrChanged(const AttributeModificationParams& params) {
  // If an ancestor has dir=auto, and this node has the first character,
  // changes to dir attribute may affect the ancestor.
  if (!CanParticipateInFlatTree())
    return;
  UpdateDistribution();
  Element* parent = FlatTreeTraversal::ParentElement(*this);
  if (parent && parent->IsHTMLElement() &&
      ToHTMLElement(parent)->SelfOrAncestorHasDirAutoAttribute()) {
    ToHTMLElement(parent)
        ->AdjustDirectionalityIfNeededAfterChildAttributeChanged(this);
  }

  if (DeprecatedEqualIgnoringCase(params.new_value, "auto"))
    CalculateAndAdjustDirectionality();
}

void HTMLElement::OnInertAttrChanged(
    const AttributeModificationParams& params) {
  UpdateDistribution();
  if (GetDocument().GetFrame()) {
    GetDocument().GetFrame()->SetIsInert(GetDocument().LocalOwner() &&
                                         GetDocument().LocalOwner()->IsInert());
  }
}

void HTMLElement::OnLangAttrChanged(const AttributeModificationParams& params) {
  PseudoStateChanged(CSSSelector::kPseudoLang);
}

void HTMLElement::OnNonceAttrChanged(
    const AttributeModificationParams& params) {
  if (params.new_value != g_empty_atom)
    setNonce(params.new_value);
}

void HTMLElement::OnTabIndexAttrChanged(
    const AttributeModificationParams& params) {
  Element::ParseAttribute(params);
}

void HTMLElement::OnXMLLangAttrChanged(
    const AttributeModificationParams& params) {
  Element::ParseAttribute(params);
}

}  // namespace blink

#ifndef NDEBUG

// For use in the debugger
void dumpInnerHTML(blink::HTMLElement*);

void dumpInnerHTML(blink::HTMLElement* element) {
  printf("%s\n", element->InnerHTMLAsString().Ascii().data());
}
#endif
