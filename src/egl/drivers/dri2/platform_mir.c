/*
 * Copyright © 2012 Canonical, Inc
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Christopher James Halse Rogers <christopher.halse.rogers@canonical.com>
 *    Alexandros Frantzis <alexandros.frantzis@canonical.com>
 */

#include <mir_toolkit/mesa/native_display.h>
#include <mir_toolkit/mir_native_buffer.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>

#include "egl_dri2.h"
#include "egl_dri2_fallbacks.h"
#include "loader.h"

static __DRIbuffer *
dri2_get_buffers_with_format(__DRIdrawable * driDrawable,
			     int *width, int *height,
			     unsigned int *attachments, int count,
			     int *out_count, void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);
   int i;

   for (i = 0; i < 2*count; i+=2) {
      assert(attachments[i] < __DRI_BUFFER_COUNT);
      assert((i/2) < ARRAY_SIZE(dri2_surf->buffers));

      if (dri2_surf->local_buffers[attachments[i]] == NULL) {
         /* Our frame callback must keep these buffers valid */
         assert(attachments[i] != __DRI_BUFFER_FRONT_LEFT);
         assert(attachments[i] != __DRI_BUFFER_BACK_LEFT);

         dri2_surf->local_buffers[attachments[i]] =
            dri2_dpy->dri2->allocateBuffer(dri2_dpy->dri_screen,
                  attachments[i], attachments[i+1],
                  dri2_surf->base.Width, dri2_surf->base.Height);

         if (!dri2_surf->local_buffers[attachments[i]]) {
            _eglError(EGL_BAD_ALLOC, "failed to allocate auxiliary buffer");
	    return NULL;
	 }
      }

      memcpy(&dri2_surf->buffers[i/2],
             dri2_surf->local_buffers[attachments[i]],
             sizeof(__DRIbuffer));
   }

   assert(dri2_surf->base.Type == EGL_PIXMAP_BIT ||
          dri2_surf->local_buffers[__DRI_BUFFER_BACK_LEFT]);

   *out_count = i/2;
   if (i == 0)
	   return NULL;

   *width = dri2_surf->base.Width;
   *height = dri2_surf->base.Height;

   return dri2_surf->buffers;
}

static __DRIbuffer *
dri2_get_buffers(__DRIdrawable * driDrawable,
		 int *width, int *height,
		 unsigned int *attachments, int count,
		 int *out_count, void *loaderPrivate)
{
   unsigned int *attachments_with_format;
   __DRIbuffer *buffer;
   const unsigned int format = 32;
   int i;

   attachments_with_format = calloc(count * 2, sizeof(unsigned int));
   if (!attachments_with_format) {
      *out_count = 0;
      return NULL;
   }

   for (i = 0; i < count; ++i) {
      attachments_with_format[2*i] = attachments[i];
      attachments_with_format[2*i + 1] = format;
   }

   buffer =
      dri2_get_buffers_with_format(driDrawable,
				   width, height,
				   attachments_with_format, count,
				   out_count, loaderPrivate);

   free(attachments_with_format);

   return buffer;
}

static int
dri2_image_get_buffers(__DRIdrawable *driDrawable,
                       unsigned int format,
                       uint32_t *stamp,
                       void *loaderPrivate,
                       uint32_t buffer_mask,
                       struct __DRIimageList *buffers)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;

   if (buffer_mask & __DRI_IMAGE_BUFFER_BACK) {
      if (!dri2_surf->back)
         return 0;

      buffers->back = ((struct gbm_dri_bo *)dri2_surf->back->bo)->image;
      buffers->image_mask = __DRI_IMAGE_BUFFER_BACK;

      return 1;
   }

   return 0;
}

static void
dri2_flush_front_buffer(__DRIdrawable * driDrawable, void *loaderPrivate)
{
   (void) driDrawable;

   /* FIXME: Does EGL support front buffer rendering at all? */

#if 0
   struct dri2_egl_surface *dri2_surf = loaderPrivate;

   dri2WaitGL(dri2_surf);
#else
   (void) loaderPrivate;
#endif
}

static struct gbm_bo *create_gbm_bo_from_buffer(struct gbm_device* gbm_dev,
                                                MirBufferPackage *package)
{
   struct gbm_import_fd_data data;

   data.fd = package->fd[0];
   data.width = package->width;
   data.height = package->height;
   data.format = GBM_FORMAT_ARGB8888; /* TODO: Use mir surface format */
   data.stride = package->stride;

   return gbm_bo_import(gbm_dev, GBM_BO_IMPORT_FD, &data, GBM_BO_USE_RENDERING);
}

