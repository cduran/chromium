// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gpu/command_buffer/client/gles2_implementation.h"

#ifndef GPU_SKIA_BINDINGS_GLES2_IMPLEMENTATION_WITH_GRCONTEXT_SUPPORT_H_
#define GPU_SKIA_BINDINGS_GLES2_IMPLEMENTATION_WITH_GRCONTEXT_SUPPORT_H_

class GrContext;

namespace skia_bindings {

class GLES2ImplementationWithGrContextSupport
    : public gpu::gles2::GLES2Implementation {
 public:
  GLES2ImplementationWithGrContextSupport(
      gpu::gles2::GLES2CmdHelper* helper,
      scoped_refptr<gpu::gles2::ShareGroup> share_group,
      gpu::TransferBufferInterface* transfer_buffer,
      bool bind_generates_resource,
      bool lose_context_when_out_of_memory,
      bool support_client_side_arrays,
      gpu::GpuControl* gpu_control);

  ~GLES2ImplementationWithGrContextSupport() override;

  typedef gpu::gles2::GLES2Implementation BaseClass;

  void WillCallGLFromSkia() override;
  void DidCallGLFromSkia() override;
  void SetGrContext(GrContext* gr) override;
  bool HasGrContextSupport() const override;

  // Overrides for GLES2 calls that invalidate state that is tracked by skia
  //=========================================================================
  //
  // These must be kept in sync with the invalidation defines in
  // GrGLGpu::onResetContext()

  // Calls that invalidate kRenderTarget_GrGLBackendState
  void BindFramebuffer(GLenum target, GLuint framebuffer) override;

  // Calls that invalidate kTextureBinding_GrGLBackendState
  // Note: Deleting a texture may affect the binding if the currently bound
  // texture is deleted. Locking and Unlocking discardable textures may
  // internally invoke texture deletion, so they too may affect the texture
  // binding.
  void BindTexture(GLenum target, GLuint texture) override;
  void UnlockDiscardableTextureCHROMIUM(GLuint texture) override;
  bool LockDiscardableTextureCHROMIUM(GLuint texture) override;
  void DeleteTextures(GLsizei n, const GLuint* textures) override;
  void ActiveTexture(GLenum texture) override;

  // Calls that invalidate kView_GrGLBackendState
  void Scissor(GLint x, GLint y, GLsizei width, GLsizei height) override;
  void Viewport(GLint x, GLint y, GLsizei width, GLsizei height) override;
  void WindowRectanglesEXT(GLenum mode,
                           GLsizei count,
                           const GLint* box) override;

  // Calls that invalidate kBlend_GrGLBackendState
  void BlendColor(GLclampf red,
                  GLclampf green,
                  GLclampf blue,
                  GLclampf alpha) override;
  void BlendEquation(GLenum mode) override;
  void BlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha) override;
  void BlendFunc(GLenum sfactor, GLenum dfactor) override;
  void BlendFuncSeparate(GLenum srcRGB,
                         GLenum dstRGB,
                         GLenum srcAlpha,
                         GLenum dstAlpha) override;

  // Calls that invalidate kMSAAEnable_GrGLBackendState
  void CoverageModulationCHROMIUM(GLenum components) override;

  // Calls that invalidate kVertex_GrGLBackendState
  void BindVertexArrayOES(GLuint array) override;

  // Calls that invalidate kStencil_GrGLBackendState
  void StencilFunc(GLenum func, GLint ref, GLuint mask) override;
  void StencilFuncSeparate(GLenum face,
                           GLenum func,
                           GLint ref,
                           GLuint mask) override;
  void StencilMask(GLuint mask) override;
  void StencilMaskSeparate(GLenum face, GLuint mask) override;
  void StencilOp(GLenum fail, GLenum zfail, GLenum zpass) override;
  void StencilOpSeparate(GLenum face,
                         GLenum fail,
                         GLenum zfail,
                         GLenum zpass) override;

  // Calls that invalidate kPixelStore_GrGLBackendState
  void PixelStorei(GLenum pname, GLint param) override;

  // Calls that invalidate kProgram_GrGLBackendState
  void UseProgram(GLuint program) override;

  // Calls that invalidate kMisc_GrGLBackendState
  void DepthMask(GLboolean flag) override;
  void FrontFace(GLenum mode) override;
  void LineWidth(GLfloat width) override;
  void ColorMask(GLboolean red,
                 GLboolean green,
                 GLboolean blue,
                 GLboolean alpha) override;

  // Calls that invalidate kPathRendering_GrGLBackendState
  void PathStencilFuncCHROMIUM(GLenum func, GLint ref, GLuint mask) override;
  void MatrixLoadfCHROMIUM(GLenum matrixMode, const GLfloat* m) override;
  // Note: MatrixLoadIdentity omitted on purpose

  // Calls that invalidate different bits, depending on args
  void BindBuffer(GLenum target, GLuint buffer) override;
  void BindBufferBase(GLenum target, GLuint index, GLuint buffer) override;
  void BindBufferRange(GLenum target,
                       GLuint index,
                       GLuint buffer,
                       GLintptr offset,
                       GLsizeiptr size) override;
  void Disable(GLenum cap) override;
  void Enable(GLenum cap) override;

 private:
  void WillBindBuffer(GLenum target);
  void WillEnableOrDisable(GLenum cap);
  void ResetGrContextIfNeeded(uint32_t dirty_bits);

  GrContext* gr_context_ = nullptr;
  bool using_gl_from_skia_ = false;
};

}  // namespace skia_bindings

#endif
