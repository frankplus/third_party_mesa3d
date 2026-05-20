/*
 * Mesa 3-D graphics library
 *
 * Copyright (c) 2021 Huawei Device Co., Ltd. *
 * Copyright (c) 2025 Diemit <598757652@qq.com>
 *
 * Based on platform_android, which has
 *
 * Copyright (C) 2010-2011 Chia-I Wu <olvaffe@gmail.com>
 * Copyright (C) 2010-2011 LunarG Inc.
 *
 * Based on platform_x11, which has
 *
 * Copyright © 2011 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <xf86drm.h>
#include <drm-uapi/drm_fourcc.h>
#include <sys/types.h>

#include "util/compiler.h"
#include "util/libsync.h"
#include "util/os_file.h"

#include "main/glconfig.h"
#include "egl_dri2.h"
#include "eglglobals.h"
#include "loader.h"
#include "loader_dri_helper.h"
#include "platform_ohos.h"
#include "dri_util.h"
#include "ohos_log.h"

static int
get_format_bpp(int native)
{
    int bpp;

    switch (native) {
        case PIXEL_FMT_RGBA_8888:
        case PIXEL_FMT_RGBX_8888:
        case PIXEL_FMT_BGRA_8888:
            bpp = 4;
            break;
        case PIXEL_FMT_RGB_565:
            bpp = 2;
            break;
        default:
            bpp = 0;
            break;
    }

    return bpp;
}

/* returns # of fds, and by reference the actual fds */
static unsigned
get_native_buffer_fds(struct ANativeWindowBuffer *buf, int fds[3])
{
    BufferHandle *handle = GetBufferHandleFromNative(buf);
    if (handle == NULL) {
        return 0;
    }

    fds[0] = handle->fd;
    return 1;
}

/* createImageFromFds requires fourcc format */
static int get_fourcc(int native)
{
    switch (native) {
        case PIXEL_FMT_RGB_565:
            return DRM_FORMAT_RGB565;
        case PIXEL_FMT_BGRA_8888:
            return DRM_FORMAT_ARGB8888;
        case PIXEL_FMT_RGBA_8888:
            return DRM_FORMAT_ABGR8888;
        case PIXEL_FMT_RGBX_8888:
            return DRM_FORMAT_XBGR8888;
        case PIXEL_FMT_BGRX_8888:
            return DRM_FORMAT_XRGB8888;
        default:
            _eglLog(_EGL_WARNING, "unsupported native buffer format 0x%{public}x", native);
    }
    return -1;
}

static int
native_window_buffer_get_buffer_info(struct dri2_egl_display *dri2_dpy,
                                     struct ANativeWindowBuffer *buf,
                                     struct u_gralloc_buffer_basic_info *out_buf_info)
{
    int num_planes = 0;
    int drm_fourcc = 0;
    int pitch = 0;
    int fds[3];
    /*
     * Non-YUV formats could *also* have multiple planes, such as ancillary
     * color compression state buffer, but the rest of the code isn't ready
     * yet to deal with modifiers:
     */
    num_planes = get_native_buffer_fds(buf, fds);
    if (num_planes == 0) {
        return -EINVAL;
    }

    assert(num_planes == 1);
    BufferHandle *bufferHandle;
    bufferHandle = GetBufferHandleFromNative(buf);
    drm_fourcc = get_fourcc(bufferHandle->format);
    if (drm_fourcc == -1) {
        _eglError(EGL_BAD_PARAMETER, "eglCreateEGLImageKHR");
        return -EINVAL;
    }
    pitch = bufferHandle->stride;
    if (pitch == 0) {
        _eglError(EGL_BAD_PARAMETER, "eglCreateEGLImageKHR");
        return -EINVAL;
    }

    *out_buf_info = (struct u_gralloc_buffer_basic_info) {
         .width = bufferHandle->width,
         .height = bufferHandle->height,
         .drm_fourcc = drm_fourcc,
         .num_planes = num_planes,
         .fds = {fds[0], -1, -1, -1},
         .modifier = DRM_FORMAT_MOD_INVALID,
         .offsets = {0, 0, 0, 0},
         .strides = {pitch, 0, 0, 0},
    };

    return 0;
}

static struct dri_image *
ohos_create_image_from_buffer_info(
   struct dri2_egl_display *dri2_dpy,
   struct u_gralloc_buffer_basic_info *buf_info,
   struct u_gralloc_buffer_color_info *color_info, void *priv)
{
   unsigned error;

   return dri2_from_dma_bufs(
      dri2_dpy->dri_screen_render_gpu, buf_info->width, buf_info->height, buf_info->drm_fourcc,
      buf_info->modifier, buf_info->fds, buf_info->num_planes,
      buf_info->strides, buf_info->offsets, color_info->yuv_color_space,
      color_info->sample_range, color_info->horizontal_siting,
      color_info->vertical_siting, 0, &error, priv);
}