static ssize_t find_cached_buffer_with_fd(struct dri2_egl_surface *dri2_surf,
                                          int fd)
{
   ssize_t i;

   for (i = 0; i < ARRAY_SIZE(dri2_surf->color_buffers); i++) {
      if (dri2_surf->color_buffers[i].fd == fd)
         return i;
   }

   return -1;
}

static void cache_buffer(struct dri2_egl_surface *dri2_surf, size_t slot,
                         MirBufferPackage *buffer_package)
{
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);

   if (dri2_surf->color_buffers[slot].bo != NULL)
      gbm_bo_destroy(dri2_surf->color_buffers[slot].bo);

   dri2_surf->color_buffers[slot].bo = create_gbm_bo_from_buffer(
      &dri2_dpy->gbm_dri->base,
      buffer_package);

   dri2_surf->color_buffers[slot].fd = buffer_package->fd[0];
}

static size_t find_best_cache_slot(struct dri2_egl_surface *dri2_surf)
{
   size_t i;
   size_t start_slot = 0;

   /*
    * If we have a back buffer, start searching after it to ensure
    * we don't reuse the slot too soon.
    */
   if (dri2_surf->back != NULL) {
      start_slot = dri2_surf->back - dri2_surf->color_buffers;
      start_slot = (start_slot + 1) % ARRAY_SIZE(dri2_surf->color_buffers);
   }

   /* Try to find an empty slot */
   for (i = 0; i < ARRAY_SIZE(dri2_surf->color_buffers); i++) {
      size_t slot = (start_slot + i) % ARRAY_SIZE(dri2_surf->color_buffers);
      if (dri2_surf->color_buffers[slot].bo == NULL)
         return slot;
   }

   /* If we don't have an empty slot, use the start slot */
   return start_slot;
}

static void update_cached_buffer_ages(struct dri2_egl_surface *dri2_surf,
                                      size_t used_slot)
{
   /*
    * If 3 (Mir surfaces are triple buffered at most) other buffers have been
    * used since a buffer was used, we probably won't need this buffer again.
    */
   static const int destruction_age = 3;
   size_t i;

   for (i = 0; i < ARRAY_SIZE(dri2_surf->color_buffers); i++) {
      if (dri2_surf->color_buffers[i].bo != NULL) {
         if (i == used_slot) {
            dri2_surf->color_buffers[i].age = 0;
         }
         else {
            ++dri2_surf->color_buffers[i].age;
            if (dri2_surf->color_buffers[i].age == destruction_age) {
               gbm_bo_destroy(dri2_surf->color_buffers[i].bo);
               dri2_surf->color_buffers[i].bo = NULL;
               dri2_surf->color_buffers[i].fd = -1;
            }
         }
      }
   }
}

static void clear_cached_buffers(struct dri2_egl_surface *dri2_surf)
{
   size_t i;
   for (i = 0; i < ARRAY_SIZE(dri2_surf->color_buffers); i++) {
      if (dri2_surf->color_buffers[i].bo != NULL)
         gbm_bo_destroy(dri2_surf->color_buffers[i].bo);
      dri2_surf->color_buffers[i].bo = NULL;
      dri2_surf->color_buffers[i].fd = -1;
      dri2_surf->color_buffers[i].age = 0;
   }
}

