// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gpu/skia_bindings/gles2_implementation_with_grcontext_support.h"

#include "gpu/skia_bindings/grcontext_for_gles2_interface.h"
#include "third_party/khronos/GLES2/gl2ext.h"
#include "third_party/skia/include/gpu/GrContext.h"

namespace skia_bindings {

GLES2ImplementationWithGrContextSupport::
    GLES2ImplementationWithGrContextSupport(
        gpu::gles2::GLES2CmdHelper* helper,
        scoped_refptr<gpu::gles2::ShareGroup> share_group,
        gpu::TransferBufferInterface* transfer_buffer,
        bool bind_generates_resource,
        bool lose_context_when_out_of_memory,
        bool support_client_side_arrays,
        gpu::GpuControl* gpu_control)
    : GLES2Implementation(helper,
                          std::move(share_group),
                          transfer_buffer,
                          bind_generates_resource,
                          lose_context_when_out_of_memory,
                          support_client_side_arrays,
                          gpu_control) {}

GLES2ImplementationWithGrContextSupport::
    ~GLES2ImplementationWithGrContextSupport() {}

bool GLES2ImplementationWithGrContextSupport::HasGrContextSupport() const {
  return true;
}

void GLES2ImplementationWithGrContextSupport::ResetGrContextIfNeeded(
    uint32_t dirty_bits) {
  if (gr_context_ && !using_gl_from_skia_) {
    gr_context_->resetContext(dirty_bits);
  }
}

void GLES2ImplementationWithGrContextSupport::SetGrContext(GrContext* gr) {
  DCHECK(!gr || !gr_context_);  // Cant have multiple linked GrContexts
  gr_context_ = gr;
}

void GLES2ImplementationWithGrContextSupport::WillCallGLFromSkia() {
  using_gl_from_skia_ = true;
}

void GLES2ImplementationWithGrContextSupport::DidCallGLFromSkia() {
  using_gl_from_skia_ = false;
}

// Calls that invalidate kRenderTarget_GrGLBackendState
void GLES2ImplementationWithGrContextSupport::BindFramebuffer(
    GLenum target,
    GLuint framebuffer) {
  BaseClass::BindFramebuffer(target, framebuffer);
  ResetGrContextIfNeeded(kRenderTarget_GrGLBackendState);
}

// Calls that invalidate kTextureBinding_GrGLBackendState
void GLES2ImplementationWithGrContextSupport::BindTexture(GLenum target,
                                                          GLuint texture) {
  BaseClass::BindTexture(target, texture);
  ResetGrContextIfNeeded(kTextureBinding_GrGLBackendState);
}
void GLES2ImplementationWithGrContextSupport::UnlockDiscardableTextureCHROMIUM(
    GLuint texture) {
  BaseClass::UnlockDiscardableTextureCHROMIUM(texture);
  ResetGrContextIfNeeded(kTextureBinding_GrGLBackendState);
}
bool GLES2ImplementationWithGrContextSupport::LockDiscardableTextureCHROMIUM(
    GLuint texture) {
  bool result = BaseClass::LockDiscardableTextureCHROMIUM(texture);
  ResetGrContextIfNeeded(kTextureBinding_GrGLBackendState);
  return result;
}
void GLES2ImplementationWithGrContextSupport::DeleteTextures(
    GLsizei n,
    const GLuint* textures) {
  BaseClass::DeleteTextures(n, textures);
  ResetGrContextIfNeeded(kTextureBinding_GrGLBackendState);
}
void GLES2ImplementationWithGrContextSupport::ActiveTexture(GLenum texture) {
  BaseClass::ActiveTexture(texture);
  ResetGrContextIfNeeded(kTextureBinding_GrGLBackendState);
}

// Calls that invalidate kView_GrGLBackendState
void GLES2ImplementationWithGrContextSupport::Scissor(GLint x,
                                                      GLint y,
                                                      GLsizei width,
                                                      GLsizei height) {
  BaseClass::Scissor(x, y, width, height);
  ResetGrContextIfNeeded(kView_GrGLBackendState);
}
void GLES2ImplementationWithGrContextSupport::Viewport(GLint x,
                                                       GLint y,
                                                       GLsizei width,
                                                       GLsizei height) {
  BaseClass::Viewport(x, y, width, height);
  ResetGrContextIfNeeded(kView_GrGLBackendState);
}
void GLES2ImplementationWithGrContextSupport::WindowRectanglesEXT(
    GLenum mode,
    GLsizei count,
    const GLint* box) {
  BaseClass::WindowRectanglesEXT(mode, count, box);
  ResetGrContextIfNeeded(kView_GrGLBackendState);
}

// Calls that invalidate kBlend_GrGLBackendState
void GLES2ImplementationWithGrContextSupport::BlendColor(GLclampf red,
                                                         GLclampf green,
                                                         GLclampf blue,
                                                         GLclampf alpha) {
  BaseClass::BlendColor(red, green, blue, alpha);
  ResetGrContextIfNeeded(kBlend_GrGLBackendState);
}
void GLES2ImplementationWithGrContextSupport::BlendEquation(GLenum mode) {
  BaseClass::BlendEquation(mode);
  ResetGrContextIfNeeded(kBlend_GrGLBackendState);
}
void GLES2ImplementationWithGrContextSupport::BlendEquationSeparate(
    GLenum modeRGB,
    GLenum modeAlpha) {
  BaseClass::BlendEquationSeparate(modeRGB, modeAlpha);
  ResetGrContextIfNeeded(kBlend_GrGLBackendState);
}
void GLES2ImplementationWithGrContextSupport::BlendFunc(GLenum sfactor,
                                                        GLenum dfactor) {
  BaseClass::BlendFunc(sfactor, dfactor);
  ResetGrContextIfNeeded(kBlend_GrGLBackendState);
}
void GLES2ImplementationWithGrContextSupport::BlendFuncSeparate(
    GLenum srcRGB,
    GLenum dstRGB,
    GLenum srcAlpha,
    GLenum dstAlpha) {
  BaseClass::BlendFuncSeparate(srcRGB, dstRGB, srcAlpha, dstAlpha);
  ResetGrContextIfNeeded(kBlend_GrGLBackendState);
}

// Calls that invalidate kMSAAEnable_GrGLBackendState
void GLES2ImplementationWithGrContextSupport::CoverageModulationCHROMIUM(
    GLenum components) {
  BaseClass::CoverageModulationCHROMIUM(components);
  ResetGrContextIfNeeded(kMSAAEnable_GrGLBackendState);
}

// Calls that invalidate kVertex_GrGLBackendState
void GLES2ImplementationWithGrContextSupport::BindVertexArrayOES(GLuint array) {
  BaseClass::BindVertexArrayOES(array);
  ResetGrContextIfNeeded(kVertex_GrGLBackendState);
}

// Calls that invalidate kStencil_GrGLBackendState
void GLES2ImplementationWithGrContextSupport::StencilFunc(GLenum func,
                                                          GLint ref,
                                                          GLuint mask) {
  BaseClass::StencilFunc(func, ref, mask);
  ResetGrContextIfNeeded(kStencil_GrGLBackendState);
}
void GLES2ImplementationWithGrContextSupport::StencilFuncSeparate(GLenum face,
                                                                  GLenum func,
                                                                  GLint ref,
                                                                  GLuint mask) {
  BaseClass::StencilFuncSeparate(face, func, ref, mask);
  ResetGrContextIfNeeded(kStencil_GrGLBackendState);
}
void GLES2ImplementationWithGrContextSupport::StencilMask(GLuint mask) {
  BaseClass::StencilMask(mask);
  ResetGrContextIfNeeded(kStencil_GrGLBackendState);
}
void GLES2ImplementationWithGrContextSupport::StencilMaskSeparate(GLenum face,
                                                                  GLuint mask) {
  BaseClass::StencilMaskSeparate(face, mask);
  ResetGrContextIfNeeded(kStencil_GrGLBackendState);
}
void GLES2ImplementationWithGrContextSupport::StencilOp(GLenum fail,
                                                        GLenum zfail,
                                                        GLenum zpass) {
  BaseClass::StencilOp(fail, zfail, zpass);
  ResetGrContextIfNeeded(kStencil_GrGLBackendState);
}
void GLES2ImplementationWithGrContextSupport::StencilOpSeparate(GLenum face,
                                                                GLenum fail,
                                                                GLenum zfail,
                                                                GLenum zpass) {
  BaseClass::StencilOpSeparate(face, fail, zfail, zpass);
  ResetGrContextIfNeeded(kStencil_GrGLBackendState);
}

// Calls that invalidate kPixelStore_GrGLBackendState
void GLES2ImplementationWithGrContextSupport::PixelStorei(GLenum pname,
                                                          GLint param) {
  BaseClass::PixelStorei(pname, param);
  ResetGrContextIfNeeded(kPixelStore_GrGLBackendState);
}

// Calls that invalidate kProgram_GrGLBackendState
void GLES2ImplementationWithGrContextSupport::UseProgram(GLuint program) {
  BaseClass::UseProgram(program);
  ResetGrContextIfNeeded(kProgram_GrGLBackendState);
}

// Calls that invalidate kMisc_GrGLBackendState
void GLES2ImplementationWithGrContextSupport::DepthMask(GLboolean flag) {
  BaseClass::DepthMask(flag);
  ResetGrContextIfNeeded(kMisc_GrGLBackendState);
}
void GLES2ImplementationWithGrContextSupport::FrontFace(GLenum mode) {
  BaseClass::FrontFace(mode);
  ResetGrContextIfNeeded(kMisc_GrGLBackendState);
}
void GLES2ImplementationWithGrContextSupport::LineWidth(GLfloat width) {
  BaseClass::LineWidth(width);
  ResetGrContextIfNeeded(kMisc_GrGLBackendState);
}
void GLES2ImplementationWithGrContextSupport::ColorMask(GLboolean red,
                                                        GLboolean green,
                                                        GLboolean blue,
                                                        GLboolean alpha) {
  BaseClass::ColorMask(red, green, blue, alpha);
  ResetGrContextIfNeeded(kMisc_GrGLBackendState);
}

// Calls that invalidate kPathRendering_GrGLBackendState
void GLES2ImplementationWithGrContextSupport::PathStencilFuncCHROMIUM(
    GLenum func,
    GLint ref,
    GLuint mask) {
  BaseClass::PathStencilFuncCHROMIUM(func, ref, mask);
  ResetGrContextIfNeeded(kPathRendering_GrGLBackendState);
}
void GLES2ImplementationWithGrContextSupport::MatrixLoadfCHROMIUM(
    GLenum matrixMode,
    const GLfloat* m) {
  BaseClass::MatrixLoadfCHROMIUM(matrixMode, m);
  ResetGrContextIfNeeded(kPathRendering_GrGLBackendState);
}

// Calls that invalidate many flags
void GLES2ImplementationWithGrContextSupport::BindBuffer(GLenum target,
                                                         GLuint buffer) {
  WillBindBuffer(target);
  BaseClass::BindBuffer(target, buffer);
}
void GLES2ImplementationWithGrContextSupport::BindBufferBase(GLenum target,
                                                             GLuint index,
                                                             GLuint buffer) {
  WillBindBuffer(target);
  BaseClass::BindBufferBase(target, index, buffer);
}
void GLES2ImplementationWithGrContextSupport::BindBufferRange(GLenum target,
                                                              GLuint index,
                                                              GLuint buffer,
                                                              GLintptr offset,
                                                              GLsizeiptr size) {
  WillBindBuffer(target);
  BaseClass::BindBufferRange(target, index, buffer, offset, size);
}
void GLES2ImplementationWithGrContextSupport::WillBindBuffer(GLenum target) {
  switch (target) {
    case GL_ELEMENT_ARRAY_BUFFER:
    case GL_ARRAY_BUFFER:
      ResetGrContextIfNeeded(kVertex_GrGLBackendState);
      break;
    case GL_TEXTURE_BUFFER_OES:
    //  case GL_DRAW_INDIRECT_BUFFER:
    case GL_PIXEL_UNPACK_BUFFER:
    case GL_PIXEL_PACK_BUFFER:
      ResetGrContextIfNeeded(kMisc_GrGLBackendState);
      break;
    default:
      break;
  }
}

void GLES2ImplementationWithGrContextSupport::Disable(GLenum cap) {
  WillEnableOrDisable(cap);
  BaseClass::Disable(cap);
}

void GLES2ImplementationWithGrContextSupport::Enable(GLenum cap) {
  WillEnableOrDisable(cap);
  BaseClass::Enable(cap);
}

void GLES2ImplementationWithGrContextSupport::WillEnableOrDisable(GLenum cap) {
  switch (cap) {
    case GL_FRAMEBUFFER_SRGB_EXT:
      ResetGrContextIfNeeded(kRenderTarget_GrGLBackendState);
      break;
    case GL_SCISSOR_TEST:
      ResetGrContextIfNeeded(kView_GrGLBackendState);
      break;
    case GL_BLEND:
      ResetGrContextIfNeeded(kBlend_GrGLBackendState);
      break;
    case GL_MULTISAMPLE_EXT:
      ResetGrContextIfNeeded(kMSAAEnable_GrGLBackendState);
      break;
    case GL_STENCIL_TEST:
      ResetGrContextIfNeeded(kStencil_GrGLBackendState);
      break;
    case GL_DEPTH_TEST:
    case GL_CULL_FACE:
    case GL_DITHER:
    //  Commented-out non-ES bits that skia cares about
    //  case GL_POINT_SMOOTH:
    //  case GL_LINE_SMOOTH:
    //  case GL_POLYGON_SMOOTH:
    //  case GL_POLYGON_STIPPLE:
    //  case GL_COLOR_LOGIC_OP:
    //  case GL_INDEX_LOGIC_OP:
    //  case GL_COLOR_TABLE:
    //  case GL_VERTEX_PROGRAM_POINT_SIZE:
    case GL_POLYGON_OFFSET_FILL:
    case GL_FETCH_PER_SAMPLE_ARM:
      ResetGrContextIfNeeded(kMisc_GrGLBackendState);
      break;
    default:
      break;
  }
}

}  // namespace skia_bindings