static struct dri_image *
ohos_create_image_from_native_buffer(_EGLDisplay *disp,
                                      struct ANativeWindowBuffer *buf,
                                      void *priv)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct u_gralloc_buffer_basic_info buf_info;
   struct u_gralloc_buffer_color_info color_info = {
      .yuv_color_space = EGL_ITU_REC601_EXT,
      .sample_range = EGL_YUV_NARROW_RANGE_EXT,
      .horizontal_siting = EGL_YUV_CHROMA_SITING_0_EXT,
      .vertical_siting = EGL_YUV_CHROMA_SITING_0_EXT,
   };

   struct dri_image *img = NULL;

   if(native_window_buffer_get_buffer_info(dri2_dpy, buf, &buf_info))
      return 0;

   img = ohos_create_image_from_buffer_info(dri2_dpy, &buf_info, &color_info, priv);

   if (!img) {
      /* If dri driver is gallium virgl, real modifier info queried back from
       * CrOS info (and potentially mapper metadata if integrated later) cannot
       * get resolved and the buffer import will fail. Thus the fallback
       * behavior is preserved so that the buffer can be imported without
       * modifier info as a last resort.
       */
      buf_info.modifier = DRM_FORMAT_MOD_INVALID;
      img = ohos_create_image_from_buffer_info(
         dri2_dpy, &buf_info, &color_info, priv);
   }

   return img;
}

static void
handle_in_fence_fd(struct dri2_egl_surface *dri2_surf, struct dri_image *img)
{
   _EGLDisplay *disp = dri2_surf->base.Resource.Display;
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);

   if (dri2_surf->in_fence_fd < 0)
      return;

   validate_fence_fd(dri2_surf->in_fence_fd);

   if (dri2_dpy->has_native_fence_fd) {
      dri2_set_in_fence_fd(img, dri2_surf->in_fence_fd);
   } else {
      sync_wait(dri2_surf->in_fence_fd, -1);
   }
}

static void
close_in_fence_fd(struct dri2_egl_surface *dri2_surf)
{
   validate_fence_fd(dri2_surf->in_fence_fd);
   if (dri2_surf->in_fence_fd >= 0)
      close(dri2_surf->in_fence_fd);
   dri2_surf->in_fence_fd = -1;
}

static EGLBoolean
ohos_window_dequeue_buffer(struct dri2_egl_surface *dri2_surf)
{
   int fence_fd;

   if (ANativeWindow_dequeueBuffer(dri2_surf->window, &dri2_surf->buffer,
                                   &fence_fd))
      return EGL_FALSE;

   close_in_fence_fd(dri2_surf);

   validate_fence_fd(fence_fd);

   dri2_surf->in_fence_fd = fence_fd;

   /* Record all the buffers created by ANativeWindow and update back buffer
    * for updating buffer's age in swap_buffers.
    */
   EGLBoolean updated = EGL_FALSE;
   for (int i = 0; i < dri2_surf->color_buffers_count; i++) {
      if (!dri2_surf->color_buffers[i].buffer) {
         dri2_surf->color_buffers[i].buffer = dri2_surf->buffer;
      }
      if (dri2_surf->color_buffers[i].buffer == dri2_surf->buffer) {
         dri2_surf->back = &dri2_surf->color_buffers[i];
         updated = EGL_TRUE;
         break;
      }
   }

   if (!updated) {
      /* In case of all the buffers were recreated by ANativeWindow, reset
       * the color_buffers
       */
      for (int i = 0; i < dri2_surf->color_buffers_count; i++) {
         dri2_surf->color_buffers[i].buffer = NULL;
         dri2_surf->color_buffers[i].age = 0;
      }
      dri2_surf->color_buffers[0].buffer = dri2_surf->buffer;
      dri2_surf->back = &dri2_surf->color_buffers[0];
   }

   return EGL_TRUE;
}

static EGLBoolean
ohos_window_enqueue_buffer(_EGLDisplay *disp,
                            struct dri2_egl_surface *dri2_surf)
{
   /* Queue the buffer with stored out fence fd. The ANativeWindow or buffer
    * consumer may choose to wait for the fence to signal before accessing
    * it. If fence fd value is -1, buffer can be accessed by consumer
    * immediately. Consumer or application shouldn't rely on timestamp
    * associated with fence if the fence fd is -1.
    *
    * Ownership of fd is transferred to consumer after queueBuffer and the
    * consumer is responsible for closing it. Caller must not use the fd
    * after passing it to queueBuffer.
    */
   int fence_fd = dri2_surf->out_fence_fd;
   dri2_surf->out_fence_fd = -1;
   ANativeWindow_queueBuffer(dri2_surf->window, dri2_surf->buffer, fence_fd);

   dri2_surf->buffer = NULL;
   dri2_surf->back = NULL;

   if (dri2_surf->dri_image_back) {
      dri2_destroy_image(dri2_surf->dri_image_back);
      dri2_surf->dri_image_back = NULL;
   }

   return EGL_TRUE;
}