static EGLBoolean
mir_advance_colour_buffer(struct dri2_egl_surface *dri2_surf)
{
   MirBufferPackage buffer_package;
   ssize_t buf_slot = -1;

   if(!dri2_surf->mir_surf->surface_advance_buffer(dri2_surf->mir_surf, &buffer_package))
      return EGL_FALSE;

   /* We expect no data items, and (for the moment) one PRIME fd */
   assert(buffer_package.data_items == 0);
   assert(buffer_package.fd_items == 1);

   /* Mir ABIs prior to release 0.1.2 lacked width and height */
   if (buffer_package.width && buffer_package.height) {
      dri2_surf->base.Width = buffer_package.width;
      dri2_surf->base.Height = buffer_package.height;
   }

   buf_slot = find_cached_buffer_with_fd(dri2_surf, buffer_package.fd[0]);
   if (buf_slot != -1) {
      /*
       * If we get a new buffer with an fd of a previously cached buffer,
       * replace the old buffer in the cache...
       */
      if (buffer_package.age == 0)
         cache_buffer(dri2_surf, buf_slot, &buffer_package);
      /* ... otherwise just reuse the existing cached buffer */
   }
   else {
      /* We got a new buffer with an fd that's not in the cache, so add it */
      buf_slot = find_best_cache_slot(dri2_surf);
      cache_buffer(dri2_surf, buf_slot, &buffer_package);
   }

   update_cached_buffer_ages(dri2_surf, buf_slot);

   dri2_surf->back = &dri2_surf->color_buffers[buf_slot];
   dri2_surf->back->buffer_age = buffer_package.age;
   dri2_surf->local_buffers[__DRI_BUFFER_BACK_LEFT]->name = 0;
   dri2_surf->local_buffers[__DRI_BUFFER_BACK_LEFT]->fd = buffer_package.fd[0];
   dri2_surf->local_buffers[__DRI_BUFFER_BACK_LEFT]->pitch = buffer_package.stride;
   return EGL_TRUE;
}

/**
 * Called via eglCreateWindowSurface(), drv->API.CreateWindowSurface().
 */
static _EGLSurface *
dri2_create_mir_window_surface(_EGLDriver *drv, _EGLDisplay *disp,
                               _EGLConfig *conf, EGLNativeWindowType window,
                               const EGLint *attrib_list)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_config *dri2_conf = dri2_egl_config(conf);
   struct dri2_egl_surface *dri2_surf;
   const __DRIconfig *config;
   MirWindowParameters win_params;

   (void) drv;

   dri2_surf = calloc(1, sizeof *dri2_surf);
   if (!dri2_surf) {
      _eglError(EGL_BAD_ALLOC, "dri2_create_surface");
      return NULL;
   }

   if (!_eglInitSurface(&dri2_surf->base, disp, EGL_WINDOW_BIT, conf, attrib_list, window))
      goto cleanup_surf;

   dri2_surf->mir_surf = window;
   if (!dri2_surf->mir_surf->surface_get_parameters(dri2_surf->mir_surf, &win_params))
      goto cleanup_surf;

   dri2_surf->base.Width = win_params.width;
   dri2_surf->base.Height = win_params.height;

   dri2_surf->local_buffers[__DRI_BUFFER_FRONT_LEFT] =
      calloc(sizeof(*dri2_surf->local_buffers[0]), 1);
   dri2_surf->local_buffers[__DRI_BUFFER_BACK_LEFT] =
      calloc(sizeof(*dri2_surf->local_buffers[0]), 1);

   dri2_surf->local_buffers[__DRI_BUFFER_BACK_LEFT]->attachment =
      __DRI_BUFFER_BACK_LEFT;
   /* We only do ARGB 8888 for the moment */
   dri2_surf->local_buffers[__DRI_BUFFER_BACK_LEFT]->cpp = 4;

   clear_cached_buffers(dri2_surf);

   if(!mir_advance_colour_buffer(dri2_surf))
      goto cleanup_surf;

   config = dri2_get_dri_config(dri2_conf, EGL_WINDOW_BIT,
                                dri2_surf->base.GLColorspace);

   if (dri2_dpy->gbm_dri) {
      struct gbm_dri_surface *surf = malloc(sizeof *surf);

      dri2_surf->gbm_surf = surf;
      surf->base.gbm = &dri2_dpy->gbm_dri->base;
      surf->base.width = dri2_surf->base.Width;
      surf->base.height = dri2_surf->base.Height;
      surf->base.format = GBM_FORMAT_ARGB8888;
      surf->base.flags = GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING;
      surf->dri_private = dri2_surf;

      dri2_surf->dri_drawable =
          (*dri2_dpy->dri2->createNewDrawable) (dri2_dpy->dri_screen,
                                                config,
                                                dri2_surf->gbm_surf);
   }
   else {
      dri2_surf->dri_drawable =
          (*dri2_dpy->dri2->createNewDrawable) (dri2_dpy->dri_screen,
                                                config,
                                                dri2_surf);
   }

   if (dri2_surf->dri_drawable == NULL) {
      _eglError(EGL_BAD_ALLOC, "dri2->createNewDrawable");
   }

   return &dri2_surf->base;

cleanup_surf:
   free(dri2_surf);
   return NULL;
}

