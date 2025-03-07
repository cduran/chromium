// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_ACCESSIBILITY_ACCESSIBILITY_EVENT_RECORDER_H_
#define CONTENT_BROWSER_ACCESSIBILITY_ACCESSIBILITY_EVENT_RECORDER_H_

#include <string>
#include <vector>

#include "base/callback.h"
#include "base/macros.h"
#include "base/process/process_handle.h"
#include "content/common/content_export.h"

namespace content {

using AccessibilityEventCallback = base::RepeatingCallback<void(std::string)>;

class BrowserAccessibilityManager;

// Listens for native accessibility events fired by a given
// BrowserAccessibilityManager and saves human-readable log strings for
// each event fired to a vector. Construct an instance of this class to
// begin listening, call GetEventLogs() to get all of the logs so far, and
// destroy it to stop listening.
//
// A log string should be of the form "<event> on <node>" where <event> is
// the name of the event fired (platform-specific) and <node> is information
// about the accessibility node on which the event was fired, for example its
// role and name.
//
// The implementation is highly platform-specific; a subclass is needed for
// each platform does most of the work.
class AccessibilityEventRecorder {
 public:
  // Construct the right platform-specific subclass.
  static AccessibilityEventRecorder* Create(
      BrowserAccessibilityManager* manager,
      base::ProcessId pid);
  virtual ~AccessibilityEventRecorder();

  void set_only_web_events(bool only_web_events) {
    only_web_events_ = only_web_events;
  }

  void ListenToEvents(AccessibilityEventCallback callback) {
    callback_ = callback;
  }

  // Access the vector of human-readable event logs, one string per event.
  const std::vector<std::string>& event_logs() { return event_logs_; }

 protected:
  explicit AccessibilityEventRecorder(BrowserAccessibilityManager* manager,
                                      base::ProcessId pid);

  void OnEvent(std::string event);

  BrowserAccessibilityManager* manager_;
  bool only_web_events_ = false;

 private:
  std::vector<std::string> event_logs_;
  AccessibilityEventCallback callback_;

  DISALLOW_COPY_AND_ASSIGN(AccessibilityEventRecorder);
};

}  // namespace content

#endif  // CONTENT_BROWSER_ACCESSIBILITY_ACCESSIBILITY_EVENT_RECORDER_H_