static void
ohos_window_cancel_buffer(struct dri2_egl_surface *dri2_surf)
{
   int ret;
   int fence_fd = dri2_surf->out_fence_fd;

   dri2_surf->out_fence_fd = -1;
   ret = ANativeWindow_cancelBuffer(dri2_surf->window, dri2_surf->buffer,
                                    fence_fd);
   dri2_surf->buffer = NULL;
   if (ret < 0) {
      _eglLog(_EGL_WARNING, "ANativeWindow_cancelBuffer failed");
      dri2_surf->base.Lost = EGL_TRUE;
   }

   close_in_fence_fd(dri2_surf);
}

static _EGLSurface *
ohos_create_surface(_EGLDisplay *disp, EGLint type, _EGLConfig *conf,
                     void *native_window, const EGLint *attrib_list)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_config *dri2_conf = dri2_egl_config(conf);
   struct dri2_egl_surface *dri2_surf;
   struct ANativeWindow *window = native_window;
   const struct dri_config *config;

   dri2_surf = calloc(1, sizeof *dri2_surf);
   if (!dri2_surf) {
      _eglError(EGL_BAD_ALLOC, "ohos_create_surface");
      return NULL;
   }

   dri2_surf->in_fence_fd = -1;

   if (!dri2_init_surface(&dri2_surf->base, disp, type, conf, attrib_list, true,
                          native_window))
      goto cleanup_surface;

   if (type == EGL_WINDOW_BIT) {
      int format;
      int buffer_count;
      int min_undequeued_buffers;

      format = ANativeWindow_getFormat(window);
      if (format < 0) {
         _eglError(EGL_BAD_NATIVE_WINDOW, "ohos_create_surface");
         goto cleanup_surface;
      }

      /* Required buffer caching slots. */
      buffer_count = 3; // default use 3 buffer

      dri2_surf->color_buffers =
         calloc(buffer_count, sizeof(*dri2_surf->color_buffers));
      if (!dri2_surf->color_buffers) {
         _eglError(EGL_BAD_ALLOC, "ohos_create_surface");
         goto cleanup_surface;
      }
      dri2_surf->color_buffers_count = buffer_count;

      if (format != dri2_conf->base.NativeVisualID) {
         _eglLog(_EGL_WARNING, "Native format mismatch: 0x%x != 0x%x", format,
                 dri2_conf->base.NativeVisualID);
      }

      NativeWindowHandleOpt(window, GET_BUFFER_GEOMETRY, &dri2_surf->base.Height, &dri2_surf->base.Width);
   }

   config = dri2_get_dri_config(dri2_conf, type, dri2_surf->base.GLColorspace);
   if (!config) {
      _eglError(EGL_BAD_MATCH,
                "Unsupported surfacetype/colorspace configuration");
      goto cleanup_surface;
   }

   if (!dri2_create_drawable(dri2_dpy, config, dri2_surf, dri2_surf))
      goto cleanup_surface;

   if (window) {
      ANativeWindow_acquire(window);
      dri2_surf->window = window;
   }

   return &dri2_surf->base;

cleanup_surface:
   if (dri2_surf->color_buffers_count)
      free(dri2_surf->color_buffers);
   free(dri2_surf);

   return NULL;
}

static _EGLSurface *
ohos_create_window_surface(_EGLDisplay *disp, _EGLConfig *conf,
                            void *native_window, const EGLint *attrib_list)
{
   return ohos_create_surface(disp, EGL_WINDOW_BIT, conf, native_window,
                               attrib_list);
}

static _EGLSurface *
ohos_create_pbuffer_surface(_EGLDisplay *disp, _EGLConfig *conf,
                             const EGLint *attrib_list)
{
   return ohos_create_surface(disp, EGL_PBUFFER_BIT, conf, NULL, attrib_list);
}

static EGLBoolean
ohos_destroy_surface(_EGLDisplay *disp, _EGLSurface *surf)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);

   if (dri2_surf->base.Type == EGL_WINDOW_BIT) {
      if (dri2_surf->buffer)
         ohos_window_cancel_buffer(dri2_surf);

      ANativeWindow_release(dri2_surf->window);
   }

   if (dri2_surf->dri_image_back) {
      _eglLog(_EGL_DEBUG, "%s : %d : destroy dri_image_back", __func__,
              __LINE__);
      dri2_destroy_image(dri2_surf->dri_image_back);
      dri2_surf->dri_image_back = NULL;
   }

   if (dri2_surf->dri_image_front) {
      _eglLog(_EGL_DEBUG, "%s : %d : destroy dri_image_front", __func__,
              __LINE__);
      dri2_destroy_image(dri2_surf->dri_image_front);
      dri2_surf->dri_image_front = NULL;
   }

   driDestroyDrawable(dri2_surf->dri_drawable);

   close_in_fence_fd(dri2_surf);
   dri2_fini_surface(surf);
   free(dri2_surf->color_buffers);
   free(dri2_surf);

   return EGL_TRUE;
}