static _EGLSurface *
dri2_mir_create_pixmap_surface(_EGLDriver *drv, _EGLDisplay *disp,
                              _EGLConfig *conf, void *native_window,
                              const EGLint *attrib_list)
{
   _eglError(EGL_BAD_PARAMETER, "cannot create EGL pixmap surfaces on mir");
   return NULL;
}
// 

static EGLBoolean
dri2_destroy_mir_surface(_EGLDriver *drv, _EGLDisplay *disp, _EGLSurface *surf)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);
   int i;

   (void) drv;

   if (!_eglPutSurface(surf))
      return EGL_TRUE;

   clear_cached_buffers(dri2_surf);

   (*dri2_dpy->core->destroyDrawable)(dri2_surf->dri_drawable);

   for (i = 0; i < __DRI_BUFFER_COUNT; ++i) {
      if (dri2_surf->local_buffers[i]) {
         if ((i == __DRI_BUFFER_FRONT_LEFT) ||
             (i == __DRI_BUFFER_BACK_LEFT)) {
            free(dri2_surf->local_buffers[i]);
         }
         else {
            dri2_dpy->dri2->releaseBuffer(dri2_dpy->dri_screen,
                                          dri2_surf->local_buffers[i]);
         }
      }
   }

   free(dri2_surf->gbm_surf);
   free(surf);

   return EGL_TRUE;
}

/**
 * Called via eglSwapInterval(), drv->API.SwapInterval().
 */
static EGLBoolean
dri2_set_swap_interval(_EGLDriver *drv, _EGLDisplay *disp,
                       _EGLSurface *surf, EGLint interval)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);
   if(!dri2_surf->mir_surf->surface_set_swapinterval(dri2_surf->mir_surf, interval))
      return EGL_FALSE;
   return EGL_TRUE;
}

/**
 * Called via eglSwapBuffers(), drv->API.SwapBuffers().
 */
static EGLBoolean
dri2_swap_buffers(_EGLDriver *drv, _EGLDisplay *disp, _EGLSurface *draw)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(draw);

   (*dri2_dpy->flush->flush)(dri2_surf->dri_drawable);

   int rc = mir_advance_colour_buffer(dri2_surf);

   (*dri2_dpy->flush->invalidate)(dri2_surf->dri_drawable);

   return rc;
}

static int
dri2_mir_authenticate(_EGLDisplay *disp, uint32_t id)
{
   return 0;
}

static _EGLImage *
dri2_create_image_khr_pixmap(_EGLDisplay *disp, _EGLContext *ctx,
                             EGLClientBuffer buffer, const EGLint *attr_list)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct gbm_dri_bo *dri_bo = gbm_dri_bo((struct gbm_bo *) buffer);
   struct dri2_egl_image *dri2_img;

   dri2_img = malloc(sizeof *dri2_img);
   if (!dri2_img) {
      _eglError(EGL_BAD_ALLOC, "dri2_create_image_khr_pixmap");
      return NULL;
   }

   _eglInitImage(&dri2_img->base, disp);

   dri2_img->dri_image = dri2_dpy->image->dupImage(dri_bo->image, dri2_img);
   if (dri2_img->dri_image == NULL) {
      free(dri2_img);
      _eglError(EGL_BAD_ALLOC, "dri2_create_image_khr_pixmap");
      return NULL;
   }

   return &dri2_img->base;
}

static _EGLImage *
dri2_mir_create_image_khr(_EGLDriver *drv, _EGLDisplay *disp,
                          _EGLContext *ctx, EGLenum target,
                          EGLClientBuffer buffer, const EGLint *attr_list)
{
   (void) drv;

   switch (target) {
   case EGL_NATIVE_PIXMAP_KHR:
      return dri2_create_image_khr_pixmap(disp, ctx, buffer, attr_list);
   default:
      return dri2_create_image_khr(drv, disp, ctx, target, buffer, attr_list);
   }
}

static EGLint
dri2_mir_query_buffer_age(_EGLDriver *drv, _EGLDisplay *dpy,
                               _EGLSurface *surf)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);
   if (dri2_surf->back)
   {
      return dri2_surf->back->buffer_age;
   }
   return 0;
}

