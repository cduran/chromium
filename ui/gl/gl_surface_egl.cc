// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/gl/gl_surface_egl.h"

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <memory>
#include <vector>

#include "base/command_line.h"
#include "base/lazy_instance.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/memory/ptr_util.h"
#include "base/message_loop/message_loop.h"
#include "base/metrics/histogram_macros.h"
#include "base/strings/string_number_conversions.h"
#include "base/sys_info.h"
#include "base/trace_event/trace_event.h"
#include "build/build_config.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gl/angle_platform_impl.h"
#include "ui/gl/egl_util.h"
#include "ui/gl/gl_context.h"
#include "ui/gl/gl_context_egl.h"
#include "ui/gl/gl_image.h"
#include "ui/gl/gl_implementation.h"
#include "ui/gl/gl_surface_presentation_helper.h"
#include "ui/gl/gl_surface_stub.h"
#include "ui/gl/gl_utils.h"
#include "ui/gl/scoped_make_current.h"
#include "ui/gl/sync_control_vsync_provider.h"

#if defined(USE_X11)
#include "ui/gfx/x/x11.h"

#include "ui/base/x/x11_util_internal.h"  // nogncheck
#endif

#if defined(OS_ANDROID)
#include <android/native_window_jni.h>
#include "base/android/build_info.h"
#endif

#if !defined(EGL_FIXED_SIZE_ANGLE)
#define EGL_FIXED_SIZE_ANGLE 0x3201
#endif

#if !defined(EGL_OPENGL_ES3_BIT)
#define EGL_OPENGL_ES3_BIT 0x00000040
#endif

// Not present egl/eglext.h yet.

#ifndef EGL_EXT_gl_colorspace_display_p3
#define EGL_EXT_gl_colorspace_display_p3 1
#define EGL_GL_COLORSPACE_DISPLAY_P3_EXT 0x3363
#endif /* EGL_EXT_gl_colorspace_display_p3 */

// From ANGLE's egl/eglext.h.

#ifndef EGL_ANGLE_platform_angle
#define EGL_ANGLE_platform_angle 1
#define EGL_PLATFORM_ANGLE_ANGLE 0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#define EGL_PLATFORM_ANGLE_MAX_VERSION_MAJOR_ANGLE 0x3204
#define EGL_PLATFORM_ANGLE_MAX_VERSION_MINOR_ANGLE 0x3205
#define EGL_PLATFORM_ANGLE_TYPE_DEFAULT_ANGLE 0x3206
#define EGL_PLATFORM_ANGLE_DEBUG_LAYERS_ENABLED_ANGLE 0x3451
#define EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE 0x3209
#define EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE 0x320A
#define EGL_PLATFORM_ANGLE_DEVICE_TYPE_NULL_ANGLE 0x345E
#endif /* EGL_ANGLE_platform_angle */

#ifndef EGL_ANGLE_platform_angle_d3d
#define EGL_ANGLE_platform_angle_d3d 1
#define EGL_PLATFORM_ANGLE_TYPE_D3D9_ANGLE 0x3207
#define EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE 0x3208
#define EGL_PLATFORM_ANGLE_DEVICE_TYPE_D3D_WARP_ANGLE 0x320B
#define EGL_PLATFORM_ANGLE_DEVICE_TYPE_D3D_REFERENCE_ANGLE 0x320C
#endif /* EGL_ANGLE_platform_angle_d3d */

#ifndef EGL_ANGLE_platform_angle_opengl
#define EGL_ANGLE_platform_angle_opengl 1
#define EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE 0x320D
#define EGL_PLATFORM_ANGLE_TYPE_OPENGLES_ANGLE 0x320E
#endif /* EGL_ANGLE_platform_angle_opengl */

#ifndef EGL_ANGLE_platform_angle_null
#define EGL_ANGLE_platform_angle_null 1
#define EGL_PLATFORM_ANGLE_TYPE_NULL_ANGLE 0x33AE
#endif /* EGL_ANGLE_platform_angle_null */

#ifndef EGL_ANGLE_x11_visual
#define EGL_ANGLE_x11_visual 1
#define EGL_X11_VISUAL_ID_ANGLE 0x33A3
#endif /* EGL_ANGLE_x11_visual */

#ifndef EGL_ANGLE_surface_orientation
#define EGL_ANGLE_surface_orientation
#define EGL_OPTIMAL_SURFACE_ORIENTATION_ANGLE 0x33A7
#define EGL_SURFACE_ORIENTATION_ANGLE 0x33A8
#define EGL_SURFACE_ORIENTATION_INVERT_X_ANGLE 0x0001
#define EGL_SURFACE_ORIENTATION_INVERT_Y_ANGLE 0x0002
#endif /* EGL_ANGLE_surface_orientation */

#ifndef EGL_ANGLE_direct_composition
#define EGL_ANGLE_direct_composition 1
#define EGL_DIRECT_COMPOSITION_ANGLE 0x33A5
#endif /* EGL_ANGLE_direct_composition */

#ifndef EGL_ANGLE_flexible_surface_compatibility
#define EGL_ANGLE_flexible_surface_compatibility 1
#define EGL_FLEXIBLE_SURFACE_COMPATIBILITY_SUPPORTED_ANGLE 0x33A6
#endif /* EGL_ANGLE_flexible_surface_compatibility */

#ifndef EGL_ANGLE_display_robust_resource_initialization
#define EGL_ANGLE_display_robust_resource_initialization 1
#define EGL_DISPLAY_ROBUST_RESOURCE_INITIALIZATION_ANGLE 0x3453
#endif /* EGL_ANGLE_display_robust_resource_initialization */

using ui::GetLastEGLErrorString;

namespace gl {

bool GLSurfaceEGL::initialized_ = false;

namespace {

EGLDisplay g_egl_display = EGL_NO_DISPLAY;
EGLNativeDisplayType g_native_display = EGL_DEFAULT_DISPLAY;

const char* g_egl_extensions = nullptr;
bool g_egl_create_context_robustness_supported = false;
bool g_egl_create_context_bind_generates_resource_supported = false;
bool g_egl_create_context_webgl_compatability_supported = false;
bool g_egl_sync_control_supported = false;
bool g_egl_window_fixed_size_supported = false;
bool g_egl_surfaceless_context_supported = false;
bool g_egl_surface_orientation_supported = false;
bool g_egl_context_priority_supported = false;
bool g_egl_khr_colorspace = false;
bool g_egl_ext_colorspace_display_p3 = false;
bool g_use_direct_composition = false;
bool g_egl_robust_resource_init_supported = false;
bool g_egl_display_texture_share_group_supported = false;
bool g_egl_create_context_client_arrays_supported = false;
bool g_egl_android_native_fence_sync_supported = false;

const char kSwapEventTraceCategories[] = "gpu";

constexpr size_t kMaxTimestampsSupportable = 9;

struct TraceSwapEventsInitializer {
  TraceSwapEventsInitializer()
      : value(*TRACE_EVENT_API_GET_CATEGORY_GROUP_ENABLED(
            kSwapEventTraceCategories)) {}
  const unsigned char& value;
};

static base::LazyInstance<TraceSwapEventsInitializer>::Leaky
    g_trace_swap_enabled = LAZY_INSTANCE_INITIALIZER;

class EGLSyncControlVSyncProvider : public SyncControlVSyncProvider {
 public:
  explicit EGLSyncControlVSyncProvider(EGLSurface surface)
      : SyncControlVSyncProvider(),
        surface_(surface) {
  }

  ~EGLSyncControlVSyncProvider() override {}

  static bool IsSupported() {
    return SyncControlVSyncProvider::IsSupported() &&
           g_egl_sync_control_supported;
  }

