// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/events/blink/web_input_event.h"

#include "ui/events/base_event_utils.h"
#include "ui/events/blink/blink_event_util.h"
#include "ui/events/blink/blink_features.h"
#include "ui/events/event.h"
#include "ui/events/event_utils.h"
#include "ui/events/keycodes/dom/keycode_converter.h"
#include "ui/events/keycodes/keyboard_code_conversion.h"

#if defined(OS_WIN)
#include "ui/events/blink/web_input_event_builders_win.h"
#endif

#if defined(USE_X11)
#include "ui/gfx/x/x11.h"
#endif

namespace ui {

namespace {

gfx::PointF GetScreenLocationFromEvent(
    const LocatedEvent& event,
    const base::Callback<gfx::PointF(const LocatedEvent& event)>&
        screen_location_callback) {
  DCHECK(!screen_location_callback.is_null());
  return event.target() ? screen_location_callback.Run(event)
                        : event.root_location_f();
}

blink::WebPointerProperties::PointerType EventPointerTypeToWebPointerType(
    EventPointerType pointer_type) {
  switch (pointer_type) {
    case EventPointerType::POINTER_TYPE_UNKNOWN:
      return blink::WebPointerProperties::PointerType::kUnknown;
    case EventPointerType::POINTER_TYPE_MOUSE:
      return blink::WebPointerProperties::PointerType::kMouse;
    case EventPointerType::POINTER_TYPE_PEN:
      return blink::WebPointerProperties::PointerType::kPen;
    case EventPointerType::POINTER_TYPE_ERASER:
      return blink::WebPointerProperties::PointerType::kEraser;
    case EventPointerType::POINTER_TYPE_TOUCH:
      return blink::WebPointerProperties::PointerType::kTouch;
  }
  NOTREACHED() << "Unexpected EventPointerType";
  return blink::WebPointerProperties::PointerType::kUnknown;
}

// Creates a WebGestureEvent from a GestureEvent. Note that it does not
// populate the event coordinates (i.e. |x|, |y|, |globalX|, and |globalY|). So
// the caller must populate these fields.
blink::WebGestureEvent MakeWebGestureEventFromUIEvent(
    const GestureEvent& event) {
  return CreateWebGestureEvent(event.details(), event.time_stamp(),
                               event.location_f(), event.root_location_f(),
                               event.flags(), event.unique_touch_event_id());
}

}  // namespace

#if defined(OS_WIN)
// On Windows, we can just use the builtin WebKit factory methods to fully
// construct our pre-translated events.

blink::WebMouseEvent MakeUntranslatedWebMouseEventFromNativeEvent(
    const base::NativeEvent& native_event,
    const base::TimeTicks& time_stamp,
    blink::WebPointerProperties::PointerType pointer_type) {
  return WebMouseEventBuilder::Build(
      native_event.hwnd, native_event.message, native_event.wParam,
      native_event.lParam, EventTimeStampToSeconds(time_stamp), pointer_type);
}

blink::WebMouseWheelEvent MakeUntranslatedWebMouseWheelEventFromNativeEvent(
    const base::NativeEvent& native_event,
    const base::TimeTicks& time_stamp,
    blink::WebPointerProperties::PointerType pointer_type) {
  return WebMouseWheelEventBuilder::Build(
      native_event.hwnd, native_event.message, native_event.wParam,
      native_event.lParam, EventTimeStampToSeconds(time_stamp), pointer_type);
}
#endif  // defined(OS_WIN)

blink::WebKeyboardEvent MakeWebKeyboardEventFromUiEvent(const KeyEvent& event) {
  blink::WebInputEvent::Type type = blink::WebInputEvent::kUndefined;
  switch (event.type()) {
    case ET_KEY_PRESSED:
      type = event.is_char() ? blink::WebInputEvent::kChar
                             : blink::WebInputEvent::kRawKeyDown;
      break;
    case ET_KEY_RELEASED:
      type = blink::WebInputEvent::kKeyUp;
      break;
    default:
      NOTREACHED();
  }

  blink::WebKeyboardEvent webkit_event(
      type, EventFlagsToWebEventModifiers(event.flags()) |
                DomCodeToWebInputEventModifiers(event.code()),
      EventTimeStampToSeconds(event.time_stamp()));

  if (webkit_event.GetModifiers() & blink::WebInputEvent::kAltKey)
    webkit_event.is_system_key = true;

  // TODO(dtapuska): crbug.com/570388. Ozone appears to deliver
  // key_code events that aren't "located" for the keypad like
  // Windows and X11 do and blink expects.
  webkit_event.windows_key_code =
      NonLocatedToLocatedKeypadKeyboardCode(event.key_code(), event.code());
  webkit_event.native_key_code =
      KeycodeConverter::DomCodeToNativeKeycode(event.code());
  webkit_event.dom_code = static_cast<int>(event.code());
  webkit_event.dom_key = static_cast<int>(event.GetDomKey());
  webkit_event.unmodified_text[0] = event.GetUnmodifiedText();
  webkit_event.text[0] = event.GetText();

  return webkit_event;
}

blink::WebMouseWheelEvent MakeWebMouseWheelEventFromUiEvent(
    const ScrollEvent& event) {
  blink::WebMouseWheelEvent webkit_event(
      blink::WebInputEvent::kMouseWheel,
      EventFlagsToWebEventModifiers(event.flags()),
      EventTimeStampToSeconds(event.time_stamp()));

  webkit_event.button = blink::WebMouseEvent::Button::kNoButton;
  webkit_event.has_precise_scrolling_deltas = true;

  float offset_ordinal_x = event.x_offset_ordinal();
  float offset_ordinal_y = event.y_offset_ordinal();
  webkit_event.delta_x = event.x_offset();
  webkit_event.delta_y = event.y_offset();

  if (offset_ordinal_x != 0.f && webkit_event.delta_x != 0.f)
    webkit_event.acceleration_ratio_x = offset_ordinal_x / webkit_event.delta_x;
  webkit_event.wheel_ticks_x =
      webkit_event.delta_x / MouseWheelEvent::kWheelDelta;
  webkit_event.wheel_ticks_y =
      webkit_event.delta_y / MouseWheelEvent::kWheelDelta;
  if (offset_ordinal_y != 0.f && webkit_event.delta_y != 0.f)
    webkit_event.acceleration_ratio_y = offset_ordinal_y / webkit_event.delta_y;

  webkit_event.pointer_type =
      EventPointerTypeToWebPointerType(event.pointer_details().pointer_type);
  return webkit_event;
}

blink::WebGestureEvent MakeWebGestureEventFromUiEvent(
    const ScrollEvent& event) {
  blink::WebInputEvent::Type type = blink::WebInputEvent::kUndefined;
  switch (event.type()) {
    case ET_SCROLL_FLING_START:
      type = blink::WebInputEvent::kGestureFlingStart;
      break;
    case ET_SCROLL_FLING_CANCEL:
      type = blink::WebInputEvent::kGestureFlingCancel;
      break;
    case ET_SCROLL:
      NOTREACHED() << "Invalid gesture type: " << event.type();
      break;
    default:
      NOTREACHED() << "Unknown gesture type: " << event.type();
  }

  blink::WebGestureEvent webkit_event(
      type, EventFlagsToWebEventModifiers(event.flags()),
      EventTimeStampToSeconds(event.time_stamp()));
  webkit_event.source_device = blink::kWebGestureDeviceTouchpad;
  if (event.type() == ET_SCROLL_FLING_START) {
    webkit_event.data.fling_start.velocity_x = event.x_offset();
    webkit_event.data.fling_start.velocity_y = event.y_offset();
  }

  return webkit_event;
}

blink::WebMouseEvent MakeWebMouseEventFromUiEvent(const MouseEvent& event);
blink::WebMouseWheelEvent MakeWebMouseWheelEventFromUiEvent(
    const MouseWheelEvent& event);

// General approach:
//
// Event only carries a subset of possible event data provided to UI by the host
// platform. WebKit utilizes a larger subset of that information, and includes
// some built in cracking functionality that we rely on to obtain this
// information cleanly and consistently.
//
// The only place where an Event's data differs from what the underlying
// base::NativeEvent would provide is position data. We would like to provide
// coordinates relative to its hosting window, rather than the top level
// platform window. To do this a callback is accepted to allow for clients to
// map the coordinates.
//
// The approach is to fully construct a blink::WebInputEvent from the
// Event's base::NativeEvent, and then replace the coordinate fields with
// the translated values from the Event.
//
// The exception is mouse events on linux. The MouseEvent contains enough
// necessary information to construct a WebMouseEvent. So instead of extracting
// the information from the XEvent, which can be tricky when supporting both
// XInput2 and XInput, the WebMouseEvent is constructed from the
// MouseEvent. This will not be necessary once only XInput2 is supported.
//

blink::WebMouseEvent MakeWebMouseEvent(
    const MouseEvent& event,
    const base::Callback<gfx::PointF(const LocatedEvent& event)>&
        screen_location_callback) {
  // Construct an untranslated event from the platform event data.
  blink::WebMouseEvent webkit_event =
#if defined(OS_WIN)
      // On Windows we have WM_ events comming from desktop and pure Events
      // comming from metro mode.
      event.native_event().message && (event.type() != ET_MOUSE_EXITED)
          ? MakeUntranslatedWebMouseEventFromNativeEvent(
                event.native_event(), event.time_stamp(),
                EventPointerTypeToWebPointerType(
                    event.pointer_details().pointer_type))
          : MakeWebMouseEventFromUiEvent(event);
#else
      MakeWebMouseEventFromUiEvent(event);
#endif
  // Replace the event's coordinate fields with translated position data from
  // |event|.
  webkit_event.SetPositionInWidget(event.x(), event.y());

#if defined(OS_WIN)
  if (event.native_event().message)
    return webkit_event;
#endif

  const gfx::PointF screen_point =
      GetScreenLocationFromEvent(event, screen_location_callback);
  webkit_event.SetPositionInScreen(screen_point.x(), screen_point.y());

  return webkit_event;
}

blink::WebMouseWheelEvent MakeWebMouseWheelEvent(
    const MouseWheelEvent& event,
    const base::Callback<gfx::PointF(const LocatedEvent& event)>&
        screen_location_callback) {
#if defined(OS_WIN)
  // Construct an untranslated event from the platform event data.
  blink::WebMouseWheelEvent webkit_event =
      event.native_event().message
          ? MakeUntranslatedWebMouseWheelEventFromNativeEvent(
                event.native_event(), event.time_stamp(),
                EventPointerTypeToWebPointerType(
                    event.pointer_details().pointer_type))
          : MakeWebMouseWheelEventFromUiEvent(event);
#else
  blink::WebMouseWheelEvent webkit_event =
      MakeWebMouseWheelEventFromUiEvent(event);
#endif

  // Replace the event's coordinate fields with translated position data from
  // |event|.
  webkit_event.SetPositionInWidget(event.x(), event.y());

  const gfx::PointF screen_point =
      GetScreenLocationFromEvent(event, screen_location_callback);
  webkit_event.SetPositionInScreen(screen_point.x(), screen_point.y());

  return webkit_event;
}

blink::WebMouseWheelEvent MakeWebMouseWheelEvent(
    const ScrollEvent& event,
    const base::Callback<gfx::PointF(const LocatedEvent& event)>&
        screen_location_callback) {
#if defined(OS_WIN)
  // Construct an untranslated event from the platform event data.
  blink::WebMouseWheelEvent webkit_event =
      event.native_event().message
          ? MakeUntranslatedWebMouseWheelEventFromNativeEvent(
                event.native_event(), event.time_stamp(),
                EventPointerTypeToWebPointerType(
                    event.pointer_details().pointer_type))
          : MakeWebMouseWheelEventFromUiEvent(event);
#else
  blink::WebMouseWheelEvent webkit_event =
      MakeWebMouseWheelEventFromUiEvent(event);
#endif

  // Replace the event's coordinate fields with translated position data from
  // |event|.
  webkit_event.SetPositionInWidget(event.x(), event.y());

  const gfx::PointF screen_point =
      GetScreenLocationFromEvent(event, screen_location_callback);
  webkit_event.SetPositionInScreen(screen_point.x(), screen_point.y());

  return webkit_event;
}

blink::WebKeyboardEvent MakeWebKeyboardEvent(const KeyEvent& event) {
  // TODO(wez): Work out how this comment relates to the code below.
  // Windows can figure out whether or not to construct a RawKeyDown or a Char
  // WebInputEvent based on the type of message carried in
  // event.native_event(). X11 is not so fortunate, there is no separate
  // translated event type, so DesktopHostLinux sends an extra KeyEvent with
  // is_char() == true. We need to pass the KeyEvent to the X11 function
  // to detect this case so the right event type can be constructed.
  blink::WebKeyboardEvent webkit_event = MakeWebKeyboardEventFromUiEvent(event);
#if defined(OS_WIN)
  if (event.HasNativeEvent()) {
    const base::NativeEvent& native_event = event.native_event();

    // System key events are explicitly distinguished, under Windows.
    webkit_event.is_system_key = native_event.message == WM_SYSCHAR ||
                                 native_event.message == WM_SYSKEYDOWN ||
                                 native_event.message == WM_SYSKEYUP;

    // Copy the OEM scancode, including flag bits, directly from the event.
    webkit_event.native_key_code = static_cast<int>(native_event.lParam);
  }
#endif
  return webkit_event;
}

blink::WebGestureEvent MakeWebGestureEvent(
    const GestureEvent& event,
    const base::Callback<gfx::PointF(const LocatedEvent& event)>&
        screen_location_callback) {
  blink::WebGestureEvent gesture_event = MakeWebGestureEventFromUIEvent(event);

  gesture_event.x = event.x();
  gesture_event.y = event.y();

  const gfx::PointF screen_point =
      GetScreenLocationFromEvent(event, screen_location_callback);
  gesture_event.global_x = screen_point.x();
  gesture_event.global_y = screen_point.y();

  return gesture_event;
}

blink::WebGestureEvent MakeWebGestureEvent(
    const ScrollEvent& event,
    const base::Callback<gfx::PointF(const LocatedEvent& event)>&
        screen_location_callback) {
  blink::WebGestureEvent gesture_event = MakeWebGestureEventFromUiEvent(event);
  gesture_event.x = event.x();
  gesture_event.y = event.y();

  const gfx::PointF screen_point =
      GetScreenLocationFromEvent(event, screen_location_callback);
  gesture_event.global_x = screen_point.x();
  gesture_event.global_y = screen_point.y();

  return gesture_event;
}

blink::WebGestureEvent MakeWebGestureEventFlingCancel() {
  blink::WebGestureEvent gesture_event(
      blink::WebInputEvent::kGestureFlingCancel,
      blink::WebInputEvent::kNoModifiers,
      EventTimeStampToSeconds(EventTimeForNow()));

  // All other fields are ignored on a GestureFlingCancel event.
  gesture_event.source_device = blink::kWebGestureDeviceTouchpad;
  return gesture_event;
}

blink::WebMouseEvent MakeWebMouseEventFromUiEvent(const MouseEvent& event) {
  blink::WebInputEvent::Type type = blink::WebInputEvent::kUndefined;
  int click_count = 0;
  switch (event.type()) {
    case ET_MOUSE_PRESSED:
      type = blink::WebInputEvent::kMouseDown;
      click_count = event.GetClickCount();
      break;
    case ET_MOUSE_RELEASED:
      type = blink::WebInputEvent::kMouseUp;
      click_count = event.GetClickCount();
      break;
    case ET_MOUSE_EXITED: {
#if defined(USE_X11)
      // NotifyVirtual events are created for intermediate windows that the
      // pointer crosses through. These occur when middle clicking.
      // Change these into mouse move events.
      const base::NativeEvent& native_event = event.native_event();

      if (native_event && native_event->type == LeaveNotify &&
          native_event->xcrossing.detail == NotifyVirtual) {
        type = blink::WebInputEvent::kMouseMove;
        break;
      }
#endif
      static bool s_send_leave =
          base::FeatureList::IsEnabled(features::kSendMouseLeaveEvents);
      type = s_send_leave ? blink::WebInputEvent::kMouseLeave
                          : blink::WebInputEvent::kMouseMove;
      break;
    }
    case ET_MOUSE_ENTERED:
    case ET_MOUSE_MOVED:
    case ET_MOUSE_DRAGGED:
      type = blink::WebInputEvent::kMouseMove;
      break;
    default:
      NOTIMPLEMENTED() << "Received unexpected event: " << event.type();
      break;
  }

  blink::WebMouseEvent webkit_event(
      type, EventFlagsToWebEventModifiers(event.flags()),
      EventTimeStampToSeconds(event.time_stamp()), event.pointer_details().id);
  webkit_event.button = blink::WebMouseEvent::Button::kNoButton;
  int button_flags = event.flags();
  if (event.type() == ET_MOUSE_PRESSED || event.type() == ET_MOUSE_RELEASED) {
    // We want to use changed_button_flags() for mouse pressed & released.
    // These flags can be used only if they are set which is not always the case
    // (see e.g. GetChangedMouseButtonFlagsFromNative() in events_win.cc).
    if (event.changed_button_flags())
      button_flags = event.changed_button_flags();
  }

  // TODO(mustaq): This |if| ordering look suspicious. Replacing with if-else &
  // changing the order to L/R/M/B/F breaks
  // pointerevent_pointermove_on_chorded_mouse_button-manual.html! Investigate.
  if (button_flags & EF_BACK_MOUSE_BUTTON)
    webkit_event.button = blink::WebMouseEvent::Button::kBack;
  if (button_flags & EF_FORWARD_MOUSE_BUTTON)
    webkit_event.button = blink::WebMouseEvent::Button::kForward;
  if (button_flags & EF_LEFT_MOUSE_BUTTON)
    webkit_event.button = blink::WebMouseEvent::Button::kLeft;
  if (button_flags & EF_MIDDLE_MOUSE_BUTTON)
    webkit_event.button = blink::WebMouseEvent::Button::kMiddle;
  if (button_flags & EF_RIGHT_MOUSE_BUTTON)
    webkit_event.button = blink::WebMouseEvent::Button::kRight;

  webkit_event.click_count = click_count;
  webkit_event.tilt_x = roundf(event.pointer_details().tilt_x);
  webkit_event.tilt_y = roundf(event.pointer_details().tilt_y);
  webkit_event.force = event.pointer_details().force;
  webkit_event.tangential_pressure =
      event.pointer_details().tangential_pressure;
  webkit_event.twist = event.pointer_details().twist;
  webkit_event.id = event.pointer_details().id;
  webkit_event.pointer_type =
      EventPointerTypeToWebPointerType(event.pointer_details().pointer_type);

  return webkit_event;
}

blink::WebMouseWheelEvent MakeWebMouseWheelEventFromUiEvent(
    const MouseWheelEvent& event) {
  blink::WebMouseWheelEvent webkit_event(
      blink::WebInputEvent::kMouseWheel,
      EventFlagsToWebEventModifiers(event.flags()),
      EventTimeStampToSeconds(event.time_stamp()));

  webkit_event.button = blink::WebMouseEvent::Button::kNoButton;

  webkit_event.delta_x = event.x_offset();
  webkit_event.delta_y = event.y_offset();

  if (event.flags() & ui::EF_PRECISION_SCROLLING_DELTA)
    webkit_event.has_precise_scrolling_deltas = true;

  webkit_event.wheel_ticks_x =
      webkit_event.delta_x / MouseWheelEvent::kWheelDelta;
  webkit_event.wheel_ticks_y =
      webkit_event.delta_y / MouseWheelEvent::kWheelDelta;

  webkit_event.tilt_x = roundf(event.pointer_details().tilt_x);
  webkit_event.tilt_y = roundf(event.pointer_details().tilt_y);
  webkit_event.force = event.pointer_details().force;
  webkit_event.pointer_type =
      EventPointerTypeToWebPointerType(event.pointer_details().pointer_type);

  return webkit_event;
}

}  // namespace ui
