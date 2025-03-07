// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/shell/test_runner/test_plugin.h"

#include <stddef.h>
#include <stdint.h>

#include <utility>

#include "base/bind.h"
#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/memory/shared_memory.h"
#include "base/strings/stringprintf.h"
#include "cc/blink/web_layer_impl.h"
#include "cc/layers/texture_layer.h"
#include "components/viz/common/resources/shared_bitmap_manager.h"
#include "content/shell/test_runner/web_test_delegate.h"
#include "gpu/command_buffer/client/gles2_interface.h"
#include "third_party/WebKit/public/platform/Platform.h"
#include "third_party/WebKit/public/platform/WebCoalescedInputEvent.h"
#include "third_party/WebKit/public/platform/WebCompositorSupport.h"
#include "third_party/WebKit/public/platform/WebGestureEvent.h"
#include "third_party/WebKit/public/platform/WebGraphicsContext3DProvider.h"
#include "third_party/WebKit/public/platform/WebInputEvent.h"
#include "third_party/WebKit/public/platform/WebMouseEvent.h"
#include "third_party/WebKit/public/platform/WebThread.h"
#include "third_party/WebKit/public/platform/WebTouchEvent.h"
#include "third_party/WebKit/public/platform/WebTouchPoint.h"
#include "third_party/WebKit/public/platform/WebURL.h"
#include "third_party/WebKit/public/web/WebKit.h"
#include "third_party/WebKit/public/web/WebPluginParams.h"
#include "third_party/WebKit/public/web/WebUserGestureIndicator.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkPaint.h"
#include "third_party/skia/include/core/SkPath.h"