 protected:
  bool GetSyncValues(int64_t* system_time,
                     int64_t* media_stream_counter,
                     int64_t* swap_buffer_counter) override {
    uint64_t u_system_time, u_media_stream_counter, u_swap_buffer_counter;
    bool result =
        eglGetSyncValuesCHROMIUM(g_egl_display, surface_, &u_system_time,
                                 &u_media_stream_counter,
                                 &u_swap_buffer_counter) == EGL_TRUE;
    if (result) {
      *system_time = static_cast<int64_t>(u_system_time);
      *media_stream_counter = static_cast<int64_t>(u_media_stream_counter);
      *swap_buffer_counter = static_cast<int64_t>(u_swap_buffer_counter);
    }
    return result;
  }

  bool GetMscRate(int32_t* numerator, int32_t* denominator) override {
    return false;
  }

  bool IsHWClock() const override { return true; }

 private:
  EGLSurface surface_;

  DISALLOW_COPY_AND_ASSIGN(EGLSyncControlVSyncProvider);
};

EGLDisplay GetPlatformANGLEDisplay(EGLNativeDisplayType native_display,
                                   EGLenum platform_type,
                                   bool warpDevice,
                                   bool nullDevice) {
  std::vector<EGLint> display_attribs;

  display_attribs.push_back(EGL_PLATFORM_ANGLE_TYPE_ANGLE);
  display_attribs.push_back(platform_type);

  if (warpDevice) {
    DCHECK(!nullDevice);
    display_attribs.push_back(EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE);
    display_attribs.push_back(EGL_PLATFORM_ANGLE_DEVICE_TYPE_D3D_WARP_ANGLE);
  } else if (nullDevice) {
    DCHECK(!warpDevice);
    display_attribs.push_back(EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE);
    display_attribs.push_back(EGL_PLATFORM_ANGLE_DEVICE_TYPE_NULL_ANGLE);
  }

#if defined(USE_X11)
  // ANGLE_NULL doesn't use the visual, and may run without X11 where we can't
  // get it anyway.
  if (platform_type != EGL_PLATFORM_ANGLE_TYPE_NULL_ANGLE) {
    Visual* visual;
    ui::XVisualManager::GetInstance()->ChooseVisualForWindow(
        true, &visual, nullptr, nullptr, nullptr);
    display_attribs.push_back(EGL_X11_VISUAL_ID_ANGLE);
    display_attribs.push_back(static_cast<EGLint>(XVisualIDFromVisual(visual)));
  }
#endif

  display_attribs.push_back(EGL_NONE);

  return eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE,
                                  reinterpret_cast<void*>(native_display),
                                  &display_attribs[0]);
}

EGLDisplay GetDisplayFromType(DisplayType display_type,
                              EGLNativeDisplayType native_display) {
  switch (display_type) {
    case DEFAULT:
    case SWIFT_SHADER:
      return eglGetDisplay(native_display);
    case ANGLE_D3D9:
      return GetPlatformANGLEDisplay(
          native_display, EGL_PLATFORM_ANGLE_TYPE_D3D9_ANGLE, false, false);
    case ANGLE_D3D11:
      return GetPlatformANGLEDisplay(
          native_display, EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE, false, false);
    case ANGLE_D3D11_NULL:
      return GetPlatformANGLEDisplay(
          native_display, EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE, false, true);
    case ANGLE_OPENGL:
      return GetPlatformANGLEDisplay(
          native_display, EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE, false, false);
    case ANGLE_OPENGL_NULL:
      return GetPlatformANGLEDisplay(
          native_display, EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE, false, true);
    case ANGLE_OPENGLES:
      return GetPlatformANGLEDisplay(
          native_display, EGL_PLATFORM_ANGLE_TYPE_OPENGLES_ANGLE, false, false);
    case ANGLE_OPENGLES_NULL:
      return GetPlatformANGLEDisplay(
          native_display, EGL_PLATFORM_ANGLE_TYPE_OPENGLES_ANGLE, false, true);
    case ANGLE_NULL:
      return GetPlatformANGLEDisplay(
          native_display, EGL_PLATFORM_ANGLE_TYPE_NULL_ANGLE, false, false);
    default:
      NOTREACHED();
      return EGL_NO_DISPLAY;
  }
}

const char* DisplayTypeString(DisplayType display_type) {
  switch (display_type) {
    case DEFAULT:
      return "Default";
    case SWIFT_SHADER:
      return "SwiftShader";
    case ANGLE_D3D9:
      return "D3D9";
    case ANGLE_D3D11:
      return "D3D11";
    case ANGLE_D3D11_NULL:
      return "D3D11Null";
    case ANGLE_OPENGL:
      return "OpenGL";
    case ANGLE_OPENGL_NULL:
      return "OpenGLNull";
    case ANGLE_OPENGLES:
      return "OpenGLES";
    case ANGLE_OPENGLES_NULL:
      return "OpenGLESNull";
    case ANGLE_NULL:
      return "Null";
    default:
      NOTREACHED();
      return "Err";
  }
}

bool ValidateEglConfig(EGLDisplay display,
                       const EGLint* config_attribs,
                       EGLint* num_configs) {
  if (!eglChooseConfig(display,
                       config_attribs,
                       NULL,
                       0,
                       num_configs)) {
    LOG(ERROR) << "eglChooseConfig failed with error "
               << GetLastEGLErrorString();
    return false;
  }
  if (*num_configs == 0) {
    return false;
  }
  return true;
}

EGLConfig ChooseConfig(GLSurfaceFormat format, bool surfaceless) {
  // Choose an EGL configuration.
  // On X this is only used for PBuffer surfaces.

  std::vector<EGLint> renderable_types;
  if (!base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kDisableES3GLContext)) {
    renderable_types.push_back(EGL_OPENGL_ES3_BIT);
  }
  renderable_types.push_back(EGL_OPENGL_ES2_BIT);

  EGLint buffer_size = format.GetBufferSize();
  EGLint alpha_size = 8;
  bool want_rgb565 = buffer_size == 16;
  EGLint depth_size = format.GetDepthBits();
  EGLint stencil_size = format.GetStencilBits();
  EGLint samples = format.GetSamples();

#if defined(USE_X11)
  // If we're using ANGLE_NULL, we may not have a display, in which case we
  // can't use XVisualManager.
  if (g_native_display) {
    ui::XVisualManager::GetInstance()->ChooseVisualForWindow(
        true, nullptr, &buffer_size, nullptr, nullptr);
    alpha_size = buffer_size == 32 ? 8 : 0;
  }
#endif

  EGLint surface_type =
      (surfaceless ? EGL_DONT_CARE : EGL_WINDOW_BIT | EGL_PBUFFER_BIT);

