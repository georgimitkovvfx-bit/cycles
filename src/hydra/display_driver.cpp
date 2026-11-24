/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#ifdef _WIN32
// Include first to avoid "NOGDI" definition set in Cycles headers
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <Windows.h>
#endif

#include "hydra/display_driver.h"
#include "hydra/render_buffer.h"
#include "hydra/session.h"

#include <epoxy/gl.h>
#include <pxr/imaging/hgiGL/texture.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

HdCyclesDisplayDriver::HdCyclesDisplayDriver(HdCyclesSession *renderParam, Hgi *hgi)
    : _renderParam(renderParam), _hgi(hgi)
{
}

HdCyclesDisplayDriver::~HdCyclesDisplayDriver()
{
  if (texture_) {
    _hgi->DestroyTexture(&texture_);
  }

  // Cycles destroys the display driver in the session thread, which has no OpenGL context
  // current. Calling OpenGL functions without one crashes, so use the shared context.
  if (gl_context_enable()) {
    if (gl_upload_sync_) {
      glDeleteSync((GLsync)gl_upload_sync_);
    }
    if (gl_render_sync_) {
      glDeleteSync((GLsync)gl_render_sync_);
    }
    if (gl_pbo_id_) {
      glDeleteBuffers(1, &gl_pbo_id_);
    }
    if (gl_small_texture_id_) {
      glDeleteTextures(1, &gl_small_texture_id_);
    }
    gl_context_disable();
  }

  gl_context_dispose();
}

void HdCyclesDisplayDriver::gl_context_create()
{
#ifdef _WIN32
  if (!gl_context_) {
    hdc_ = GetDC(CreateWindowA("STATIC",
                               "HdCycles",
                               WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                               0,
                               0,
                               64,
                               64,
                               nullptr,
                               nullptr,
                               GetModuleHandle(nullptr),
                               nullptr));

    int pixelFormat = GetPixelFormat(wglGetCurrentDC());
    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd)};
    DescribePixelFormat((HDC)hdc_, pixelFormat, sizeof(pfd), &pfd);
    SetPixelFormat((HDC)hdc_, pixelFormat, &pfd);

    TF_VERIFY(gl_context_ = wglCreateContext((HDC)hdc_));
    TF_VERIFY(wglShareLists(wglGetCurrentContext(), (HGLRC)gl_context_));
  }
  if (!gl_context_) {
    return;
  }
#endif

  if (!gl_pbo_id_) {
    glGenBuffers(1, &gl_pbo_id_);
    graphics_interop_buffer_.clear();
  }
}

bool HdCyclesDisplayDriver::gl_context_enable()
{
#ifdef _WIN32
  if (!hdc_ || !gl_context_) {
    return false;
  }

  mutex_.lock();

  // Do not change context if this is called in the main thread
  if (wglGetCurrentContext() == nullptr) {
    if (!TF_VERIFY(wglMakeCurrent((HDC)hdc_, (HGLRC)gl_context_))) {
      mutex_.unlock();
      return false;
    }
  }

  return true;
#else
  return false;
#endif
}

void HdCyclesDisplayDriver::gl_context_disable()
{
#ifdef _WIN32
  if (wglGetCurrentContext() == gl_context_) {
    TF_VERIFY(wglMakeCurrent(nullptr, nullptr));
  }

  mutex_.unlock();
#endif
}

void HdCyclesDisplayDriver::gl_context_dispose()
{
#ifdef _WIN32
  if (gl_context_) {
    TF_VERIFY(wglDeleteContext((HGLRC)gl_context_));
    DestroyWindow(WindowFromDC((HDC)hdc_));
  }
#endif
}

void HdCyclesDisplayDriver::next_tile_begin() {}

bool HdCyclesDisplayDriver::update_needed()
{
  // No color AOV bound (dome lights, other first AOV): the output driver writes the AOVs.
  // No context before the first draw: update_begin() would fail and log an error.
#ifdef _WIN32
  return gl_context_ && _renderParam->GetDisplayAovBinding().renderBuffer != nullptr;
#else
  return false; /* No GL context outside Windows (gl_context_enable): the output driver shows the render. */
#endif
}