namespace test_runner {

namespace {

void PremultiplyAlpha(const uint8_t color_in[3],
                      float alpha,
                      float color_out[4]) {
  for (int i = 0; i < 3; ++i)
    color_out[i] = (color_in[i] / 255.0f) * alpha;

  color_out[3] = alpha;
}

const char* PointState(blink::WebTouchPoint::State state) {
  switch (state) {
    case blink::WebTouchPoint::kStateReleased:
      return "Released";
    case blink::WebTouchPoint::kStatePressed:
      return "Pressed";
    case blink::WebTouchPoint::kStateMoved:
      return "Moved";
    case blink::WebTouchPoint::kStateCancelled:
      return "Cancelled";
    default:
      return "Unknown";
  }
}

void PrintTouchList(WebTestDelegate* delegate,
                    const blink::WebTouchPoint* points,
                    int length) {
  for (int i = 0; i < length; ++i) {
    delegate->PrintMessage(base::StringPrintf(
        "* %.2f, %.2f: %s\n", points[i].PositionInWidget().x,
        points[i].PositionInWidget().y, PointState(points[i].state)));
  }
}

void PrintEventDetails(WebTestDelegate* delegate,
                       const blink::WebInputEvent& event) {
  if (blink::WebInputEvent::IsTouchEventType(event.GetType())) {
    const blink::WebTouchEvent& touch =
        static_cast<const blink::WebTouchEvent&>(event);
    PrintTouchList(delegate, touch.touches, touch.touches_length);
  } else if (blink::WebInputEvent::IsMouseEventType(event.GetType()) ||
             event.GetType() == blink::WebInputEvent::kMouseWheel) {
    const blink::WebMouseEvent& mouse =
        static_cast<const blink::WebMouseEvent&>(event);
    delegate->PrintMessage(base::StringPrintf("* %.2f, %.2f\n",
                                              mouse.PositionInWidget().x,
                                              mouse.PositionInWidget().y));
  } else if (blink::WebInputEvent::IsGestureEventType(event.GetType())) {
    const blink::WebGestureEvent& gesture =
        static_cast<const blink::WebGestureEvent&>(event);
    delegate->PrintMessage(
        base::StringPrintf("* %d, %d\n", gesture.x, gesture.y));
  }
}

blink::WebPluginContainer::TouchEventRequestType ParseTouchEventRequestType(
    const blink::WebString& string) {
  if (string == blink::WebString::FromUTF8("raw"))
    return blink::WebPluginContainer::kTouchEventRequestTypeRaw;
  if (string == blink::WebString::FromUTF8("raw-lowlatency"))
    return blink::WebPluginContainer::kTouchEventRequestTypeRawLowLatency;
  if (string == blink::WebString::FromUTF8("synthetic"))
    return blink::WebPluginContainer::kTouchEventRequestTypeSynthesizedMouse;
  return blink::WebPluginContainer::kTouchEventRequestTypeNone;
}

}  // namespace

TestPlugin::TestPlugin(const blink::WebPluginParams& params,
                       WebTestDelegate* delegate,
                       blink::WebLocalFrame* frame)
    : delegate_(delegate),
      container_(nullptr),
      web_local_frame_(frame),
      gl_(nullptr),
      color_texture_(0),
      content_changed_(false),
      framebuffer_(0),
      touch_event_request_(
          blink::WebPluginContainer::kTouchEventRequestTypeNone),
      re_request_touch_events_(false),
      print_event_details_(false),
      print_user_gesture_status_(false),
      can_process_drag_(false),
      supports_keyboard_focus_(false),
      is_persistent_(params.mime_type == PluginPersistsMimeType()),
      can_create_without_renderer_(params.mime_type ==
                                   CanCreateWithoutRendererMimeType()) {
  DCHECK_EQ(params.attribute_names.size(), params.attribute_values.size());
  size_t size = params.attribute_names.size();
  for (size_t i = 0; i < size; ++i) {
    const blink::WebString& attribute_name = params.attribute_names[i];
    const blink::WebString& attribute_value = params.attribute_values[i];

    if (attribute_name == "primitive")
      scene_.primitive = ParsePrimitive(attribute_value);
    else if (attribute_name == "background-color")
      ParseColor(attribute_value, scene_.background_color);
    else if (attribute_name == "primitive-color")
      ParseColor(attribute_value, scene_.primitive_color);
    else if (attribute_name == "opacity")
      scene_.opacity = ParseOpacity(attribute_value);
    else if (attribute_name == "accepts-touch")
      touch_event_request_ = ParseTouchEventRequestType(attribute_value);
    else if (attribute_name == "re-request-touch")
      re_request_touch_events_ = ParseBoolean(attribute_value);
    else if (attribute_name == "print-event-details")
      print_event_details_ = ParseBoolean(attribute_value);
    else if (attribute_name == "can-process-drag")
      can_process_drag_ = ParseBoolean(attribute_value);
    else if (attribute_name == "supports-keyboard-focus")
      supports_keyboard_focus_ = ParseBoolean(attribute_value);
    else if (attribute_name == "print-user-gesture-status")
      print_user_gesture_status_ = ParseBoolean(attribute_value);
  }
  if (can_create_without_renderer_)
    delegate_->PrintMessage(
        std::string("TestPlugin: canCreateWithoutRenderer\n"));
}

TestPlugin::~TestPlugin() {}

bool TestPlugin::Initialize(blink::WebPluginContainer* container) {
  DCHECK(container);
  DCHECK_EQ(this, container->Plugin());

  container_ = container;

  blink::Platform::ContextAttributes attrs;
  blink::WebURL url = container->GetDocument().Url();
  blink::Platform::GraphicsInfo gl_info;
  context_provider_ =
      blink::Platform::Current()->CreateOffscreenGraphicsContext3DProvider(
          attrs, url, nullptr, &gl_info);
  if (context_provider_ && !context_provider_->BindToCurrentThread())
    context_provider_ = nullptr;
  gl_ = context_provider_ ? context_provider_->ContextGL() : nullptr;

  if (!InitScene())
    return false;

  layer_ = cc::TextureLayer::CreateForMailbox(this);
  web_layer_ = base::WrapUnique(new cc_blink::WebLayerImpl(layer_));
  container_->SetWebLayer(web_layer_.get());
  if (re_request_touch_events_) {
    container_->RequestTouchEventType(
        blink::WebPluginContainer::kTouchEventRequestTypeSynthesizedMouse);
    container_->RequestTouchEventType(
        blink::WebPluginContainer::kTouchEventRequestTypeRaw);
  }
  container_->RequestTouchEventType(touch_event_request_);
  container_->SetWantsWheelEvents(true);
  return true;
}

void TestPlugin::Destroy() {
  if (layer_.get())
    layer_->ClearTexture();
  if (container_)
    container_->SetWebLayer(nullptr);
  web_layer_.reset();
  layer_ = nullptr;
  DestroyScene();

  gl_ = nullptr;
  context_provider_.reset();

  container_ = nullptr;

  blink::Platform::Current()->MainThread()->GetTaskRunner()->DeleteSoon(
      FROM_HERE, this);
}

blink::WebPluginContainer* TestPlugin::Container() const {
  return container_;
}

bool TestPlugin::CanProcessDrag() const {
  return can_process_drag_;
}

bool TestPlugin::SupportsKeyboardFocus() const {
  return supports_keyboard_focus_;
}

void TestPlugin::UpdateGeometry(
    const blink::WebRect& window_rect,
    const blink::WebRect& clip_rect,
    const blink::WebRect& unobscured_rect,
    bool is_visible) {
  if (clip_rect == rect_)
    return;
  rect_ = clip_rect;

  if (rect_.IsEmpty()) {
    mailbox_ = gpu::Mailbox();
    sync_token_ = gpu::SyncToken();
    shared_bitmap_ = nullptr;
  } else if (gl_) {
    gl_->Viewport(0, 0, rect_.width, rect_.height);

    gl_->BindTexture(GL_TEXTURE_2D, color_texture_);
    gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl_->TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rect_.width, rect_.height, 0,
                    GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl_->BindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    gl_->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_TEXTURE_2D, color_texture_, 0);

    DrawSceneGL();

    gl_->GenMailboxCHROMIUM(mailbox_.name);
    gl_->ProduceTextureDirectCHROMIUM(color_texture_, mailbox_.name);
    gl_->Flush();
    gl_->GenSyncTokenCHROMIUM(sync_token_.GetData());

    shared_bitmap_ = nullptr;
  } else {
    mailbox_ = gpu::Mailbox();
    sync_token_ = gpu::SyncToken();
    shared_bitmap_ = delegate_->GetSharedBitmapManager()->AllocateSharedBitmap(
        gfx::Rect(rect_).size(), viz::RGBA_8888);
    DrawSceneSoftware(shared_bitmap_->pixels());
  }