  for (auto renderable_type : renderable_types) {
    EGLint config_attribs_8888[] = {EGL_BUFFER_SIZE,
                                    buffer_size,
                                    EGL_ALPHA_SIZE,
                                    alpha_size,
                                    EGL_BLUE_SIZE,
                                    8,
                                    EGL_GREEN_SIZE,
                                    8,
                                    EGL_RED_SIZE,
                                    8,
                                    EGL_SAMPLES,
                                    samples,
                                    EGL_DEPTH_SIZE,
                                    depth_size,
                                    EGL_STENCIL_SIZE,
                                    stencil_size,
                                    EGL_RENDERABLE_TYPE,
                                    renderable_type,
                                    EGL_SURFACE_TYPE,
                                    surface_type,
                                    EGL_NONE};

    EGLint config_attribs_565[] = {EGL_BUFFER_SIZE,
                                   16,
                                   EGL_BLUE_SIZE,
                                   5,
                                   EGL_GREEN_SIZE,
                                   6,
                                   EGL_RED_SIZE,
                                   5,
                                   EGL_SAMPLES,
                                   samples,
                                   EGL_DEPTH_SIZE,
                                   depth_size,
                                   EGL_STENCIL_SIZE,
                                   stencil_size,
                                   EGL_RENDERABLE_TYPE,
                                   renderable_type,
                                   EGL_SURFACE_TYPE,
                                   surface_type,
                                   EGL_NONE};

    EGLint* choose_attributes = config_attribs_8888;
    if (want_rgb565) {
      choose_attributes = config_attribs_565;
    }

    EGLint num_configs;
    EGLint config_size = 1;
    EGLConfig config = nullptr;
    EGLConfig* config_data = &config;
    // Validate if there are any configs for given attribs.
    if (!ValidateEglConfig(g_egl_display, choose_attributes, &num_configs)) {
      // Try the next renderable_type
      continue;
    }

    std::unique_ptr<EGLConfig[]> matching_configs(new EGLConfig[num_configs]);
    if (want_rgb565) {
      config_size = num_configs;
      config_data = matching_configs.get();
    }

    if (!eglChooseConfig(g_egl_display, choose_attributes, config_data,
                         config_size, &num_configs)) {
      LOG(ERROR) << "eglChooseConfig failed with error "
                 << GetLastEGLErrorString();
      return config;
    }

    if (want_rgb565) {
      // Because of the EGL config sort order, we have to iterate
      // through all of them (it'll put higher sum(R,G,B) bits
      // first with the above attribs).
      bool match_found = false;
      for (int i = 0; i < num_configs; i++) {
        EGLint red, green, blue, alpha;
        // Read the relevant attributes of the EGLConfig.
        if (eglGetConfigAttrib(g_egl_display, matching_configs[i], EGL_RED_SIZE,
                               &red) &&
            eglGetConfigAttrib(g_egl_display, matching_configs[i],
                               EGL_BLUE_SIZE, &blue) &&
            eglGetConfigAttrib(g_egl_display, matching_configs[i],
                               EGL_GREEN_SIZE, &green) &&
            eglGetConfigAttrib(g_egl_display, matching_configs[i],
                               EGL_ALPHA_SIZE, &alpha) &&
            alpha == 0 && red == 5 && green == 6 && blue == 5) {
          config = matching_configs[i];
          match_found = true;
          break;
        }
      }
      if (!match_found) {
        // To fall back to default 32 bit format, choose with
        // the right attributes again.
        if (!ValidateEglConfig(g_egl_display, config_attribs_8888,
                               &num_configs)) {
          // Try the next renderable_type
          continue;
        }
        if (!eglChooseConfig(g_egl_display, config_attribs_8888, &config, 1,
                             &num_configs)) {
          LOG(ERROR) << "eglChooseConfig failed with error "
                     << GetLastEGLErrorString();
          return config;
        }
      }
    }
    return config;
  }

  LOG(ERROR) << "No suitable EGL configs found.";
  return nullptr;
}

}  // namespace

void GetEGLInitDisplays(bool supports_angle_d3d,
                        bool supports_angle_opengl,
                        bool supports_angle_null,
                        const base::CommandLine* command_line,
                        std::vector<DisplayType>* init_displays) {
  // SwiftShader does not use the platform extensions
  if (command_line->GetSwitchValueASCII(switches::kUseGL) ==
      kGLImplementationSwiftShaderForWebGLName) {
    init_displays->push_back(SWIFT_SHADER);
    return;
  }

  std::string requested_renderer =
      command_line->GetSwitchValueASCII(switches::kUseANGLE);

  bool use_angle_default =
      !command_line->HasSwitch(switches::kUseANGLE) ||
      requested_renderer == kANGLEImplementationDefaultName;

  if (supports_angle_null &&
      requested_renderer == kANGLEImplementationNullName) {
    init_displays->push_back(ANGLE_NULL);
    return;
  }

  if (supports_angle_d3d) {
    if (use_angle_default) {
      // Default mode for ANGLE - try D3D11, else try D3D9
      if (!command_line->HasSwitch(switches::kDisableD3D11)) {
        init_displays->push_back(ANGLE_D3D11);
      }
      init_displays->push_back(ANGLE_D3D9);
    } else {
      if (requested_renderer == kANGLEImplementationD3D11Name) {
        init_displays->push_back(ANGLE_D3D11);
      } else if (requested_renderer == kANGLEImplementationD3D9Name) {
        init_displays->push_back(ANGLE_D3D9);
      } else if (requested_renderer == kANGLEImplementationD3D11NULLName) {
        init_displays->push_back(ANGLE_D3D11_NULL);
      }
    }
  }

  if (supports_angle_opengl) {
    if (use_angle_default && !supports_angle_d3d) {
      init_displays->push_back(ANGLE_OPENGL);
      init_displays->push_back(ANGLE_OPENGLES);
    } else {
      if (requested_renderer == kANGLEImplementationOpenGLName) {
        init_displays->push_back(ANGLE_OPENGL);
      } else if (requested_renderer == kANGLEImplementationOpenGLESName) {
        init_displays->push_back(ANGLE_OPENGLES);
      } else if (requested_renderer == kANGLEImplementationOpenGLNULLName) {
        init_displays->push_back(ANGLE_OPENGL_NULL);
      } else if (requested_renderer == kANGLEImplementationOpenGLESNULLName) {
        init_displays->push_back(ANGLE_OPENGLES_NULL);
      }
    }
  }

  // If no displays are available due to missing angle extensions or invalid
  // flags, request the default display.
  if (init_displays->empty()) {
    init_displays->push_back(DEFAULT);
  }
}

GLSurfaceEGL::GLSurfaceEGL() {}

GLSurfaceFormat GLSurfaceEGL::GetFormat() {
  return format_;
}

EGLDisplay GLSurfaceEGL::GetDisplay() {
  return g_egl_display;
}

EGLConfig GLSurfaceEGL::GetConfig() {
  if (!config_) {
    config_ = ChooseConfig(format_, IsSurfaceless());
  }
  return config_;
}

// static
bool GLSurfaceEGL::InitializeOneOff(EGLNativeDisplayType native_display) {
  if (initialized_)
    return true;

  // Must be called before InitializeDisplay().
  g_driver_egl.InitializeClientExtensionBindings();

  InitializeDisplay(native_display);
  if (g_egl_display == EGL_NO_DISPLAY)
    return false;

  // Must be called after InitializeDisplay().
  g_driver_egl.InitializeExtensionBindings();

  return InitializeOneOffCommon();
}

// static
bool GLSurfaceEGL::InitializeOneOffForTesting() {
  g_driver_egl.InitializeClientExtensionBindings();
  g_egl_display = eglGetCurrentDisplay();
  g_driver_egl.InitializeExtensionBindings();
  return InitializeOneOffCommon();
}