static int
update_buffers(struct dri2_egl_surface *dri2_surf)
{
   if (dri2_surf->base.Lost)
      return -1;

   if (dri2_surf->base.Type != EGL_WINDOW_BIT)
      return 0;

   /* try to dequeue the next back buffer */
   if (!dri2_surf->buffer && !ohos_window_dequeue_buffer(dri2_surf)) {
      _eglLog(_EGL_WARNING, "Could not dequeue buffer from native window");
      dri2_surf->base.Lost = EGL_TRUE;
      return -1;
   }
   BufferHandle *handle = GetBufferHandleFromNative(dri2_surf->buffer);
   /* free outdated buffers and update the surface size */
   if (dri2_surf->base.Width != handle->width ||
       dri2_surf->base.Height != handle->height) {
      dri2_surf->base.Width = handle->width;
      dri2_surf->base.Height = handle->height;
   }

   return 0;
}

static int
get_front_bo(struct dri2_egl_surface *dri2_surf, unsigned int format)
{
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);

   if (dri2_surf->dri_image_front)
      return 0;

   if (dri2_surf->base.Type == EGL_WINDOW_BIT) {
      /* According current EGL spec, front buffer rendering
       * for window surface is not supported now.
       * and mesa doesn't have the implementation of this case.
       * Add warning message, but not treat it as error.
       */
      _eglLog(
         _EGL_DEBUG,
         "DRI driver requested unsupported front buffer for window surface");
   } else if (dri2_surf->base.Type == EGL_PBUFFER_BIT) {
      dri2_surf->dri_image_front = dri_create_image(
         dri2_dpy->dri_screen_render_gpu, dri2_surf->base.Width,
         dri2_surf->base.Height, format, NULL, 0, 0, NULL);
      if (!dri2_surf->dri_image_front) {
         _eglLog(_EGL_WARNING, "dri2_image_front allocation failed");
         return -1;
      }
   }

   return 0;
}

static int
get_back_bo(struct dri2_egl_surface *dri2_surf)
{
   _EGLDisplay *disp = dri2_surf->base.Resource.Display;

   if (dri2_surf->dri_image_back)
      return 0;

   if (dri2_surf->base.Type == EGL_WINDOW_BIT) {
      if (!dri2_surf->buffer) {
         _eglLog(_EGL_WARNING, "Could not get native buffer");
         return -1;
      }

      dri2_surf->dri_image_back =
         ohos_create_image_from_native_buffer(disp, dri2_surf->buffer, NULL);
      if (!dri2_surf->dri_image_back) {
         _eglLog(_EGL_WARNING, "failed to create DRI image from FD");
         return -1;
      }

      handle_in_fence_fd(dri2_surf, dri2_surf->dri_image_back);

   } else if (dri2_surf->base.Type == EGL_PBUFFER_BIT) {
      /* The EGL 1.5 spec states that pbuffers are single-buffered.
       * Specifically, the spec states that they have a back buffer but no front
       * buffer, in contrast to pixmaps, which have a front buffer but no back
       * buffer.
       *
       * Single-buffered surfaces with no front buffer confuse Mesa; so we
       * deviate from the spec, following the precedent of Mesa's EGL X11
       * platform. The X11 platform correctly assigns pbuffers to
       * single-buffered configs, but assigns the pbuffer a front buffer instead
       * of a back buffer.
       *
       * Pbuffers in the X11 platform mostly work today, so let's just copy its
       * behavior instead of trying to fix (and hence potentially breaking) the
       * world.
       */
      _eglLog(
         _EGL_DEBUG,
         "DRI driver requested unsupported back buffer for pbuffer surface");
   }

   return 0;
}

/* Some drivers will pass multiple bits in buffer_mask.
 * For such case, will go through all the bits, and
 * will not return error when unsupported buffer is requested, only
 * return error when the allocation for supported buffer failed.
 */
static int
ohos_image_get_buffers(struct dri_drawable *driDrawable, unsigned int format,
                        uint32_t *stamp, void *loaderPrivate,
                        uint32_t buffer_mask, struct __DRIimageList *images)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;

   images->image_mask = 0;
   images->front = NULL;
   images->back = NULL;

   if (update_buffers(dri2_surf) < 0)
      return 0;

   if (_eglSurfaceInSharedBufferMode(&dri2_surf->base)) {
      if (get_back_bo(dri2_surf) < 0)
         return 0;

      /* We have dri_image_back because this is a window surface and
       * get_back_bo() succeeded.
       */
      assert(dri2_surf->dri_image_back);
      images->back = dri2_surf->dri_image_back;
      images->image_mask |= __DRI_IMAGE_BUFFER_SHARED;

      /* There exists no accompanying back nor front buffer. */
      return 1;
   }

   if (buffer_mask & __DRI_IMAGE_BUFFER_FRONT) {
      if (get_front_bo(dri2_surf, format) < 0)
         return 0;

      if (dri2_surf->dri_image_front) {
         images->front = dri2_surf->dri_image_front;
         images->image_mask |= __DRI_IMAGE_BUFFER_FRONT;
      }
   }

   if (buffer_mask & __DRI_IMAGE_BUFFER_BACK) {
      if (get_back_bo(dri2_surf) < 0)
         return 0;

      if (dri2_surf->dri_image_back) {
         images->back = dri2_surf->dri_image_back;
         images->image_mask |= __DRI_IMAGE_BUFFER_BACK;
      }
   }

   return 1;
}