bool HdCyclesDisplayDriver::update_begin(const Params &params,
                                         const int texture_width,
                                         const int texture_height)
{
  if (!gl_context_enable()) {
    return false;
  }

  texture_size_ = make_int2(texture_width, texture_height);

  if (gl_render_sync_) {
    glWaitSync((GLsync)gl_render_sync_, 0, GL_TIMEOUT_IGNORED);
  }

  if (pbo_size_.x != params.full_size.x || pbo_size_.y != params.full_size.y) {
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, gl_pbo_id_);
    glBufferData(GL_PIXEL_UNPACK_BUFFER,
                 sizeof(half4) * params.full_size.x * params.full_size.y,
                 nullptr,
                 GL_DYNAMIC_DRAW);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    pbo_size_ = params.full_size;
    graphics_interop_buffer_.clear();
  }

  need_update_ = true;

  return true;
}

void HdCyclesDisplayDriver::update_end()
{
  if (gl_upload_sync_) {
    glDeleteSync((GLsync)gl_upload_sync_);
  }
  gl_upload_sync_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  glFlush();

  gl_context_disable();
}

void HdCyclesDisplayDriver::flush()
{
  if (!gl_context_enable()) {
    return;
  }

  if (gl_upload_sync_) {
    glWaitSync((GLsync)gl_upload_sync_, 0, GL_TIMEOUT_IGNORED);
  }

  if (gl_render_sync_) {
    glWaitSync((GLsync)gl_render_sync_, 0, GL_TIMEOUT_IGNORED);
  }

  gl_context_disable();
}

half4 *HdCyclesDisplayDriver::map_texture_buffer()
{
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, gl_pbo_id_);

  auto *const mapped_rgba_pixels = static_cast<half4 *>(
      glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY));

  if (need_zero_ && mapped_rgba_pixels) {
    memset(mapped_rgba_pixels, 0, sizeof(half4) * pbo_size_.x * pbo_size_.y);
    need_zero_ = false;
  }

  return mapped_rgba_pixels;
}

void HdCyclesDisplayDriver::unmap_texture_buffer()
{
  glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);

  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}

GraphicsInteropDevice HdCyclesDisplayDriver::graphics_interop_get_device()
{
  GraphicsInteropDevice interop_device;
  interop_device.type = GraphicsInteropDevice::OPENGL;
  return interop_device;
}

void HdCyclesDisplayDriver::graphics_interop_update_buffer()
{
  if (graphics_interop_buffer_.is_empty()) {
    graphics_interop_buffer_.assign(
        GraphicsInteropDevice::OPENGL, gl_pbo_id_, pbo_size_.x * pbo_size_.y * sizeof(half4));
  }

  if (need_zero_) {
    graphics_interop_buffer_.zero();
    need_zero_ = false;
  }
}

void HdCyclesDisplayDriver::graphics_interop_activate()
{
  gl_context_enable();
}

void HdCyclesDisplayDriver::graphics_interop_deactivate()
{
  gl_context_disable();
}

void HdCyclesDisplayDriver::zero()
{
  need_zero_ = true;
}

