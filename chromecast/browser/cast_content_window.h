// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMECAST_BROWSER_CAST_CONTENT_WINDOW_H_
#define CHROMECAST_BROWSER_CAST_CONTENT_WINDOW_H_

#include <memory>

#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "content/public/browser/web_contents.h"
#include "ui/events/event.h"

namespace content {
class WebContents;
}

namespace chromecast {
class CastWindowManager;

namespace shell {

enum class VisibilityType {
  UNKNOWN = 0,
  FULL_SCREEN = 1,
  PARTIAL_OUT = 2,
  HIDDEN = 3
};

enum class VisibilityPriority {
  // Default priority, up to system to decide how to show the app.
  DEFAULT = 0,

  // Priority for app need to show in full screen mode but could be timout.
  TRANSIENT_ACTIVITY = 1,

  // A high priority interruption takes half screen if a sticky activity
  // showing on screen, otherwise takes full screen.
  HIGH_PRIORITY_INTERRUPTION = 2,

  // Priority for app need to show in full screen mode and stick to screen.
  STICKY_ACTIVITY = 3,
};

enum class GestureType { NO_GESTURE = 0, GO_BACK = 1 };

// Class that represents the "window" a WebContents is displayed in cast_shell.
// For Linux, this represents an Aura window. For Android, this is a Activity.
// See CastContentWindowLinux and CastContentWindowAndroid.
class CastContentWindow {
 public:
  class Delegate {
   public:
    virtual void OnWindowDestroyed() = 0;
    virtual void OnKeyEvent(const ui::KeyEvent& key_event) = 0;

    // To be called from Android side through JNI to send surface gesture to
    // cast activity or appliction.
    virtual bool ConsumeGesture(GestureType gesture_type) = 0;

    // To be called from Android side through JNI to notify cast activity or
    // appliction its visibility change in Android app hosting it.
    virtual void OnVisibilityChange(VisibilityType visibility_type) = 0;

    // Returns app ID of cast activity or appliction.
    virtual std::string GetId() = 0;

   protected:
    virtual ~Delegate() {}
  };

  // Creates the platform specific CastContentWindow. |delegate| should outlive
  // the created CastContentWindow.
  static std::unique_ptr<CastContentWindow> Create(
      CastContentWindow::Delegate* delegate,
      bool is_headless,
      bool enable_touch_input);

  virtual ~CastContentWindow() {}

  // Creates a full-screen window for |web_contents| and displays it if
  // |is_visible| is true.
  // |web_contents| should outlive this CastContentWindow.
  // |window_manager| should outlive this CastContentWindow.
  virtual void CreateWindowForWebContents(
      content::WebContents* web_contents,
      CastWindowManager* window_manager,
      bool is_visible,
      VisibilityPriority visibility_priority) = 0;

  // Enables touch input to be routed to the window's WebContents.
  virtual void EnableTouchInput(bool enabled) = 0;

  // Cast activity or application calls it to request for a visibility priority
  // change.
  virtual void RequestVisibility(VisibilityPriority visibility_priority) = 0;

  // Cast activity or application calls it to request for moving out of the
  // screen.
  virtual void RequestMoveOut() = 0;
};

}  // namespace shell
}  // namespace chromecast

#endif  // CHROMECAST_BROWSER_CAST_CONTENT_WINDOW_H_