// static
bool GLSurfaceEGL::InitializeOneOffCommon() {
  g_egl_extensions = eglQueryString(g_egl_display, EGL_EXTENSIONS);

  g_egl_create_context_robustness_supported =
      HasEGLExtension("EGL_EXT_create_context_robustness");
  g_egl_create_context_bind_generates_resource_supported =
      HasEGLExtension("EGL_CHROMIUM_create_context_bind_generates_resource");
  g_egl_create_context_webgl_compatability_supported =
      HasEGLExtension("EGL_ANGLE_create_context_webgl_compatibility");
  g_egl_sync_control_supported =
      HasEGLExtension("EGL_CHROMIUM_sync_control");
  g_egl_window_fixed_size_supported =
      HasEGLExtension("EGL_ANGLE_window_fixed_size");
  g_egl_surface_orientation_supported =
      HasEGLExtension("EGL_ANGLE_surface_orientation");
  g_egl_khr_colorspace = HasEGLExtension("EGL_KHR_gl_colorspace");
  g_egl_ext_colorspace_display_p3 =
      HasEGLExtension("EGL_EXT_gl_colorspace_display_p3");
  // According to https://source.android.com/compatibility/android-cdd.html the
  // EGL_IMG_context_priority extension is mandatory for Virtual Reality High
  // Performance support, but due to a bug in Android Nougat the extension
  // isn't being reported even when it's present. As a fallback, check if other
  // related extensions that were added for VR support are present, and assume
  // that this implies context priority is also supported. See also:
  // https://github.com/googlevr/gvr-android-sdk/issues/330
  g_egl_context_priority_supported =
      HasEGLExtension("EGL_IMG_context_priority") ||
      (HasEGLExtension("EGL_ANDROID_front_buffer_auto_refresh") &&
       HasEGLExtension("EGL_ANDROID_create_native_client_buffer"));

#if defined(OS_WIN)
  // Need EGL_ANGLE_flexible_surface_compatibility to allow surfaces with and
  // without alpha to be bound to the same context. Blacklist direct composition
  // if MCTU.dll or MCTUX.dll are injected. These are user mode drivers for
  // display adapters from Magic Control Technology Corporation.
  g_use_direct_composition =
      HasEGLExtension("EGL_ANGLE_direct_composition") &&
      HasEGLExtension("EGL_ANGLE_flexible_surface_compatibility") &&
      !base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kDisableDirectComposition) &&
      !GetModuleHandle(TEXT("MCTU.dll")) && !GetModuleHandle(TEXT("MCTUX.dll"));
#endif

  g_egl_display_texture_share_group_supported =
      HasEGLExtension("EGL_ANGLE_display_texture_share_group");
  g_egl_create_context_client_arrays_supported =
      HasEGLExtension("EGL_ANGLE_create_context_client_arrays");
  g_egl_robust_resource_init_supported =
      HasEGLExtension("EGL_ANGLE_robust_resource_initialization");

  // TODO(oetuaho@nvidia.com): Surfaceless is disabled on Android as a temporary
  // workaround, since code written for Android WebView takes different paths
  // based on whether GL surface objects have underlying EGL surface handles,
  // conflicting with the use of surfaceless. See https://crbug.com/382349
#if defined(OS_ANDROID)
  DCHECK(!g_egl_surfaceless_context_supported);
#else
  // Check if SurfacelessEGL is supported.
  g_egl_surfaceless_context_supported =
      HasEGLExtension("EGL_KHR_surfaceless_context");
  if (g_egl_surfaceless_context_supported) {
    // EGL_KHR_surfaceless_context is supported but ensure
    // GL_OES_surfaceless_context is also supported. We need a current context
    // to query for supported GL extensions.
    scoped_refptr<GLSurface> surface = new SurfacelessEGL(gfx::Size(1, 1));
    scoped_refptr<GLContext> context = InitializeGLContext(
        new GLContextEGL(nullptr), surface.get(), GLContextAttribs());
    if (!context->MakeCurrent(surface.get()))
      g_egl_surfaceless_context_supported = false;

    // Ensure context supports GL_OES_surfaceless_context.
    if (g_egl_surfaceless_context_supported) {
      g_egl_surfaceless_context_supported = context->HasExtension(
          "GL_OES_surfaceless_context");
      context->ReleaseCurrent(surface.get());
    }
  }
#endif

  // The native fence sync extension is a bit complicated. It's reported as
  // present for ChromeOS, but Android currently doesn't report this extension
  // even when it's present, and older devices may export a useless wrapper
  // function. See crbug.com/775707 for details. In short, if the symbol is
  // present and we're on Android N or newer, assume that it's usable even if
  // the extension wasn't reported.
  g_egl_android_native_fence_sync_supported =
      HasEGLExtension("EGL_ANDROID_native_fence_sync");
#if defined(OS_ANDROID)
  if (base::android::BuildInfo::GetInstance()->sdk_int() >=
          base::android::SDK_VERSION_NOUGAT &&
      g_driver_egl.fn.eglDupNativeFenceFDANDROIDFn) {
    g_egl_android_native_fence_sync_supported = true;
  }
#endif

  initialized_ = true;
  return true;
}

// static
bool GLSurfaceEGL::InitializeExtensionSettingsOneOff() {
  if (!initialized_)
    return false;
  g_driver_egl.UpdateConditionalExtensionBindings();
  g_egl_extensions = eglQueryString(g_egl_display, EGL_EXTENSIONS);

  return true;
}

// static
void GLSurfaceEGL::ShutdownOneOff() {
  angle::ResetPlatform(g_egl_display);

  if (g_egl_display != EGL_NO_DISPLAY) {
    DCHECK(g_driver_egl.fn.eglTerminateFn);
    eglTerminate(g_egl_display);
  }
  g_egl_display = EGL_NO_DISPLAY;

  g_egl_extensions = nullptr;
  g_egl_create_context_robustness_supported = false;
  g_egl_create_context_bind_generates_resource_supported = false;
  g_egl_create_context_webgl_compatability_supported = false;
  g_egl_sync_control_supported = false;
  g_egl_window_fixed_size_supported = false;
  g_egl_surface_orientation_supported = false;
  g_use_direct_composition = false;
  g_egl_surfaceless_context_supported = false;
  g_egl_robust_resource_init_supported = false;
  g_egl_display_texture_share_group_supported = false;
  g_egl_create_context_client_arrays_supported = false;

  initialized_ = false;
}

// static
EGLDisplay GLSurfaceEGL::GetHardwareDisplay() {
  return g_egl_display;
}

// static
EGLNativeDisplayType GLSurfaceEGL::GetNativeDisplay() {
  return g_native_display;
}

// static
const char* GLSurfaceEGL::GetEGLExtensions() {
  return g_egl_extensions;
}

// static
bool GLSurfaceEGL::HasEGLExtension(const char* name) {
  return ExtensionsContain(GetEGLExtensions(), name);
}

// static
bool GLSurfaceEGL::IsCreateContextRobustnessSupported() {
  return g_egl_create_context_robustness_supported;
}

bool GLSurfaceEGL::IsCreateContextBindGeneratesResourceSupported() {
  return g_egl_create_context_bind_generates_resource_supported;
}

bool GLSurfaceEGL::IsCreateContextWebGLCompatabilitySupported() {
  return g_egl_create_context_webgl_compatability_supported;
}

// static
bool GLSurfaceEGL::IsEGLSurfacelessContextSupported() {
  return g_egl_surfaceless_context_supported;
}

// static
bool GLSurfaceEGL::IsEGLContextPrioritySupported() {
  return g_egl_context_priority_supported;
}

// static
bool GLSurfaceEGL::IsDirectCompositionSupported() {
  return g_use_direct_composition;
}

bool GLSurfaceEGL::IsRobustResourceInitSupported() {
  return g_egl_robust_resource_init_supported;
}

bool GLSurfaceEGL::IsDisplayTextureShareGroupSupported() {
  return g_egl_display_texture_share_group_supported;
}

bool GLSurfaceEGL::IsCreateContextClientArraysSupported() {
  return g_egl_create_context_client_arrays_supported;
}

bool GLSurfaceEGL::IsAndroidNativeFenceSyncSupported() {
  return g_egl_android_native_fence_sync_supported;
}

GLSurfaceEGL::~GLSurfaceEGL() {}

