// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "modules/storage/DOMWindowStorageController.h"

#include "core/dom/Document.h"
#include "core/dom/events/Event.h"
#include "core/page/Page.h"
#include "modules/storage/DOMWindowStorage.h"

namespace blink {

DOMWindowStorageController::DOMWindowStorageController(Document& document)
    : Supplement<Document>(document) {
  document.domWindow()->RegisterEventListenerObserver(this);
}

void DOMWindowStorageController::Trace(blink::Visitor* visitor) {
  Supplement<Document>::Trace(visitor);
}

// static
const char DOMWindowStorageController::kSupplementName[] =
    "DOMWindowStorageController";

// static
DOMWindowStorageController& DOMWindowStorageController::From(
    Document& document) {
  DOMWindowStorageController* controller =
      Supplement<Document>::From<DOMWindowStorageController>(document);
  if (!controller) {
    controller = new DOMWindowStorageController(document);
    ProvideTo(document, controller);
  }
  return *controller;
}

void DOMWindowStorageController::DidAddEventListener(
    LocalDOMWindow* window,
    const AtomicString& event_type) {
  if (event_type == EventTypeNames::storage) {
    // Creating these blink::Storage objects informs the system that we'd like
    // to receive notifications about storage events that might be triggered in
    // other processes. Rather than subscribe to these notifications explicitly,
    // we subscribe to them implicitly to simplify the work done by the system.
    DOMWindowStorage::From(*window).localStorage(IGNORE_EXCEPTION_FOR_TESTING);
    DOMWindowStorage::From(*window).sessionStorage(
        IGNORE_EXCEPTION_FOR_TESTING);
  }
}

}  // namespace blink
