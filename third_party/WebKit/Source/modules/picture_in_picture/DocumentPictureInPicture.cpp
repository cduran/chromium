// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "modules/picture_in_picture/DocumentPictureInPicture.h"

#include "core/dom/DOMException.h"
#include "core/dom/Document.h"
#include "core/dom/Element.h"
#include "core/dom/events/Event.h"
#include "modules/picture_in_picture/PictureInPictureControllerImpl.h"

namespace blink {

namespace {

const char kNoPictureInPictureElement[] =
    "There is no Picture-in-Picture element in this document.";

}  // namespace

// static
bool DocumentPictureInPicture::pictureInPictureEnabled(Document& document) {
  return PictureInPictureControllerImpl::From(document)
      .PictureInPictureEnabled();
}

// static
ScriptPromise DocumentPictureInPicture::exitPictureInPicture(
    ScriptState* script_state,
    Document& document) {
  PictureInPictureControllerImpl& controller =
      PictureInPictureControllerImpl::From(document);
  Element* picture_in_picture_element =
      controller.PictureInPictureElement(ToTreeScope(document));

  if (!picture_in_picture_element) {
    return ScriptPromise::RejectWithDOMException(
        script_state,
        DOMException::Create(kInvalidStateError, kNoPictureInPictureElement));
  }

  // TODO(crbug.com/806249): Call element.exitPictureInPicture().

  controller.OnClosePictureInPictureWindow();

  controller.UnsetPictureInPictureElement();

  picture_in_picture_element->DispatchEvent(
      Event::CreateBubble(EventTypeNames::leavepictureinpicture));

  return ScriptPromise::CastUndefined(script_state);
}

// static
Element* DocumentPictureInPicture::pictureInPictureElement(TreeScope& scope) {
  return PictureInPictureControllerImpl::From(scope.GetDocument())
      .PictureInPictureElement(scope);
}

}  // namespace blink