  content_changed_ = true;
  layer_->SetNeedsDisplay();
}

bool TestPlugin::IsPlaceholder() {
  return false;
}

static void IgnoreReleaseCallback(const gpu::SyncToken& sync_token, bool lost) {
}

static void ReleaseSharedMemory(std::unique_ptr<viz::SharedBitmap> bitmap,
                                const gpu::SyncToken& sync_token,
                                bool lost) {}

bool TestPlugin::PrepareTransferableResource(
    viz::TransferableResource* resource,
    std::unique_ptr<viz::SingleReleaseCallback>* release_callback) {
  if (!content_changed_)
    return false;
  if (!mailbox_.IsZero()) {
    *resource = viz::TransferableResource::MakeGL(mailbox_, GL_LINEAR,
                                                  GL_TEXTURE_2D, sync_token_);
    *release_callback = viz::SingleReleaseCallback::Create(
        base::BindOnce(&IgnoreReleaseCallback));
  } else if (shared_bitmap_) {
    *resource = viz::TransferableResource::MakeSoftware(
        shared_bitmap_->id(), shared_bitmap_->sequence_number(),
        gfx::Size(rect_.width, rect_.height), viz::RGBA_8888);
    *release_callback = viz::SingleReleaseCallback::Create(
        base::BindOnce(&ReleaseSharedMemory, base::Passed(&shared_bitmap_)));
  }
  content_changed_ = false;
  return true;
}