// InitializeDisplay is necessary because the static binding code
// needs a full Display init before it can query the Display extensions.
// static
EGLDisplay GLSurfaceEGL::InitializeDisplay(
    EGLNativeDisplayType native_display) {
  if (g_egl_display != EGL_NO_DISPLAY) {
    return g_egl_display;
  }

  g_native_display = native_display;

  // If EGL_EXT_client_extensions not supported this call to eglQueryString
  // will return NULL.
  const char* client_extensions =
      eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);

  bool supports_angle_d3d = false;
  bool supports_angle_opengl = false;
  bool supports_angle_null = false;
  // Check for availability of ANGLE extensions.
  if (client_extensions &&
      ExtensionsContain(client_extensions, "EGL_ANGLE_platform_angle")) {
    supports_angle_d3d =
        ExtensionsContain(client_extensions, "EGL_ANGLE_platform_angle_d3d");
    supports_angle_opengl =
        ExtensionsContain(client_extensions, "EGL_ANGLE_platform_angle_opengl");
    supports_angle_null =
        ExtensionsContain(client_extensions, "EGL_ANGLE_platform_angle_null");
  }

  std::vector<DisplayType> init_displays;
  GetEGLInitDisplays(supports_angle_d3d, supports_angle_opengl,
                     supports_angle_null,
                     base::CommandLine::ForCurrentProcess(), &init_displays);

  for (size_t disp_index = 0; disp_index < init_displays.size(); ++disp_index) {
    DisplayType display_type = init_displays[disp_index];
    EGLDisplay display = GetDisplayFromType(display_type, g_native_display);
    if (display == EGL_NO_DISPLAY) {
      LOG(ERROR) << "EGL display query failed with error "
                 << GetLastEGLErrorString();
    }

    // Init ANGLE platform now that we have the global display.
    if (supports_angle_d3d || supports_angle_opengl || supports_angle_null) {
      if (!angle::InitializePlatform(display)) {
        LOG(ERROR) << "ANGLE Platform initialization failed.";
      }
    }

    if (!eglInitialize(display, nullptr, nullptr)) {
      bool is_last = disp_index == init_displays.size() - 1;

      LOG(ERROR) << "eglInitialize " << DisplayTypeString(display_type)
                 << " failed with error " << GetLastEGLErrorString()
                 << (is_last ? "" : ", trying next display type");
    } else {
      UMA_HISTOGRAM_ENUMERATION("GPU.EGLDisplayType", display_type,
                                DISPLAY_TYPE_MAX);
      g_egl_display = display;
      break;
    }
  }

  return g_egl_display;
}

NativeViewGLSurfaceEGL::NativeViewGLSurfaceEGL(
    EGLNativeWindowType window,
    std::unique_ptr<gfx::VSyncProvider> vsync_provider)
    : window_(window),
      size_(1, 1),
      enable_fixed_size_angle_(true),
      surface_(NULL),
      supports_post_sub_buffer_(false),
      supports_swap_buffer_with_damage_(false),
      flips_vertically_(false),
      vsync_provider_external_(std::move(vsync_provider)),
      use_egl_timestamps_(false) {
#if defined(OS_ANDROID)
  if (window)
    ANativeWindow_acquire(window);
#endif

#if defined(OS_WIN)
  RECT windowRect;
  if (GetClientRect(window_, &windowRect))
    size_ = gfx::Rect(windowRect).size();
#endif
}

bool NativeViewGLSurfaceEGL::Initialize(GLSurfaceFormat format) {
  DCHECK(!surface_);
  format_ = format;

  if (!GetDisplay()) {
    LOG(ERROR) << "Trying to create surface with invalid display.";
    return false;
  }

  // We need to make sure that window_ is correctly initialized with all
  // the platform-dependant quirks, if any, before creating the surface.
  if (!InitializeNativeWindow()) {
    LOG(ERROR) << "Error trying to initialize the native window.";
    return false;
  }

  std::vector<EGLint> egl_window_attributes;

  if (g_egl_window_fixed_size_supported && enable_fixed_size_angle_) {
    egl_window_attributes.push_back(EGL_FIXED_SIZE_ANGLE);
    egl_window_attributes.push_back(EGL_TRUE);
    egl_window_attributes.push_back(EGL_WIDTH);
    egl_window_attributes.push_back(size_.width());
    egl_window_attributes.push_back(EGL_HEIGHT);
    egl_window_attributes.push_back(size_.height());
  }

  if (g_driver_egl.ext.b_EGL_NV_post_sub_buffer) {
    egl_window_attributes.push_back(EGL_POST_SUB_BUFFER_SUPPORTED_NV);
    egl_window_attributes.push_back(EGL_TRUE);
  }

  if (g_egl_surface_orientation_supported) {
    EGLint attrib;
    eglGetConfigAttrib(GetDisplay(), GetConfig(),
                       EGL_OPTIMAL_SURFACE_ORIENTATION_ANGLE, &attrib);
    flips_vertically_ = (attrib == EGL_SURFACE_ORIENTATION_INVERT_Y_ANGLE);
  }

  if (flips_vertically_) {
    egl_window_attributes.push_back(EGL_SURFACE_ORIENTATION_ANGLE);
    egl_window_attributes.push_back(EGL_SURFACE_ORIENTATION_INVERT_Y_ANGLE);
  }

  if (g_use_direct_composition) {
    egl_window_attributes.push_back(
        EGL_FLEXIBLE_SURFACE_COMPATIBILITY_SUPPORTED_ANGLE);
    egl_window_attributes.push_back(EGL_TRUE);
    egl_window_attributes.push_back(EGL_DIRECT_COMPOSITION_ANGLE);
    egl_window_attributes.push_back(EGL_TRUE);
  }

  switch (format_.GetColorSpace()) {
    case GLSurfaceFormat::COLOR_SPACE_UNSPECIFIED:
      break;
    case GLSurfaceFormat::COLOR_SPACE_SRGB:
      // Note that COLORSPACE_LINEAR refers to the sRGB color space, but
      // without opting into sRGB blending. It is equivalent to
      // COLORSPACE_SRGB with Disable(FRAMEBUFFER_SRGB).
      if (g_egl_khr_colorspace) {
        egl_window_attributes.push_back(EGL_GL_COLORSPACE_KHR);
        egl_window_attributes.push_back(EGL_GL_COLORSPACE_LINEAR_KHR);
      }
      break;
    case GLSurfaceFormat::COLOR_SPACE_DISPLAY_P3:
      // Note that it is not the case that
      //   COLORSPACE_SRGB is to COLORSPACE_LINEAR_KHR
      // as
      //   COLORSPACE_DISPLAY_P3 is to COLORSPACE_DISPLAY_P3_LINEAR
      // COLORSPACE_DISPLAY_P3 is equivalent to COLORSPACE_LINEAR, except with
      // with the P3 gamut instead of the the sRGB gamut.
      // COLORSPACE_DISPLAY_P3_LINEAR has a linear transfer function, and is
      // intended for use with 16-bit formats.
      if (g_egl_khr_colorspace && g_egl_ext_colorspace_display_p3) {
        egl_window_attributes.push_back(EGL_GL_COLORSPACE_KHR);
        egl_window_attributes.push_back(EGL_GL_COLORSPACE_DISPLAY_P3_EXT);
      }
      break;
  }

  egl_window_attributes.push_back(EGL_NONE);
  // Create a surface for the native window.
  surface_ = eglCreateWindowSurface(
      GetDisplay(), GetConfig(), window_, &egl_window_attributes[0]);

  if (!surface_) {
    LOG(ERROR) << "eglCreateWindowSurface failed with error "
               << GetLastEGLErrorString();
    Destroy();
    return false;
  }

  if (g_driver_egl.ext.b_EGL_NV_post_sub_buffer) {
    EGLint surfaceVal;
    EGLBoolean retVal = eglQuerySurface(
        GetDisplay(), surface_, EGL_POST_SUB_BUFFER_SUPPORTED_NV, &surfaceVal);
    supports_post_sub_buffer_ = (surfaceVal && retVal) == EGL_TRUE;
  }

  supports_swap_buffer_with_damage_ =
      g_driver_egl.ext.b_EGL_KHR_swap_buffers_with_damage;

  if (!vsync_provider_external_ && EGLSyncControlVSyncProvider::IsSupported()) {
    vsync_provider_internal_ =
        std::make_unique<EGLSyncControlVSyncProvider>(surface_);
  }
  presentation_helper_ =
      std::make_unique<GLSurfacePresentationHelper>(GetVSyncProvider());
  return true;
}