static EGLint
ohos_query_buffer_age(_EGLDisplay *disp, _EGLSurface *surface)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surface);

   if (update_buffers(dri2_surf) < 0) {
      _eglError(EGL_BAD_ALLOC, "ohos_query_buffer_age");
      return -1;
   }

   return dri2_surf->back ? dri2_surf->back->age : 0;
}

static EGLBoolean
ohos_swap_buffers(_EGLDisplay *disp, _EGLSurface *draw)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(draw);
   const bool has_mutable_rb = _eglSurfaceHasMutableRenderBuffer(draw);

   /* From the EGL_KHR_mutable_render_buffer spec (v12):
    *
    *    If surface is a single-buffered window, pixmap, or pbuffer surface
    *    for which there is no pending change to the EGL_RENDER_BUFFER
    *    attribute, eglSwapBuffers has no effect.
    */
   if (has_mutable_rb && draw->RequestedRenderBuffer == EGL_SINGLE_BUFFER &&
       draw->ActiveRenderBuffer == EGL_SINGLE_BUFFER) {
      _eglLog(_EGL_DEBUG, "%s: remain in shared buffer mode", __func__);
      return EGL_TRUE;
   }

   for (int i = 0; i < dri2_surf->color_buffers_count; i++) {
      if (dri2_surf->color_buffers[i].age > 0)
         dri2_surf->color_buffers[i].age++;
   }

   /* "XXX: we don't use get_back_bo() since it causes regressions in
    * several dEQP tests.
    */
   if (dri2_surf->back)
      dri2_surf->back->age = 1;

   dri2_flush_drawable_for_swapbuffers_flags(disp, draw,
                                             __DRI2_NOTHROTTLE_SWAPBUFFER);

   if (dri2_dpy->pure_swrast) {
      driSwapBuffers(dri2_surf->dri_drawable);
      if (dri2_surf->buffer)
         ohos_window_enqueue_buffer(disp, dri2_surf);
   } else {
      /* dri2_surf->buffer can be null even when no error has occurred. For
       * example, if the user has called no GL rendering commands since the
       * previous eglSwapBuffers, then the driver may have not triggered
       * a callback to ANativeWindow_dequeueBuffer, in which case
       * dri2_surf->buffer remains null.
       */
      if (dri2_surf->buffer)
         ohos_window_enqueue_buffer(disp, dri2_surf);

      dri_invalidate_drawable(dri2_surf->dri_drawable);
   }

   return EGL_TRUE;
}

static EGLBoolean
ohos_query_surface(_EGLDisplay *disp, _EGLSurface *surf, EGLint attribute,
                    EGLint *value)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);
   EGLint dummy;
   switch (attribute) {
   case EGL_WIDTH:
      if (dri2_surf->base.Type == EGL_WINDOW_BIT && dri2_surf->window) {
         NativeWindowHandleOpt(dri2_surf->window,
                             GET_BUFFER_GEOMETRY, &dummy, value);
         return EGL_TRUE;
      }
      break;
   case EGL_HEIGHT:
      if (dri2_surf->base.Type == EGL_WINDOW_BIT && dri2_surf->window) {
         NativeWindowHandleOpt(dri2_surf->window,
                             GET_BUFFER_GEOMETRY, &dummy, value);
         return EGL_TRUE;
      }
      break;
   default:
      break;
   }
   return _eglQuerySurface(disp, surf, attribute, value);
}

static _EGLImage *
dri2_create_image_ohos_native_buffer(_EGLDisplay *disp, _EGLContext *ctx,
                                        struct ANativeWindowBuffer *buf)
{
   if (ctx != NULL) {
      /* From the EGL_OHOS_image_native_buffer spec:
       *
       *     * If <target> is EGL_NATIVE_BUFFER_OHOS and <ctx> is not
       *       EGL_NO_CONTEXT, the error EGL_BAD_CONTEXT is generated.
       */
      _eglError(EGL_BAD_CONTEXT,
                "eglCreateEGLImageKHR: for "
                "EGL_NATIVE_BUFFER_OHOS, the context must be "
                "EGL_NO_CONTEXT");
      return NULL;
   }

   struct dri_image *dri_image =
      ohos_create_image_from_native_buffer(disp, buf, buf);

   if (dri_image) {
      return dri2_create_image_from_dri(disp, dri_image);
   }

   return NULL;
}

static _EGLImage *
ohos_create_image_khr(_EGLDisplay *disp, _EGLContext *ctx, EGLenum target,
                       EGLClientBuffer buffer, const EGLint *attr_list)
{
   switch (target) {
   case EGL_NATIVE_BUFFER_OHOS:
      return dri2_create_image_ohos_native_buffer(
         disp, ctx, (struct ANativeWindowBuffer *)buffer);
   default:
      return dri2_create_image_khr(disp, ctx, target, buffer, attr_list);
   }
}

static void
ohos_flush_front_buffer(struct dri_drawable *driDrawable, void *loaderPrivate)
{
}