TestPlugin::Primitive TestPlugin::ParsePrimitive(
    const blink::WebString& string) {
  const CR_DEFINE_STATIC_LOCAL(blink::WebString, kPrimitiveNone, ("none"));
  const CR_DEFINE_STATIC_LOCAL(blink::WebString, kPrimitiveTriangle,
                               ("triangle"));

  Primitive primitive = PrimitiveNone;
  if (string == kPrimitiveNone)
    primitive = PrimitiveNone;
  else if (string == kPrimitiveTriangle)
    primitive = PrimitiveTriangle;
  else
    NOTREACHED();
  return primitive;
}

// FIXME: This method should already exist. Use it.
// For now just parse primary colors.
void TestPlugin::ParseColor(const blink::WebString& string, uint8_t color[3]) {
  color[0] = color[1] = color[2] = 0;
  if (string == "black")
    return;

  if (string == "red")
    color[0] = 255;
  else if (string == "green")
    color[1] = 255;
  else if (string == "blue")
    color[2] = 255;
  else
    NOTREACHED();
}

float TestPlugin::ParseOpacity(const blink::WebString& string) {
  return static_cast<float>(atof(string.Utf8().data()));
}

bool TestPlugin::ParseBoolean(const blink::WebString& string) {
  const CR_DEFINE_STATIC_LOCAL(blink::WebString, kPrimitiveTrue, ("true"));
  return string == kPrimitiveTrue;
}

bool TestPlugin::InitScene() {
  if (!gl_)
    return true;

  float color[4];
  PremultiplyAlpha(scene_.background_color, scene_.opacity, color);

  gl_->GenTextures(1, &color_texture_);
  gl_->GenFramebuffers(1, &framebuffer_);

  gl_->Viewport(0, 0, rect_.width, rect_.height);
  gl_->Disable(GL_DEPTH_TEST);
  gl_->Disable(GL_SCISSOR_TEST);

  gl_->ClearColor(color[0], color[1], color[2], color[3]);

  gl_->Enable(GL_BLEND);
  gl_->BlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

  return scene_.primitive != PrimitiveNone ? InitProgram() && InitPrimitive()
                                           : true;
}

void TestPlugin::DrawSceneGL() {
  gl_->Viewport(0, 0, rect_.width, rect_.height);
  gl_->Clear(GL_COLOR_BUFFER_BIT);

  if (scene_.primitive != PrimitiveNone)
    DrawPrimitive();
}

void TestPlugin::DrawSceneSoftware(void* memory) {
  SkColor background_color = SkColorSetARGB(
      static_cast<uint8_t>(scene_.opacity * 255), scene_.background_color[0],
      scene_.background_color[1], scene_.background_color[2]);

  const SkImageInfo info =
      SkImageInfo::MakeN32Premul(rect_.width, rect_.height);
  SkBitmap bitmap;
  bitmap.installPixels(info, memory, info.minRowBytes());
  SkCanvas canvas(bitmap);
  canvas.clear(background_color);

  if (scene_.primitive != PrimitiveNone) {
    DCHECK_EQ(PrimitiveTriangle, scene_.primitive);
    SkColor foreground_color = SkColorSetARGB(
        static_cast<uint8_t>(scene_.opacity * 255), scene_.primitive_color[0],
        scene_.primitive_color[1], scene_.primitive_color[2]);
    SkPath triangle_path;
    triangle_path.moveTo(0.5f * rect_.width, 0.9f * rect_.height);
    triangle_path.lineTo(0.1f * rect_.width, 0.1f * rect_.height);
    triangle_path.lineTo(0.9f * rect_.width, 0.1f * rect_.height);
    SkPaint paint;
    paint.setColor(foreground_color);
    paint.setStyle(SkPaint::kFill_Style);
    canvas.drawPath(triangle_path, paint);
  }
}