static struct dri2_egl_display_vtbl dri2_mir_display_vtbl = {
   .authenticate = dri2_mir_authenticate,
   .create_window_surface = dri2_create_mir_window_surface,
   .create_pixmap_surface = dri2_mir_create_pixmap_surface,
   .create_pbuffer_surface = dri2_fallback_create_pbuffer_surface,
   .destroy_surface = dri2_destroy_mir_surface,
   .create_image = dri2_mir_create_image_khr,
   .swap_interval = dri2_set_swap_interval,
   .swap_buffers = dri2_swap_buffers,
   .swap_buffers_with_damage = dri2_fallback_swap_buffers_with_damage,
   .swap_buffers_region = dri2_fallback_swap_buffers_region,
   .post_sub_buffer = dri2_fallback_post_sub_buffer,
   .copy_buffers = dri2_fallback_copy_buffers,
   .query_buffer_age = dri2_mir_query_buffer_age,
   .create_wayland_buffer_from_image = dri2_fallback_create_wayland_buffer_from_image,
   .get_sync_values = dri2_fallback_get_sync_values,
   .get_dri_drawable = dri2_surface_get_dri_drawable,
};

EGLBoolean
dri2_initialize_mir(_EGLDriver *drv, _EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy;
   struct gbm_device *gbm = NULL;
   MirPlatformPackage platform;
   const __DRIconfig *config;
   static const int argb_shifts[4] = { 16, 8, 0, 24 };
   static const unsigned int argb_sizes[4] = { 8, 8, 8, 8 };
   uint32_t types;
   int i;

   loader_set_logger(_eglLog);

   dri2_dpy = calloc(1, sizeof *dri2_dpy);
   if (!dri2_dpy)
      return _eglError(EGL_BAD_ALLOC, "eglInitialize");

   disp->DriverData = (void *) dri2_dpy;
   dri2_dpy->mir_disp = disp->PlatformDisplay;
   dri2_dpy->mir_disp->display_get_platform(dri2_dpy->mir_disp, &platform);
   dri2_dpy->fd = platform.fd[0];

   /*
    * At the moment, a pointer to gbm_device is the first and only
    * information optionally contained in platform.data[].
    */
   if (platform.data_items == 0) {
      dri2_dpy->own_device = 1;
      dri2_dpy->fd = dup(dri2_dpy->fd);
      gbm = gbm_create_device(dri2_dpy->fd);
      if (gbm == NULL)
         goto cleanup_dpy;
   }
   else {
      gbm = *(struct gbm_device**)platform.data;
   }

   if (gbm) {
      struct gbm_dri_device *gbm_dri = gbm_dri_device(gbm);

      dri2_dpy->gbm_dri = gbm_dri;
      dri2_dpy->driver_name = strdup(gbm_dri->driver_name);
      dri2_dpy->dri_screen = gbm_dri->screen;
      dri2_dpy->core = gbm_dri->core;
      dri2_dpy->dri2 = gbm_dri->dri2;
      dri2_dpy->image = gbm_dri->image;
      dri2_dpy->flush = gbm_dri->flush;
      dri2_dpy->driver_configs = gbm_dri->driver_configs;

      gbm_dri->lookup_image = dri2_lookup_egl_image;
      gbm_dri->lookup_user_data = disp;

      gbm_dri->get_buffers = dri2_get_buffers;
      gbm_dri->flush_front_buffer = dri2_flush_front_buffer;
      gbm_dri->get_buffers_with_format = dri2_get_buffers_with_format;
      gbm_dri->image_get_buffers = dri2_image_get_buffers;

      if (!dri2_setup_extensions(disp))
         goto cleanup_dpy;
      dri2_setup_screen(disp);
   }

   types = EGL_WINDOW_BIT;
   for (i = 0; dri2_dpy->driver_configs[i]; i++) {
      config = dri2_dpy->driver_configs[i];
      dri2_add_config(disp, config, i + 1, types, NULL, argb_shifts, argb_sizes);
   }

   disp->Extensions.EXT_buffer_age = EGL_TRUE;
   disp->Extensions.EXT_swap_buffers_with_damage = EGL_FALSE;
   disp->Extensions.KHR_image_pixmap = EGL_TRUE;

   dri2_dpy->vtbl = &dri2_mir_display_vtbl;

   return EGL_TRUE;

 cleanup_dpy:
   free(dri2_dpy);

   return EGL_FALSE;
}

void
dri2_teardown_mir(struct dri2_egl_display *dri2_dpy)
{
   if (dri2_dpy->own_device)
      gbm_device_destroy(&dri2_dpy->gbm_dri->base);
}