static unsigned
ohos_get_capability(void *loaderPrivate, enum dri_loader_cap cap)
{
   /* Note: loaderPrivate is _EGLDisplay* */
   switch (cap) {
   case DRI_LOADER_CAP_RGBA_ORDERING:
      return 1;
   default:
      return 0;
   }
}

static void
ohos_destroy_loader_image_state(void *loaderPrivate)
{
}

static void
ohos_add_configs_for_visuals(_EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   static const struct {
      int hal_format;
      enum pipe_format pipe_format;
   } visuals[] = {
      {PIXEL_FMT_RGBA_8888, PIPE_FORMAT_RGBA8888_UNORM},
      {PIXEL_FMT_RGBX_8888, PIPE_FORMAT_RGBX8888_UNORM},
      {PIXEL_FMT_RGB_565, PIPE_FORMAT_B5G6R5_UNORM},
      /* This must be after HAL_PIXEL_FORMAT_RGBA_8888, we only keep BGRA
       * visual if it turns out RGBA visual is not available.
       */
      {PIXEL_FMT_BGRA_8888, PIPE_FORMAT_BGRA8888_UNORM},
   };

   unsigned int format_count[ARRAY_SIZE(visuals)] = {0};

   bool has_rgba = false;
   for (int i = 0; i < ARRAY_SIZE(visuals); i++) {
      if (visuals[i].hal_format == PIXEL_FMT_BGRA_8888 && has_rgba)
         continue;
      for (int j = 0; dri2_dpy->driver_configs[j]; j++) {
         const struct gl_config *gl_config =
            (struct gl_config *) dri2_dpy->driver_configs[j];

         /* Rather than have duplicate table entries for _SRGB formats, just
          * use the linear version of the format for the comparision:
          */
         enum pipe_format linear_format =
            util_format_linear(gl_config->color_format);
         if (linear_format != visuals[i].pipe_format)
            continue;

         const EGLint surface_type = EGL_WINDOW_BIT | EGL_PBUFFER_BIT;
         const EGLint config_attrs[] = {
            EGL_NATIVE_VISUAL_ID,
            visuals[i].hal_format,
            EGL_NATIVE_VISUAL_TYPE,
            visuals[i].hal_format,
            EGL_NONE,
         };

         struct dri2_egl_config *dri2_conf = dri2_add_config(
            disp, dri2_dpy->driver_configs[j], surface_type, config_attrs);
         if (dri2_conf)
            format_count[i]++;
      }

      if (visuals[i].hal_format == PIXEL_FMT_RGBA_8888 && format_count[i])
         has_rgba = true;
   }

   for (int i = 0; i < ARRAY_SIZE(format_count); i++) {
      if (!format_count[i]) {
         _eglLog(_EGL_DEBUG, "No DRI config supports native format 0x%x",
                 visuals[i].hal_format);
      }
   }
}

static const struct dri2_egl_display_vtbl ohos_display_vtbl = {
   .authenticate = NULL,
   .create_window_surface = ohos_create_window_surface,
   .create_pbuffer_surface = ohos_create_pbuffer_surface,
   .destroy_surface = ohos_destroy_surface,
   .create_image = ohos_create_image_khr,
   .swap_buffers = ohos_swap_buffers,
   .swap_interval = NULL,
   .query_buffer_age = ohos_query_buffer_age,
   .query_surface = ohos_query_surface,
   .get_dri_drawable = dri2_surface_get_dri_drawable,
};

static const __DRIimageLoaderExtension ohos_image_loader_extension = {
   .base = {__DRI_IMAGE_LOADER, 4},

   .getBuffers = ohos_image_get_buffers,
   .flushFrontBuffer = ohos_flush_front_buffer,
   .getCapability = ohos_get_capability,
   .flushSwapBuffers = NULL,
   .destroyLoaderImageState = ohos_destroy_loader_image_state,
};

static void
ohos_display_shared_buffer(struct dri_drawable *driDrawable, int fence_fd,
                            void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;
   struct ANativeWindowBuffer *old_buffer UNUSED = dri2_surf->buffer;

   if (!_eglSurfaceInSharedBufferMode(&dri2_surf->base)) {
      _eglLog(_EGL_WARNING, "%s: internal error: buffer is not shared",
              __func__);
      return;
   }

   if (fence_fd >= 0) {
      /* The driver's fence is more recent than the surface's out fence, if it
       * exists at all. So use the driver's fence.
       */
      if (dri2_surf->out_fence_fd >= 0) {
         close(dri2_surf->out_fence_fd);
         dri2_surf->out_fence_fd = -1;
      }
   } else if (dri2_surf->out_fence_fd >= 0) {
      fence_fd = dri2_surf->out_fence_fd;
      dri2_surf->out_fence_fd = -1;
   }

   if (ANativeWindow_queueBuffer(dri2_surf->window, dri2_surf->buffer,
                                 fence_fd)) {
      _eglLog(_EGL_WARNING, "%s: ANativeWindow_queueBuffer failed", __func__);
      close(fence_fd);
      return;
   }

   fence_fd = -1;

   if (ANativeWindow_dequeueBuffer(dri2_surf->window, &dri2_surf->buffer,
                                   &fence_fd)) {
      /* Tear down the surface because it no longer has a back buffer. */
      _eglLog(_EGL_WARNING, "%s: ANativeWindow_dequeueBuffer failed", __func__);

      dri2_surf->base.Lost = true;
      dri2_surf->buffer = NULL;
      dri2_surf->back = NULL;

      if (dri2_surf->dri_image_back) {
         dri2_destroy_image(dri2_surf->dri_image_back);
         dri2_surf->dri_image_back = NULL;
      }

      dri_invalidate_drawable(dri2_surf->dri_drawable);
      return;
   }

   if (fence_fd < 0) {
        return;
    }

    /* Access to the buffer is controlled by a sync fence. Block on it.
     *
     * Ideally, we would submit the fence to the driver, and the driver would
     * postpone command execution until it signalled. But DRI lacks API for
     * that (as of 2018-04-11).
     *
     *  SYNC_IOC_WAIT waits forever if timeout < 0
     */
    sync_wait(fence_fd, -1);
    close(fence_fd);
}