void TestPlugin::DestroyScene() {
  if (scene_.program) {
    gl_->DeleteProgram(scene_.program);
    scene_.program = 0;
  }
  if (scene_.vbo) {
    gl_->DeleteBuffers(1, &scene_.vbo);
    scene_.vbo = 0;
  }

  if (framebuffer_) {
    gl_->DeleteFramebuffers(1, &framebuffer_);
    framebuffer_ = 0;
  }

  if (color_texture_) {
    gl_->DeleteTextures(1, &color_texture_);
    color_texture_ = 0;
  }
}

bool TestPlugin::InitProgram() {
  const std::string vertex_source(
      "attribute vec4 position;  \n"
      "void main() {             \n"
      "  gl_Position = position; \n"
      "}                         \n");

  const std::string fragment_source(
      "precision mediump float; \n"
      "uniform vec4 color;      \n"
      "void main() {            \n"
      "  gl_FragColor = color;  \n"
      "}                        \n");

  scene_.program = LoadProgram(vertex_source, fragment_source);
  if (!scene_.program)
    return false;

  scene_.color_location = gl_->GetUniformLocation(scene_.program, "color");
  scene_.position_location = gl_->GetAttribLocation(scene_.program, "position");
  return true;
}

bool TestPlugin::InitPrimitive() {
  DCHECK_EQ(scene_.primitive, PrimitiveTriangle);

  gl_->GenBuffers(1, &scene_.vbo);
  if (!scene_.vbo)
    return false;

  const float vertices[] = {0.0f, 0.8f, 0.0f,  -0.8f, -0.8f,
                            0.0f, 0.8f, -0.8f, 0.0f};
  gl_->BindBuffer(GL_ARRAY_BUFFER, scene_.vbo);
  gl_->BufferData(GL_ARRAY_BUFFER, sizeof(vertices), nullptr, GL_STATIC_DRAW);
  gl_->BufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
  return true;
}

void TestPlugin::DrawPrimitive() {
  DCHECK_EQ(scene_.primitive, PrimitiveTriangle);
  DCHECK(scene_.vbo);
  DCHECK(scene_.program);

  gl_->UseProgram(scene_.program);

  // Bind primitive color.
  float color[4];
  PremultiplyAlpha(scene_.primitive_color, scene_.opacity, color);
  gl_->Uniform4f(scene_.color_location, color[0], color[1], color[2], color[3]);

  // Bind primitive vertices.
  gl_->BindBuffer(GL_ARRAY_BUFFER, scene_.vbo);
  gl_->EnableVertexAttribArray(scene_.position_location);
  gl_->VertexAttribPointer(scene_.position_location, 3, GL_FLOAT, GL_FALSE, 0,
                           nullptr);
  gl_->DrawArrays(GL_TRIANGLES, 0, 3);
}

GLuint TestPlugin::LoadShader(GLenum type, const std::string& source) {
  GLuint shader = gl_->CreateShader(type);
  if (shader) {
    const GLchar* shader_data = source.data();
    GLint shader_length = strlen(shader_data);  // source.length();
    gl_->ShaderSource(shader, 1, &shader_data, &shader_length);
    gl_->CompileShader(shader);

    int compiled = 0;
    gl_->GetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
      gl_->DeleteShader(shader);
      shader = 0;
    }
  }
  return shader;
}

