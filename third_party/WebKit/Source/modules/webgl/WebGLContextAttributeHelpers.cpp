// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "modules/webgl/WebGLContextAttributeHelpers.h"
#include "modules/xr/XRDevice.h"

#include "core/frame/Settings.h"

namespace blink {

WebGLContextAttributes ToWebGLContextAttributes(
    const CanvasContextCreationAttributesCore& attrs) {
  WebGLContextAttributes result;
  result.setAlpha(attrs.alpha);
  result.setDepth(attrs.depth);
  result.setStencil(attrs.stencil);
  result.setAntialias(attrs.antialias);
  result.setPremultipliedAlpha(attrs.premultiplied_alpha);
  result.setPreserveDrawingBuffer(attrs.preserve_drawing_buffer);
  result.setFailIfMajorPerformanceCaveat(
      attrs.fail_if_major_performance_caveat);
  result.setCompatibleXRDevice(
      static_cast<XRDevice*>(attrs.compatible_xr_device.Get()));
  return result;
}

Platform::ContextAttributes ToPlatformContextAttributes(
    const CanvasContextCreationAttributesCore& attrs,
    unsigned web_gl_version,
    bool support_own_offscreen_surface) {
  Platform::ContextAttributes result;
  result.fail_if_major_performance_caveat =
      attrs.fail_if_major_performance_caveat;
  result.context_type = web_gl_version == 2 ? Platform::kWebGL2ContextType
                                            : Platform::kWebGL1ContextType;
  if (support_own_offscreen_surface) {
    // Only ask for alpha/depth/stencil/antialias if we may be using the default
    // framebuffer. They are not needed for standard offscreen rendering.
    result.support_alpha = attrs.alpha;
    result.support_depth = attrs.depth;
    result.support_stencil = attrs.stencil;
    result.support_antialias = attrs.antialias;
  }
  return result;
}

}  // namespace blink
