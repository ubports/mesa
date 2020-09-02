/*
 * Copyright © 2019 Red Hat.
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
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "val_private.h"
#include "util/format/u_format.h"
#include "util/u_inlines.h"
#include "pipe/p_state.h"

VkResult
val_image_create(VkDevice _device,
                 const struct val_image_create_info *create_info,
                 const VkAllocationCallbacks* alloc,
                 VkImage *pImage)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   const VkImageCreateInfo *pCreateInfo = create_info->vk_info;
   struct val_image *image;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);

   image = vk_zalloc2(&device->alloc, alloc, sizeof(*image), 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (image == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &image->base, VK_OBJECT_TYPE_IMAGE);
   image->alignment = 16;
   image->type = pCreateInfo->imageType;
   {
      struct pipe_resource template;

      memset(&template, 0, sizeof(template));

      template.screen = device->pscreen;
      switch (pCreateInfo->imageType) {
      case VK_IMAGE_TYPE_1D:
         template.target = pCreateInfo->arrayLayers > 1 ? PIPE_TEXTURE_1D_ARRAY : PIPE_TEXTURE_1D;
         break;
      default:
      case VK_IMAGE_TYPE_2D:
         template.target = pCreateInfo->arrayLayers > 1 ? PIPE_TEXTURE_2D_ARRAY : PIPE_TEXTURE_2D;
         if (pCreateInfo->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)
            template.target = pCreateInfo->arrayLayers == 6 ? PIPE_TEXTURE_CUBE : PIPE_TEXTURE_CUBE_ARRAY;
         break;
      case VK_IMAGE_TYPE_3D:
         template.target = PIPE_TEXTURE_3D;
         break;
      }

      template.format = vk_format_to_pipe(pCreateInfo->format);
      template.width0 = pCreateInfo->extent.width;
      template.height0 = pCreateInfo->extent.height;
      template.depth0 = pCreateInfo->extent.depth;
      template.array_size = pCreateInfo->arrayLayers;
      template.last_level = pCreateInfo->mipLevels - 1;
      template.nr_samples = pCreateInfo->samples;
      template.nr_storage_samples = pCreateInfo->samples;
      if (create_info->bind_flags)
         template.bind = create_info->bind_flags;
      image->bo = device->pscreen->resource_create_unbacked(device->pscreen,
                                                            &template,
                                                            &image->size);
   }
   *pImage = val_image_to_handle(image);

   return VK_SUCCESS;
}

VkResult
val_CreateImage(VkDevice device,
                const VkImageCreateInfo *pCreateInfo,
                const VkAllocationCallbacks *pAllocator,
                VkImage *pImage)
{
   return val_image_create(device,
      &(struct val_image_create_info) {
         .vk_info = pCreateInfo,
         .bind_flags = 0,
      },
      pAllocator,
      pImage);
}

void
val_DestroyImage(VkDevice _device, VkImage _image,
                 const VkAllocationCallbacks *pAllocator)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   VAL_FROM_HANDLE(val_image, image, _image);

   if (!_image)
     return;
   pipe_resource_reference(&image->bo, NULL);
   vk_object_base_finish(&image->base);
   vk_free2(&device->alloc, pAllocator, image);
}

VkResult
val_CreateImageView(VkDevice _device,
                    const VkImageViewCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkImageView *pView)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   VAL_FROM_HANDLE(val_image, image, pCreateInfo->image);
   struct val_image_view *view;

   view = vk_alloc2(&device->alloc, pAllocator, sizeof(*view), 8,
                     VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (view == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &view->base,
                       VK_OBJECT_TYPE_IMAGE_VIEW);
   view->view_type = pCreateInfo->viewType;
   view->format = pCreateInfo->format;
   view->pformat = vk_format_to_pipe(pCreateInfo->format);
   view->components = pCreateInfo->components;
   view->subresourceRange = pCreateInfo->subresourceRange;
   view->image = image;
   view->surface = NULL;
   *pView = val_image_view_to_handle(view);

   return VK_SUCCESS;
}

void
val_DestroyImageView(VkDevice _device, VkImageView _iview,
                     const VkAllocationCallbacks *pAllocator)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   VAL_FROM_HANDLE(val_image_view, iview, _iview);

   if (!_iview)
     return;

   pipe_surface_reference(&iview->surface, NULL);
   vk_object_base_finish(&iview->base);
   vk_free2(&device->alloc, pAllocator, iview);
}

void val_GetImageSubresourceLayout(
    VkDevice                                    _device,
    VkImage                                     _image,
    const VkImageSubresource*                   pSubresource,
    VkSubresourceLayout*                        pLayout)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   VAL_FROM_HANDLE(val_image, image, _image);
   uint32_t stride, offset;
   device->pscreen->resource_get_info(device->pscreen,
                                      image->bo,
                                      &stride, &offset);
   pLayout->offset = offset;
   pLayout->rowPitch = stride;
   pLayout->arrayPitch = 0;
   pLayout->size = image->size;
   switch (pSubresource->aspectMask) {
   case VK_IMAGE_ASPECT_COLOR_BIT:
      break;
   case VK_IMAGE_ASPECT_DEPTH_BIT:
      break;
   case VK_IMAGE_ASPECT_STENCIL_BIT:
      break;
   default:
      assert(!"Invalid image aspect");
   }
}

VkResult val_CreateBuffer(
    VkDevice                                    _device,
    const VkBufferCreateInfo*                   pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkBuffer*                                   pBuffer)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   struct val_buffer *buffer;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);

   /* gallium has max 32-bit buffer sizes */
   if (pCreateInfo->size > UINT32_MAX)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   buffer = vk_alloc2(&device->alloc, pAllocator, sizeof(*buffer), 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (buffer == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &buffer->base, VK_OBJECT_TYPE_BUFFER);
   buffer->size = pCreateInfo->size;
   buffer->usage = pCreateInfo->usage;
   buffer->offset = 0;

   {
      struct pipe_resource template;
      memset(&template, 0, sizeof(struct pipe_resource));
      template.screen = device->pscreen;
      template.target = PIPE_BUFFER;
      template.format = PIPE_FORMAT_R8_UNORM;
      template.width0 = buffer->size;
      template.height0 = 1;
      template.depth0 = 1;
      template.array_size = 1;
      template.flags = PIPE_RESOURCE_FLAG_DONT_OVER_ALLOCATE;
      buffer->bo = device->pscreen->resource_create_unbacked(device->pscreen,
                                                             &template,
                                                             &buffer->total_size);
      if (!buffer->bo) {
         vk_free2(&device->alloc, pAllocator, buffer);
         return vk_error(device->instance, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      }
   }
   *pBuffer = val_buffer_to_handle(buffer);

   return VK_SUCCESS;
}

void val_DestroyBuffer(
    VkDevice                                    _device,
    VkBuffer                                    _buffer,
    const VkAllocationCallbacks*                pAllocator)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   VAL_FROM_HANDLE(val_buffer, buffer, _buffer);

   if (!_buffer)
     return;

   pipe_resource_reference(&buffer->bo, NULL);
   vk_object_base_finish(&buffer->base);
   vk_free2(&device->alloc, pAllocator, buffer);
}

VkResult
val_CreateBufferView(VkDevice _device,
                     const VkBufferViewCreateInfo *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkBufferView *pView)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   VAL_FROM_HANDLE(val_buffer, buffer, pCreateInfo->buffer);
   struct val_buffer_view *view;
   view = vk_alloc2(&device->alloc, pAllocator, sizeof(*view), 8,
                     VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!view)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &view->base,
                       VK_OBJECT_TYPE_BUFFER_VIEW);
   view->buffer = buffer;
   view->format = pCreateInfo->format;
   view->pformat = vk_format_to_pipe(pCreateInfo->format);
   view->offset = pCreateInfo->offset;
   view->range = pCreateInfo->range;
   *pView = val_buffer_view_to_handle(view);

   return VK_SUCCESS;
}

void
val_DestroyBufferView(VkDevice _device, VkBufferView bufferView,
                      const VkAllocationCallbacks *pAllocator)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   VAL_FROM_HANDLE(val_buffer_view, view, bufferView);

   if (!bufferView)
     return;
   vk_object_base_finish(&view->base);
   vk_free2(&device->alloc, pAllocator, view);
}