GLuint TestPlugin::LoadProgram(const std::string& vertex_source,
                               const std::string& fragment_source) {
  GLuint vertex_shader = LoadShader(GL_VERTEX_SHADER, vertex_source);
  GLuint fragment_shader = LoadShader(GL_FRAGMENT_SHADER, fragment_source);
  GLuint program = gl_->CreateProgram();
  if (vertex_shader && fragment_shader && program) {
    gl_->AttachShader(program, vertex_shader);
    gl_->AttachShader(program, fragment_shader);
    gl_->LinkProgram(program);

    int linked = 0;
    gl_->GetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
      gl_->DeleteProgram(program);
      program = 0;
    }
  }
  if (vertex_shader)
    gl_->DeleteShader(vertex_shader);
  if (fragment_shader)
    gl_->DeleteShader(fragment_shader);

  return program;
}

blink::WebInputEventResult TestPlugin::HandleInputEvent(
    const blink::WebCoalescedInputEvent& coalesced_event,
    blink::WebCursorInfo& info) {
  const blink::WebInputEvent& event = coalesced_event.Event();
  const char* event_name = blink::WebInputEvent::GetName(event.GetType());
  if (!strcmp(event_name, "") || !strcmp(event_name, "Undefined"))
    event_name = "unknown";
  delegate_->PrintMessage(std::string("Plugin received event: ") + event_name +
                          "\n");
  if (print_event_details_)
    PrintEventDetails(delegate_, event);

  if (print_user_gesture_status_) {
    bool has_user_gesture =
        blink::WebUserGestureIndicator::IsProcessingUserGesture(
            web_local_frame_);
    delegate_->PrintMessage(std::string("* ") +
                            (has_user_gesture ? "" : "not ") +
                            "handling user gesture\n");
  }

  if (is_persistent_)
    delegate_->PrintMessage(std::string("TestPlugin: isPersistent\n"));
  return blink::WebInputEventResult::kNotHandled;
}

bool TestPlugin::HandleDragStatusUpdate(
    blink::WebDragStatus drag_status,
    const blink::WebDragData& data,
    blink::WebDragOperationsMask mask,
    const blink::WebFloatPoint& position,
    const blink::WebFloatPoint& screen_position) {
  const char* drag_status_name = nullptr;
  switch (drag_status) {
    case blink::kWebDragStatusEnter:
      drag_status_name = "DragEnter";
      break;
    case blink::kWebDragStatusOver:
      drag_status_name = "DragOver";
      break;
    case blink::kWebDragStatusLeave:
      drag_status_name = "DragLeave";
      break;
    case blink::kWebDragStatusDrop:
      drag_status_name = "DragDrop";
      break;
    case blink::kWebDragStatusUnknown:
      NOTREACHED();
  }
  delegate_->PrintMessage(std::string("Plugin received event: ") +
                          drag_status_name + "\n");
  return false;
}

TestPlugin* TestPlugin::Create(const blink::WebPluginParams& params,
                               WebTestDelegate* delegate,
                               blink::WebLocalFrame* frame) {
  return new TestPlugin(params, delegate, frame);
}

const blink::WebString& TestPlugin::MimeType() {
  const CR_DEFINE_STATIC_LOCAL(blink::WebString, kMimeType,
                               ("application/x-webkit-test-webplugin"));
  return kMimeType;
}

const blink::WebString& TestPlugin::CanCreateWithoutRendererMimeType() {
  const CR_DEFINE_STATIC_LOCAL(
      blink::WebString, kCanCreateWithoutRendererMimeType,
      ("application/x-webkit-test-webplugin-can-create-without-renderer"));
  return kCanCreateWithoutRendererMimeType;
}

const blink::WebString& TestPlugin::PluginPersistsMimeType() {
  const CR_DEFINE_STATIC_LOCAL(
      blink::WebString, kPluginPersistsMimeType,
      ("application/x-webkit-test-webplugin-persistent"));
  return kPluginPersistsMimeType;
}

bool TestPlugin::IsSupportedMimeType(const blink::WebString& mime_type) {
  return mime_type == TestPlugin::MimeType() ||
         mime_type == PluginPersistsMimeType() ||
         mime_type == CanCreateWithoutRendererMimeType();
}

}  // namespace test_runner
