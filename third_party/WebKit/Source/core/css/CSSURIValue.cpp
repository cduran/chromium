// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/css/CSSURIValue.h"

#include "core/css/CSSMarkup.h"
#include "core/dom/Document.h"
#include "core/svg/SVGResource.h"
#include "platform/weborigin/KURL.h"

namespace blink {

CSSURIValue::CSSURIValue(const AtomicString& relative_url,
                         const AtomicString& absolute_url)
    : CSSValue(kURIClass),
      relative_url_(relative_url),
      is_local_(relative_url.StartsWith('#')),
      absolute_url_(absolute_url) {}

CSSURIValue::CSSURIValue(const AtomicString& relative_url, const KURL& url)
    : CSSURIValue(relative_url, AtomicString(url.GetString())) {}

CSSURIValue::~CSSURIValue() = default;

SVGResource* CSSURIValue::EnsureResourceReference() const {
  if (!resource_)
    resource_ = new ExternalSVGResource(AbsoluteUrl());
  return resource_;
}

void CSSURIValue::ReResolveUrl(const Document& document) const {
  if (is_local_)
    return;
  KURL url = document.CompleteURL(relative_url_);
  AtomicString url_string(url.GetString());
  if (url_string == absolute_url_)
    return;
  absolute_url_ = url_string;
  resource_ = nullptr;
}

String CSSURIValue::CustomCSSText() const {
  return SerializeURI(relative_url_);
}

AtomicString CSSURIValue::FragmentIdentifier() const {
  if (is_local_)
    return AtomicString(relative_url_.GetString().Substring(1));
  return AtomicString(AbsoluteUrl().FragmentIdentifier());
}

KURL CSSURIValue::AbsoluteUrl() const {
  return KURL(absolute_url_);
}

bool CSSURIValue::IsLocal(const Document& document) const {
  return is_local_ ||
         EqualIgnoringFragmentIdentifier(AbsoluteUrl(), document.Url());
}

bool CSSURIValue::Equals(const CSSURIValue& other) const {
  // If only one has the 'local url' flag set, the URLs can't match.
  if (is_local_ != other.is_local_)
    return false;
  if (is_local_)
    return relative_url_ == other.relative_url_;
  return absolute_url_ == other.absolute_url_;
}

void CSSURIValue::TraceAfterDispatch(blink::Visitor* visitor) {
  visitor->Trace(resource_);
  CSSValue::TraceAfterDispatch(visitor);
}

}  // namespace blink