static void
ohos_swrast_get_drawable_info(struct dri_drawable *drawable,
	        int *x, int *y, int *width, int *height,
	        void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;

   update_buffers(dri2_surf);

   *x = 0;
   *y = 0;
   *width = dri2_surf->base.Width;
   *height = dri2_surf->base.Height;
}

static void
ohos_swrast_put_image2(struct dri_drawable *draw, int op, int x, int y, int w,
                          int h, int stride, char *data, void *loaderPrivate)
{
   return;
}

static void
ohos_swrast_put_image(struct dri_drawable *draw, int op, int x, int y, int w,
                         int h, char *data, void *loaderPrivate)
{
   return;
}

static void
ohos_swrast_get_image(struct dri_drawable *read, int x, int y, int w, int h,
                         char *data, void *loaderPrivate)
{
   return;
}

static const __DRIswrastLoaderExtension swrast_loader_extension = {
   .base = {__DRI_SWRAST_LOADER, 2},

   .getDrawableInfo = ohos_swrast_get_drawable_info,
   .putImage = ohos_swrast_put_image,
   .getImage = ohos_swrast_get_image,
   .putImage2 = ohos_swrast_put_image2,
};

static const __DRImutableRenderBufferLoaderExtension
   ohos_mutable_render_buffer_extension = {
      .base = {__DRI_MUTABLE_RENDER_BUFFER_LOADER, 1},
      .displaySharedBuffer = ohos_display_shared_buffer,
};

static const __DRIextension *ohos_image_loader_extensions[] = {
   &ohos_image_loader_extension.base,
   &image_lookup_extension.base,
   &use_invalidate.base,
   &ohos_mutable_render_buffer_extension.base,
   NULL,
};

static const __DRIextension *ohos_swrast_image_loader_extensions[] = {
   &ohos_image_loader_extension.base,
   &image_lookup_extension.base,
   &use_invalidate.base,
   &ohos_mutable_render_buffer_extension.base,
   &swrast_loader_extension.base,
   NULL,
};

static EGLBoolean
ohos_load_driver(_EGLDisplay *disp, bool swrast)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);

   if (disp->Options.Zink)
      dri2_dpy->driver_name = strdup("zink");
   else
      dri2_dpy->driver_name = loader_get_driver_for_fd(dri2_dpy->fd_render_gpu);
   if (dri2_dpy->driver_name == NULL)
      return false;

   if (swrast && !disp->Options.Zink) {
      /* Use kms swrast only with vgem / virtio_gpu.
       * virtio-gpu fallbacks to software rendering when 3D features
       * are unavailable since 6c5ab.
       */
      if (strcmp(dri2_dpy->driver_name, "vgem") == 0 ||
          strcmp(dri2_dpy->driver_name, "virtio_gpu") == 0) {
         free(dri2_dpy->driver_name);
         dri2_dpy->driver_name = strdup("kms_swrast");
      } else {
         goto error;
      }
   }

   dri2_dpy->loader_extensions = ohos_image_loader_extensions;
   if (!dri2_load_driver(disp)) {
      goto error;
   }

   return true;

error:
   free(dri2_dpy->driver_name);
   dri2_dpy->driver_name = NULL;
   return false;
}

static void
ohos_unload_driver(_EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);

   free(dri2_dpy->driver_name);
   dri2_dpy->driver_name = NULL;
}

static int
ohos_filter_device(_EGLDisplay *disp, int fd, const char *vendor)
{
   drmVersionPtr ver = drmGetVersion(fd);
   if (!ver)
      return -1;

   if (strcmp(vendor, ver->name) != 0) {
      drmFreeVersion(ver);
      return -1;
   }

   drmFreeVersion(ver);
   return 0;
}

static EGLBoolean
ohos_probe_device(_EGLDisplay *disp, bool swrast)
{
   /* Check that the device is supported, by attempting to:
    * - load the dri module
    * - and, create a screen
    */
   if (!ohos_load_driver(disp, swrast))
      return EGL_FALSE;

   if (!dri2_create_screen(disp)) {
      _eglLog(_EGL_WARNING, "DRI2: failed to create screen");
      ohos_unload_driver(disp);
      return EGL_FALSE;
   }
   return EGL_TRUE;
}