void HdCyclesDisplayDriver::draw(const Params &params)
{
  auto *const renderBuffer = static_cast<HdCyclesRenderBuffer *>(
      _renderParam->GetDisplayAovBinding().renderBuffer);
  if (!renderBuffer ||  // Ensure this render buffer matches the texture dimensions
      (renderBuffer->GetWidth() != params.size.x || renderBuffer->GetHeight() != params.size.y))
  {
    return;
  }

  if (!renderBuffer->IsResourceUsed()) {
    return;
  }

  gl_context_create();

  // Cycles 'DisplayDriver' only supports 'half4' format
  TF_VERIFY(renderBuffer->GetFormat() == HdFormatFloat16Vec4);

  const thread_scoped_lock lock(mutex_);

  const GfVec3i dimensions(params.size.x, params.size.y, 1);
  if (!texture_ || texture_->GetDescriptor().dimensions != dimensions) {
    if (texture_) {
      _hgi->DestroyTexture(&texture_);
    }

    HgiTextureDesc texDesc;
    texDesc.usage = 0;
    texDesc.format = HgiFormatFloat16Vec4;
    texDesc.type = HgiTextureType2D;
    texDesc.dimensions = dimensions;
    texDesc.sampleCount = HgiSampleCount1;

    texture_ = _hgi->CreateTexture(texDesc);
    renderBuffer->SetResource(VtValue());  // Empty until the first upload below
  }

  HgiGLTexture *const texture = dynamic_cast<HgiGLTexture *>(texture_.Get());
  if (!texture || !need_update_ || pbo_size_.x != params.size.x || pbo_size_.y != params.size.y) {
    return;
  }

  if (gl_upload_sync_) {
    glWaitSync((GLsync)gl_upload_sync_, 0, GL_TIMEOUT_IGNORED);
  }

  /* This runs in Houdini's context while it draws the viewport: restore what is changed here. */
  GLint texture_binding = 0;
  GLint unpack_buffer_binding = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture_binding);
  glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buffer_binding);

  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, gl_pbo_id_);
  if (texture_size_ == params.size) {
    glBindTexture(GL_TEXTURE_2D, texture->GetTextureId());
    glTexSubImage2D(
        GL_TEXTURE_2D, 0, 0, 0, pbo_size_.x, pbo_size_.y, GL_RGBA, GL_HALF_FLOAT, nullptr);
  }
  else {
    /* Viewport renders start at a lower resolution, packed at the start of the PBO. Upload it to
     * a texture of its size, then scale it up to the display texture, with nearest filtering
     * like Blender. */
    if (!gl_small_texture_id_) {
      glGenTextures(1, &gl_small_texture_id_);
    }
    glBindTexture(GL_TEXTURE_2D, gl_small_texture_id_);
    if (!(small_texture_size_ == texture_size_)) {
      glTexImage2D(GL_TEXTURE_2D,
                   0,
                   GL_RGBA16F,
                   texture_size_.x,
                   texture_size_.y,
                   0,
                   GL_RGBA,
                   GL_HALF_FLOAT,
                   nullptr);
      small_texture_size_ = texture_size_;
    }
    glTexSubImage2D(
        GL_TEXTURE_2D, 0, 0, 0, texture_size_.x, texture_size_.y, GL_RGBA, GL_HALF_FLOAT, nullptr);

    GLint read_framebuffer = 0;
    GLint draw_framebuffer = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_framebuffer);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_framebuffer);
    const GLboolean scissor_test = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean color_mask[4];
    glGetBooleani_v(GL_COLOR_WRITEMASK, 0, color_mask);

    /* Framebuffers are not shared between contexts, so they live only during this draw. */
    GLuint framebuffers[2];
    glGenFramebuffers(2, framebuffers);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffers[0]);
    glFramebufferTexture2D(
        GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl_small_texture_id_, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffers[1]);
    glFramebufferTexture2D(
        GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture->GetTextureId(), 0);

    /* The scissor test and color mask apply to blits. */
    glDisable(GL_SCISSOR_TEST);
    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBlitFramebuffer(0,
                      0,
                      texture_size_.x,
                      texture_size_.y,
                      0,
                      0,
                      params.size.x,
                      params.size.y,
                      GL_COLOR_BUFFER_BIT,
                      GL_NEAREST);

    glColorMaski(0, color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
    if (scissor_test) {
      glEnable(GL_SCISSOR_TEST);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read_framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_framebuffer);
    glDeleteFramebuffers(2, framebuffers);
  }
  glBindTexture(GL_TEXTURE_2D, texture_binding);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpack_buffer_binding);

  if (gl_render_sync_) {
    glDeleteSync((GLsync)gl_render_sync_);
  }
  gl_render_sync_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  glFlush();

  need_update_ = false;

  /* Houdini shows the texture once it holds an image, until then the pixels the output driver
   * writes. Set again after the render pass gave the buffer back to the output driver. */
  renderBuffer->SetResource(VtValue(texture_));
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