bool NativeViewGLSurfaceEGL::SupportsSwapTimestamps() const {
  return g_driver_egl.ext.b_EGL_ANDROID_get_frame_timestamps;
}

void NativeViewGLSurfaceEGL::SetEnableSwapTimestamps() {
  DCHECK(g_driver_egl.ext.b_EGL_ANDROID_get_frame_timestamps);

  // If frame timestamps are supported, set the proper attribute to enable the
  // feature and then cache the timestamps supported by the underlying
  // implementation. EGL_DISPLAY_PRESENT_TIME_ANDROID support, in particular,
  // is spotty.
  // Clear the supported timestamps here to protect against Initialize() being
  // called twice.
  supported_egl_timestamps_.clear();
  supported_event_names_.clear();

  eglSurfaceAttrib(GetDisplay(), surface_, EGL_TIMESTAMPS_ANDROID, EGL_TRUE);

  static const struct {
    EGLint egl_name;
    const char* name;
  } all_timestamps[kMaxTimestampsSupportable] = {
      {EGL_REQUESTED_PRESENT_TIME_ANDROID, "Queue"},
      {EGL_RENDERING_COMPLETE_TIME_ANDROID, "WritesDone"},
      {EGL_COMPOSITION_LATCH_TIME_ANDROID, "LatchedForDisplay"},
      {EGL_FIRST_COMPOSITION_START_TIME_ANDROID, "1stCompositeCpu"},
      {EGL_LAST_COMPOSITION_START_TIME_ANDROID, "NthCompositeCpu"},
      {EGL_FIRST_COMPOSITION_GPU_FINISHED_TIME_ANDROID, "GpuCompositeDone"},
      {EGL_DISPLAY_PRESENT_TIME_ANDROID, "ScanOutStart"},
      {EGL_DEQUEUE_READY_TIME_ANDROID, "DequeueReady"},
      {EGL_READS_DONE_TIME_ANDROID, "ReadsDone"},
  };

  supported_egl_timestamps_.reserve(kMaxTimestampsSupportable);
  supported_event_names_.reserve(kMaxTimestampsSupportable);
  for (const auto& ts : all_timestamps) {
    if (!eglGetFrameTimestampSupportedANDROID(GetDisplay(), surface_,
                                              ts.egl_name))
      continue;

    // Stored in separate vectors so we can pass the egl timestamps
    // directly to the EGL functions.
    supported_egl_timestamps_.push_back(ts.egl_name);
    supported_event_names_.push_back(ts.name);
  }

  use_egl_timestamps_ = !supported_egl_timestamps_.empty();
}

bool NativeViewGLSurfaceEGL::SupportsPresentationCallback() {
  return true;
}

bool NativeViewGLSurfaceEGL::InitializeNativeWindow() {
  return true;
}

void NativeViewGLSurfaceEGL::Destroy() {
  presentation_helper_ = nullptr;
  vsync_provider_internal_ = nullptr;

  if (surface_) {
    if (!eglDestroySurface(GetDisplay(), surface_)) {
      LOG(ERROR) << "eglDestroySurface failed with error "
                 << GetLastEGLErrorString();
    }
    surface_ = NULL;
  }
}

bool NativeViewGLSurfaceEGL::IsOffscreen() {
  return false;
}

gfx::SwapResult NativeViewGLSurfaceEGL::SwapBuffers(
    const PresentationCallback& callback) {
  TRACE_EVENT2("gpu", "NativeViewGLSurfaceEGL:RealSwapBuffers",
      "width", GetSize().width(),
      "height", GetSize().height());

  if (!CommitAndClearPendingOverlays()) {
    DVLOG(1) << "Failed to commit pending overlay planes.";
    return gfx::SwapResult::SWAP_FAILED;
  }

  EGLuint64KHR newFrameId = 0;
  bool newFrameIdIsValid = true;
  if (use_egl_timestamps_) {
    newFrameIdIsValid =
        !!eglGetNextFrameIdANDROID(GetDisplay(), surface_, &newFrameId);
  }

  GLSurfacePresentationHelper::ScopedSwapBuffers scoped_swap_buffers(
      presentation_helper_.get(), callback);
  if (!eglSwapBuffers(GetDisplay(), surface_)) {
    DVLOG(1) << "eglSwapBuffers failed with error "
             << GetLastEGLErrorString();
    scoped_swap_buffers.set_result(gfx::SwapResult::SWAP_FAILED);
  } else if (use_egl_timestamps_) {
    UpdateSwapEvents(newFrameId, newFrameIdIsValid);
  }
  return scoped_swap_buffers.result();
}

void NativeViewGLSurfaceEGL::UpdateSwapEvents(EGLuint64KHR newFrameId,
                                              bool newFrameIdIsValid) {
  // Queue info for the frame just swapped.
  swap_info_queue_.push({newFrameIdIsValid, newFrameId});

  // Make sure we have a frame old enough that all it's timstamps should
  // be available by now.
  constexpr int kFramesAgoToGetServerTimestamps = 4;
  if (swap_info_queue_.size() <= kFramesAgoToGetServerTimestamps)
    return;

  // TraceEvents if needed.
  // If we weren't able to get a valid frame id before the swap, we can't get
  // its timestamps now.
  const SwapInfo& old_swap_info = swap_info_queue_.front();
  if (old_swap_info.frame_id_is_valid && g_trace_swap_enabled.Get().value)
    TraceSwapEvents(old_swap_info.frame_id);

  swap_info_queue_.pop();
}