static EGLBoolean
ohos_open_device(_EGLDisplay *disp, bool swrast)
{
#define MAX_DRM_DEVICES 64
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   _EGLDevice *dev_list = _eglGlobal.DeviceList;
   drmDevicePtr device;

   char *vendor_name = NULL;

#ifdef EGL_FORCE_RENDERNODE
   const unsigned node_type = DRM_NODE_RENDER;
#else
   const unsigned node_type = swrast ? DRM_NODE_PRIMARY : DRM_NODE_RENDER;
#endif

   while (dev_list) {
      if (!_eglDeviceSupports(dev_list, _EGL_DEVICE_DRM))
         goto next;

      device = _eglDeviceDrm(dev_list);
      assert(device);

      if (!(device->available_nodes & (1 << node_type)))
         goto next;

      dri2_dpy->fd_render_gpu = loader_open_device(device->nodes[node_type]);
      if (dri2_dpy->fd_render_gpu < 0) {
         DISPLAY_LOGI();
         _eglLog(_EGL_WARNING, "%s() Failed to open DRM device %s", __func__,
                 device->nodes[node_type]);
         goto next;
      }

      /* If a vendor is explicitly provided, we use only that.
       * Otherwise we fall-back the first device that is supported.
       */
      DISPLAY_LOGI("vendor_name %{public}s", vendor_name);
      if (vendor_name) {
         if (ohos_filter_device(disp, dri2_dpy->fd_render_gpu, vendor_name)) {
            /* Device does not match - try next device */
            close(dri2_dpy->fd_render_gpu);
            dri2_dpy->fd_render_gpu = -1;
            DISPLAY_LOGI();
            goto next;
         }
         /* If the requested device matches - use it. Regardless if
          * init fails, do not fall-back to any other device.
          */
         if (!ohos_probe_device(disp, false)) {
            close(dri2_dpy->fd_render_gpu);
            dri2_dpy->fd_render_gpu = -1;
         }
         DISPLAY_LOGI();
         break;
      }
      if (ohos_probe_device(disp, swrast))
         break;

      DISPLAY_LOGI();
      /* No explicit request - attempt the next device */
      close(dri2_dpy->fd_render_gpu);
      dri2_dpy->fd_render_gpu = -1;

   next:
      dev_list = _eglDeviceNext(dev_list);
   }

   if (dri2_dpy->fd_render_gpu < 0) {
      _eglLog(_EGL_WARNING, "Failed to open %s DRM device",
              vendor_name ? "desired" : "any");
      return EGL_FALSE;
   }

   return EGL_TRUE;
}

EGLBoolean
dri2_initialize_ohos(_EGLDisplay *disp)
{
   bool device_opened = false;
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   const char *err;

   // dri2_dpy->gralloc = u_gralloc_create(U_GRALLOC_TYPE_AUTO);
   // if (dri2_dpy->gralloc == NULL) {
   //    err = "DRI2: failed to get gralloc";
   //    goto cleanup;
   // }

   bool force_pure_swrast = debug_get_bool_option("MESA_OHOS_NO_KMS_SWRAST", false);

   if (!force_pure_swrast)
      device_opened = ohos_open_device(disp, disp->Options.ForceSoftware);

   if ((!device_opened && disp->Options.ForceSoftware) ||
       force_pure_swrast) {
      dri2_dpy->driver_name = strdup("swrast");
      dri2_dpy->loader_extensions = ohos_swrast_image_loader_extensions;
      dri2_dpy->fd_render_gpu = -1;
      dri2_dpy->pure_swrast = true;
      if(!dri2_load_driver(disp)) {
         err = "DRI2: Failed to load swrast";
         goto cleanup;
      }

      if (!dri2_create_screen(disp)) {
         err = "DRI2: Failed to create swrast screen";
         goto cleanup;
      }

      device_opened = EGL_TRUE;
   }

   if (!device_opened) {
      err = "DRI2: failed to open device";
      goto cleanup;
   }

   dri2_dpy->fd_display_gpu = dri2_dpy->fd_render_gpu;

   if (!dri2_dpy->pure_swrast && !dri2_setup_device(disp, false)) {
      err = "DRI2: failed to setup EGLDevice";
      goto cleanup;
   }

   dri2_setup_screen(disp);

   dri2_setup_swap_interval(disp, 1);

   disp->Extensions.KHR_image = EGL_TRUE;

   /* Create configs *after* enabling extensions because presence of DRI
    * driver extensions can affect the capabilities of EGLConfigs.
    */
   ohos_add_configs_for_visuals(disp);

   /* Fill vtbl last to prevent accidentally calling virtual function during
    * initialization.
    */
   dri2_dpy->vtbl = &ohos_display_vtbl;

   return EGL_TRUE;

cleanup:
   return _eglError(EGL_NOT_INITIALIZED, err);
}

GLAPI void APIENTRY glEGLImageTargetTexture2DOES (GLenum target, GLeglImageOES image){}