void NativeViewGLSurfaceEGL::TraceSwapEvents(EGLuint64KHR oldFrameId) {
  // We shouldn't be calling eglGetFrameTimestampsANDROID with more timestamps
  // than it supports.
  DCHECK_LE(supported_egl_timestamps_.size(), kMaxTimestampsSupportable);

  // Get the timestamps.
  std::vector<EGLnsecsANDROID> egl_timestamps(supported_egl_timestamps_.size(),
                                              EGL_TIMESTAMP_INVALID_ANDROID);
  if (!eglGetFrameTimestampsANDROID(
          GetDisplay(), surface_, oldFrameId,
          static_cast<EGLint>(supported_egl_timestamps_.size()),
          supported_egl_timestamps_.data(), egl_timestamps.data())) {
    TRACE_EVENT_INSTANT0("gpu", "eglGetFrameTimestamps:Failed",
                         TRACE_EVENT_SCOPE_THREAD);
    return;
  }

  // Track supported and valid time/name pairs.
  struct TimeNamePair {
    base::TimeTicks time;
    const char* name;
  };

  std::vector<TimeNamePair> tracePairs;
  tracePairs.reserve(supported_egl_timestamps_.size());
  for (size_t i = 0; i < egl_timestamps.size(); i++) {
    // Although a timestamp of 0 is technically valid, we shouldn't expect to
    // see it in practice. 0's are more likely due to a known linux kernel bug
    // that inadvertently discards timestamp information when merging two
    // retired fences.
    if (egl_timestamps[i] == 0 ||
        egl_timestamps[i] == EGL_TIMESTAMP_INVALID_ANDROID ||
        egl_timestamps[i] == EGL_TIMESTAMP_PENDING_ANDROID) {
      continue;
    }
    // TODO(brianderson): Replace FromInternalValue usage.
    tracePairs.push_back(
        {base::TimeTicks::FromInternalValue(
             egl_timestamps[i] / base::TimeTicks::kNanosecondsPerMicrosecond),
         supported_event_names_[i]});
  }
  if (tracePairs.empty()) {
    TRACE_EVENT_INSTANT0("gpu", "TraceSwapEvents:NoValidTimestamps",
                         TRACE_EVENT_SCOPE_THREAD);
    return;
  }

  // Sort the pairs so we can trace them in order.
  std::sort(tracePairs.begin(), tracePairs.end(),
            [](auto& a, auto& b) { return a.time < b.time; });

  // Trace the overall range under which the sub events will be nested.
  // Add an epsilon since the trace viewer interprets timestamp ranges
  // as closed on the left and open on the right. i.e.: [begin, end).
  // The last sub event isn't nested properly without the epsilon.
  auto epsilon = base::TimeDelta::FromMicroseconds(1);
  static const char* SwapEvents = "SwapEvents";
  const int64_t trace_id = oldFrameId;
  TRACE_EVENT_NESTABLE_ASYNC_BEGIN_WITH_TIMESTAMP0(
      kSwapEventTraceCategories, SwapEvents, trace_id, tracePairs.front().time);
  TRACE_EVENT_NESTABLE_ASYNC_END_WITH_TIMESTAMP1(
      kSwapEventTraceCategories, SwapEvents, trace_id,
      tracePairs.back().time + epsilon, "id", trace_id);

  // Trace the first event, which does not have a range before it.
  TRACE_EVENT_NESTABLE_ASYNC_INSTANT_WITH_TIMESTAMP0(
      kSwapEventTraceCategories, tracePairs.front().name, trace_id,
      tracePairs.front().time);

  // Trace remaining events and their ranges.
  // Use the first characters to represent events still pending.
  // This helps color code the remaining events in the viewer, which makes
  // it obvious:
  //   1) when the order of events are different between frames and
  //   2) if multiple events occurred very close together.
  std::string valid_symbols(tracePairs.size(), '\0');
  for (size_t i = 0; i < valid_symbols.size(); i++)
    valid_symbols[i] = tracePairs[i].name[0];

  const char* pending_symbols = valid_symbols.c_str();
  for (size_t i = 1; i < tracePairs.size(); i++) {
    pending_symbols++;
    TRACE_EVENT_COPY_NESTABLE_ASYNC_BEGIN_WITH_TIMESTAMP0(
        kSwapEventTraceCategories, pending_symbols, trace_id,
        tracePairs[i - 1].time);
    TRACE_EVENT_COPY_NESTABLE_ASYNC_END_WITH_TIMESTAMP0(
        kSwapEventTraceCategories, pending_symbols, trace_id,
        tracePairs[i].time);
    TRACE_EVENT_NESTABLE_ASYNC_INSTANT_WITH_TIMESTAMP0(
        kSwapEventTraceCategories, tracePairs[i].name, trace_id,
        tracePairs[i].time);
  }
}

gfx::Size NativeViewGLSurfaceEGL::GetSize() {
  EGLint width;
  EGLint height;
  if (!eglQuerySurface(GetDisplay(), surface_, EGL_WIDTH, &width) ||
      !eglQuerySurface(GetDisplay(), surface_, EGL_HEIGHT, &height)) {
    NOTREACHED() << "eglQuerySurface failed with error "
                 << GetLastEGLErrorString();
    return gfx::Size();
  }

  return gfx::Size(width, height);
}

bool NativeViewGLSurfaceEGL::Resize(const gfx::Size& size,
                                    float scale_factor,
                                    ColorSpace color_space,
                                    bool has_alpha) {
  if (size == GetSize())
    return true;

  size_ = size;

  std::unique_ptr<ui::ScopedMakeCurrent> scoped_make_current;
  GLContext* current_context = GLContext::GetCurrent();
  bool was_current =
      current_context && current_context->IsCurrent(this);
  if (was_current) {
    scoped_make_current.reset(
        new ui::ScopedMakeCurrent(current_context, this));
    current_context->ReleaseCurrent(this);
  }

  Destroy();

  if (!Initialize(format_)) {
    LOG(ERROR) << "Failed to resize window.";
    return false;
  }

  return true;
}

bool NativeViewGLSurfaceEGL::Recreate() {
  Destroy();
  if (!Initialize(format_)) {
    LOG(ERROR) << "Failed to create surface.";
    return false;
  }
  return true;
}

EGLSurface NativeViewGLSurfaceEGL::GetHandle() {
  return surface_;
}

bool NativeViewGLSurfaceEGL::SupportsPostSubBuffer() {
  return supports_post_sub_buffer_;
}

bool NativeViewGLSurfaceEGL::FlipsVertically() const {
  return flips_vertically_;
}

bool NativeViewGLSurfaceEGL::BuffersFlipped() const {
  return g_use_direct_composition;
}

gfx::SwapResult NativeViewGLSurfaceEGL::SwapBuffersWithDamage(
    const std::vector<int>& rects,
    const PresentationCallback& callback) {
  DCHECK(supports_swap_buffer_with_damage_);
  if (!CommitAndClearPendingOverlays()) {
    DVLOG(1) << "Failed to commit pending overlay planes.";
    return gfx::SwapResult::SWAP_FAILED;
  }

  GLSurfacePresentationHelper::ScopedSwapBuffers scoped_swap_buffers(
      presentation_helper_.get(), callback);
  if (!eglSwapBuffersWithDamageKHR(GetDisplay(), surface_,
                                   const_cast<EGLint*>(rects.data()),
                                   static_cast<EGLint>(rects.size() / 4))) {
    DVLOG(1) << "eglSwapBuffersWithDamageKHR failed with error "
             << GetLastEGLErrorString();
    scoped_swap_buffers.set_result(gfx::SwapResult::SWAP_FAILED);
  }
  return scoped_swap_buffers.result();
}

gfx::SwapResult NativeViewGLSurfaceEGL::PostSubBuffer(
    int x,
    int y,
    int width,
    int height,
    const PresentationCallback& callback) {
  DCHECK(supports_post_sub_buffer_);
  if (!CommitAndClearPendingOverlays()) {
    DVLOG(1) << "Failed to commit pending overlay planes.";
    return gfx::SwapResult::SWAP_FAILED;
  }
  if (flips_vertically_) {
    // With EGL_SURFACE_ORIENTATION_INVERT_Y_ANGLE the contents are rendered
    // inverted, but the PostSubBuffer rectangle is still measured from the
    // bottom left.
    y = GetSize().height() - y - height;
  }

  GLSurfacePresentationHelper::ScopedSwapBuffers scoped_swap_buffers(
      presentation_helper_.get(), callback);
  if (!eglPostSubBufferNV(GetDisplay(), surface_, x, y, width, height)) {
    DVLOG(1) << "eglPostSubBufferNV failed with error "
             << GetLastEGLErrorString();
    scoped_swap_buffers.set_result(gfx::SwapResult::SWAP_FAILED);
  }
  return scoped_swap_buffers.result();
}

bool NativeViewGLSurfaceEGL::SupportsCommitOverlayPlanes() {
#if defined(OS_ANDROID)
  return true;
#else
  return false;
#endif
}

gfx::SwapResult NativeViewGLSurfaceEGL::CommitOverlayPlanes(
    const PresentationCallback& callback) {
  DCHECK(SupportsCommitOverlayPlanes());
  // Here we assume that the overlays scheduled on this surface will display
  // themselves to the screen right away in |CommitAndClearPendingOverlays|,
  // rather than being queued and waiting for a "swap" signal.
  GLSurfacePresentationHelper::ScopedSwapBuffers scoped_swap_buffers(
      presentation_helper_.get(), callback);
  if (!CommitAndClearPendingOverlays())
    scoped_swap_buffers.set_result(gfx::SwapResult::SWAP_FAILED);
  return scoped_swap_buffers.result();
}

bool NativeViewGLSurfaceEGL::OnMakeCurrent(GLContext* context) {
  if (presentation_helper_)
    presentation_helper_->OnMakeCurrent(context, this);
  return GLSurfaceEGL::OnMakeCurrent(context);
}

gfx::VSyncProvider* NativeViewGLSurfaceEGL::GetVSyncProvider() {
  return vsync_provider_external_ ? vsync_provider_external_.get()
                                  : vsync_provider_internal_.get();
}

bool NativeViewGLSurfaceEGL::ScheduleOverlayPlane(
    int z_order,
    gfx::OverlayTransform transform,
    GLImage* image,
    const gfx::Rect& bounds_rect,
    const gfx::RectF& crop_rect) {
#if !defined(OS_ANDROID)
  NOTIMPLEMENTED();
  return false;
#else
  pending_overlays_.push_back(
      GLSurfaceOverlay(z_order, transform, image, bounds_rect, crop_rect));
  return true;
#endif
}

NativeViewGLSurfaceEGL::~NativeViewGLSurfaceEGL() {
  Destroy();
#if defined(OS_ANDROID)
  if (window_)
    ANativeWindow_release(window_);
#endif
}

bool NativeViewGLSurfaceEGL::CommitAndClearPendingOverlays() {
  if (pending_overlays_.empty())
    return true;

  bool success = true;
#if defined(OS_ANDROID)
  for (const auto& overlay : pending_overlays_)
    success &= overlay.ScheduleOverlayPlane(window_);
  pending_overlays_.clear();
#else
  NOTIMPLEMENTED();
#endif
  return success;
}

PbufferGLSurfaceEGL::PbufferGLSurfaceEGL(const gfx::Size& size)
    : size_(size),
      surface_(NULL) {
  // Some implementations of Pbuffer do not support having a 0 size. For such
  // cases use a (1, 1) surface.
  if (size_.GetArea() == 0)
    size_.SetSize(1, 1);
}

bool PbufferGLSurfaceEGL::Initialize(GLSurfaceFormat format) {
  EGLSurface old_surface = surface_;

#if defined(OS_ANDROID)
  // This is to allow context virtualization which requires on- and offscreen
  // to use a compatible config. We expect the client to request RGB565
  // onscreen surface also for this to work (with the exception of
  // fullscreen video).
  if (base::SysInfo::AmountOfPhysicalMemoryMB() <= 512)
    format.SetRGB565();
#endif

  format_ = format;

  EGLDisplay display = GetDisplay();
  if (!display) {
    LOG(ERROR) << "Trying to create surface with invalid display.";
    return false;
  }

  // Allocate the new pbuffer surface before freeing the old one to ensure
  // they have different addresses. If they have the same address then a
  // future call to MakeCurrent might early out because it appears the current
  // context and surface have not changed.
  std::vector<EGLint> pbuffer_attribs;
  pbuffer_attribs.push_back(EGL_WIDTH);
  pbuffer_attribs.push_back(size_.width());
  pbuffer_attribs.push_back(EGL_HEIGHT);
  pbuffer_attribs.push_back(size_.height());

  if (g_use_direct_composition) {
    pbuffer_attribs.push_back(
        EGL_FLEXIBLE_SURFACE_COMPATIBILITY_SUPPORTED_ANGLE);
    pbuffer_attribs.push_back(EGL_TRUE);
  }

  pbuffer_attribs.push_back(EGL_NONE);

  EGLSurface new_surface =
      eglCreatePbufferSurface(display, GetConfig(), &pbuffer_attribs[0]);
  if (!new_surface) {
    LOG(ERROR) << "eglCreatePbufferSurface failed with error "
               << GetLastEGLErrorString();
    return false;
  }

  if (old_surface)
    eglDestroySurface(display, old_surface);

  surface_ = new_surface;
  return true;
}

void PbufferGLSurfaceEGL::Destroy() {
  if (surface_) {
    if (!eglDestroySurface(GetDisplay(), surface_)) {
      LOG(ERROR) << "eglDestroySurface failed with error "
                 << GetLastEGLErrorString();
    }
    surface_ = NULL;
  }
}

bool PbufferGLSurfaceEGL::IsOffscreen() {
  return true;
}

gfx::SwapResult PbufferGLSurfaceEGL::SwapBuffers(
    const PresentationCallback& callback) {
  NOTREACHED() << "Attempted to call SwapBuffers on a PbufferGLSurfaceEGL.";
  return gfx::SwapResult::SWAP_FAILED;
}

gfx::Size PbufferGLSurfaceEGL::GetSize() {
  return size_;
}

bool PbufferGLSurfaceEGL::Resize(const gfx::Size& size,
                                 float scale_factor,
                                 ColorSpace color_space,
                                 bool has_alpha) {
  if (size == size_)
    return true;

  std::unique_ptr<ui::ScopedMakeCurrent> scoped_make_current;
  GLContext* current_context = GLContext::GetCurrent();
  bool was_current =
      current_context && current_context->IsCurrent(this);
  if (was_current) {
    scoped_make_current.reset(
        new ui::ScopedMakeCurrent(current_context, this));
  }

  size_ = size;

  if (!Initialize(format_)) {
    LOG(ERROR) << "Failed to resize pbuffer.";
    return false;
  }

  return true;
}

EGLSurface PbufferGLSurfaceEGL::GetHandle() {
  return surface_;
}

void* PbufferGLSurfaceEGL::GetShareHandle() {
#if defined(OS_ANDROID)
  NOTREACHED();
  return NULL;
#else
  if (!g_driver_egl.ext.b_EGL_ANGLE_query_surface_pointer)
    return NULL;

  if (!g_driver_egl.ext.b_EGL_ANGLE_surface_d3d_texture_2d_share_handle)
    return NULL;

  void* handle;
  if (!eglQuerySurfacePointerANGLE(g_egl_display, GetHandle(),
                                   EGL_D3D_TEXTURE_2D_SHARE_HANDLE_ANGLE,
                                   &handle)) {
    return NULL;
  }

  return handle;
#endif
}

PbufferGLSurfaceEGL::~PbufferGLSurfaceEGL() {
  Destroy();
}

SurfacelessEGL::SurfacelessEGL(const gfx::Size& size) : size_(size) {}

bool SurfacelessEGL::Initialize(GLSurfaceFormat format) {
  format_ = format;
  return true;
}

void SurfacelessEGL::Destroy() {
}

bool SurfacelessEGL::IsOffscreen() {
  return true;
}

bool SurfacelessEGL::IsSurfaceless() const {
  return true;
}

gfx::SwapResult SurfacelessEGL::SwapBuffers(
    const PresentationCallback& callback) {
  LOG(ERROR) << "Attempted to call SwapBuffers with SurfacelessEGL.";
  return gfx::SwapResult::SWAP_FAILED;
}

gfx::Size SurfacelessEGL::GetSize() {
  return size_;
}

bool SurfacelessEGL::Resize(const gfx::Size& size,
                            float scale_factor,
                            ColorSpace color_space,
                            bool has_alpha) {
  size_ = size;
  return true;
}

EGLSurface SurfacelessEGL::GetHandle() {
  return EGL_NO_SURFACE;
}

void* SurfacelessEGL::GetShareHandle() {
  return NULL;
}

SurfacelessEGL::~SurfacelessEGL() {
}

}  // namespace gl
