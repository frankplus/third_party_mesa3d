/*
 * Copyright © 2026 OpenHarmony
 *
 * SPDX-License-Identifier: MIT
 */

#include "radv_ohos.h"

#if DETECT_OS_OHOS

#include "radv_buffer.h"
#include "radv_device.h"
#include "radv_device_memory.h"
#include "radv_entrypoints.h"
#include "radv_formats.h"
#include "radv_image.h"
#include "radv_physical_device.h"
#include "radv_queue.h"

#include "buffer_handle.h"
#include "native_buffer.h"
#include "native_buffer_inner.h"
#include "util/os_file.h"
#include "vk_common_entrypoints.h"
#include "vk_format.h"
#include "vk_log.h"
#include "vk_util.h"

#include <inttypes.h>
#include <string.h>
#include <unistd.h>

#define OHOS_BUFFER_USAGE_MEM_DMA     NATIVEBUFFER_USAGE_MEM_DMA
#define OHOS_BUFFER_USAGE_HW_RENDER   NATIVEBUFFER_USAGE_HW_RENDER
#define OHOS_BUFFER_USAGE_HW_TEXTURE  NATIVEBUFFER_USAGE_HW_TEXTURE
#define OHOS_BUFFER_USAGE_HW_COMPOSER (1ULL << 10)

static VkDeviceSize
ohos_native_buffer_allocation_size(const BufferHandle *handle)
{
   if (!handle || handle->fd < 0)
      return 0;

   if (handle->size > 0)
      return (VkDeviceSize)handle->size;

   off_t size = lseek(handle->fd, 0, SEEK_END);
   return size > 0 ? (VkDeviceSize)size : 0;
}

static bool
ohos_native_buffer_pitch(const BufferHandle *handle, VkFormat format,
                         uint32_t *pitch)
{
   unsigned bpe = vk_format_get_blocksize(format);

   if (!handle || handle->stride <= 0 || bpe == 0 ||
       handle->stride % bpe != 0)
      return false;

   *pitch = handle->stride / bpe;
   return *pitch > 0;
}

static bool
radv_ohos_image_override_offset_stride(struct radv_device *device,
                                       struct radv_image *image,
                                       uint64_t offset,
                                       uint32_t pitch)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);

   return ac_surface_override_offset_stride(&pdev->info,
                                            &image->planes[0].surface,
                                            image->vk.array_layers,
                                            image->vk.mip_levels,
                                            offset, pitch);
}

static VkFormat
vk_format_from_ohos_native_buffer(int format)
{
   switch (format) {
   case NATIVEBUFFER_PIXEL_FMT_RGBA_8888:
      return VK_FORMAT_R8G8B8A8_UNORM;
   case NATIVEBUFFER_PIXEL_FMT_BGRA_8888:
      return VK_FORMAT_B8G8R8A8_UNORM;
   case NATIVEBUFFER_PIXEL_FMT_RGBX_8888:
      return VK_FORMAT_R8G8B8A8_UNORM;
   case NATIVEBUFFER_PIXEL_FMT_BGRX_8888:
      return VK_FORMAT_B8G8R8A8_UNORM;
   case NATIVEBUFFER_PIXEL_FMT_RGB_565:
      return VK_FORMAT_R5G6B5_UNORM_PACK16;
   case NATIVEBUFFER_PIXEL_FMT_BGR_565:
      return VK_FORMAT_B5G6R5_UNORM_PACK16;
   case NATIVEBUFFER_PIXEL_FMT_RGBA_1010102:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
   default:
      return VK_FORMAT_UNDEFINED;
   }
}

uint64_t
radv_ohos_native_buffer_usage_for_vk_image(VkImageCreateFlags flags,
                                           VkImageUsageFlags usage)
{
   uint64_t native_usage = OHOS_BUFFER_USAGE_MEM_DMA;

   if (usage & (VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
      native_usage |= OHOS_BUFFER_USAGE_HW_RENDER;

   if (usage & (VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_STORAGE_BIT |
                VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT))
      native_usage |= OHOS_BUFFER_USAGE_HW_TEXTURE;

   if (!(flags & VK_IMAGE_CREATE_PROTECTED_BIT))
      native_usage |= OHOS_BUFFER_USAGE_HW_COMPOSER;

   return native_usage;
}

bool
radv_ohos_native_buffer_supports_format(VkFormat format,
                                        VkImageUsageFlags usage)
{
   (void)usage;

   switch (format) {
   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_R8G8B8A8_SRGB:
   case VK_FORMAT_B8G8R8A8_UNORM:
   case VK_FORMAT_B8G8R8A8_SRGB:
   case VK_FORMAT_R5G6B5_UNORM_PACK16:
   case VK_FORMAT_B5G6R5_UNORM_PACK16:
   case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
      return true;
   default:
      return false;
   }
}

static inline bool
unmask32(uint32_t *inout_mask, uint32_t test_mask)
{
   uint32_t orig_mask = *inout_mask;
   *inout_mask &= ~test_mask;
   return *inout_mask != orig_mask;
}

VkResult
radv_image_from_ohos_native_buffer(VkDevice device_h,
                                   const VkImageCreateInfo *base_info,
                                   const VkNativeBufferOHOS *native_buffer,
                                   const VkAllocationCallbacks *alloc,
                                   VkImage *out_image_h)
{
   VK_FROM_HANDLE(radv_device, device, device_h);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const BufferHandle *handle = (const BufferHandle *)native_buffer->handle;
   VkImage image_h = VK_NULL_HANDLE;
   struct radv_image *image = NULL;
   VkResult result;

   if (!handle || handle->fd < 0) {
      return vk_errorf(device, VK_ERROR_INVALID_EXTERNAL_HANDLE,
                       "VkNativeBufferOHOS::handle has no valid dma-buf fd");
   }

   const VkDeviceSize allocation_size = ohos_native_buffer_allocation_size(handle);
   if (allocation_size == 0) {
      return vk_errorf(device, VK_ERROR_INVALID_EXTERNAL_HANDLE,
                       "VkNativeBufferOHOS::handle has no valid allocation size");
   }

   const VkImportMemoryFdInfoKHR import_info = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
      .fd = os_dupfd_cloexec(handle->fd),
   };

   if (import_info.fd < 0)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   int memory_type_index = -1;
   for (int i = 0; i < pdev->memory_properties.memoryTypeCount; ++i) {
      bool is_local = !!(pdev->memory_properties.memoryTypes[i].propertyFlags &
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      bool is_32bit = !!(pdev->memory_types_32bit & (1u << i));
      if (is_local && !is_32bit) {
         memory_type_index = i;
         break;
      }
   }

   if (memory_type_index == -1)
      memory_type_index = 0;

   VkDeviceMemory memory_h;
   result = radv_AllocateMemory(device_h,
                                &(VkMemoryAllocateInfo){
                                   .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                   .pNext = &import_info,
                                   .allocationSize = allocation_size,
                                   .memoryTypeIndex = memory_type_index,
                                },
                                alloc, &memory_h);
   if (result != VK_SUCCESS) {
      close(import_info.fd);
      return result;
   }

   struct radv_device_memory *memory =
      radv_device_memory_from_handle(memory_h);

   VkImageCreateInfo updated_base_info = *base_info;
   updated_base_info.tiling = VK_IMAGE_TILING_LINEAR;
   VkExternalMemoryImageCreateInfo external_memory_info = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .pNext = updated_base_info.pNext,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   updated_base_info.pNext = &external_memory_info;

   result = radv_image_create(device_h,
                              &(struct radv_image_create_info){
                                 .vk_info = &updated_base_info,
                                 .no_metadata_planes = true,
                              },
                              alloc, &image_h, false);
   if (result != VK_SUCCESS)
      goto fail_create_image;

   image = radv_image_from_handle(image_h);
   uint32_t pitch = 0;
   if (!ohos_native_buffer_pitch(handle, image->vk.format, &pitch)) {
      result = vk_errorf(device, VK_ERROR_INVALID_EXTERNAL_HANDLE,
                         "OHOS native buffer has invalid stride %d for VkFormat(%d)",
                         handle->stride, image->vk.format);
      goto fail_bind_image;
   }

   if (!radv_ohos_image_override_offset_stride(device, image, 0, pitch)) {
      result = vk_errorf(device, VK_ERROR_INVALID_EXTERNAL_HANDLE,
                         "OHOS native buffer pitch %u is incompatible with RADV image layout",
                         pitch);
      goto fail_bind_image;
   }

   VkDeviceSize bo_size = memory->bo->size;
   if (bo_size < image->size) {
      result = vk_errorf(device, VK_ERROR_INVALID_EXTERNAL_HANDLE,
                         "OHOS native buffer BO (%" PRIu64
                         ") is smaller than RADV image size (%" PRIu64 ")",
                         bo_size, image->size);
      goto fail_bind_image;
   }

   const VkBindImageMemoryInfo bind_info = {
      .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
      .image = image_h,
      .memory = memory_h,
      .memoryOffset = 0,
   };
   result = radv_BindImageMemory2(device_h, 1, &bind_info);
   if (result != VK_SUCCESS)
      goto fail_bind_image;

   image->owned_memory = memory_h;
   *out_image_h = image_h;

   return VK_SUCCESS;

fail_bind_image:
   radv_DestroyImage(device_h, image_h, alloc);
fail_create_image:
   radv_FreeMemory(device_h, memory_h, alloc);
   return result;
}

VkResult
radv_import_ohos_native_buffer_memory(struct radv_device *device,
                                      struct radv_device_memory *mem,
                                      unsigned priority,
                                      const VkImportNativeBufferInfoOHOS *info)
{
   if (!info->buffer)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;

   const BufferHandle *handle = OH_NativeBuffer_GetBufferHandle(info->buffer);
   int dma_buf = handle ? handle->fd : -1;
   if (dma_buf < 0)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;

   uint64_t alloc_size = 0;
   VkResult result = radv_bo_from_fd(device, dma_buf, priority, mem, &alloc_size);
   if (result != VK_SUCCESS)
      return result;
   if (alloc_size == 0 && mem->bo)
      alloc_size = mem->bo->size;

   if (mem->image) {
      struct radv_image_create_info create_info = {
         .no_metadata_planes = true,
      };

      result = radv_image_create_layout(device, create_info, NULL, NULL,
                                        mem->image);
      if (result != VK_SUCCESS) {
         radv_bo_destroy(device, NULL, mem->bo);
         mem->bo = NULL;
         return result;
      }

      uint32_t pitch = 0;
      if (!ohos_native_buffer_pitch(handle, mem->image->vk.format, &pitch) ||
          !radv_ohos_image_override_offset_stride(device, mem->image, 0, pitch)) {
         radv_bo_destroy(device, NULL, mem->bo);
         mem->bo = NULL;
         return VK_ERROR_INVALID_EXTERNAL_HANDLE;
      }

      if (alloc_size < mem->image->size) {
         radv_bo_destroy(device, NULL, mem->bo);
         mem->bo = NULL;
         return VK_ERROR_INVALID_EXTERNAL_HANDLE;
      }
   } else if (mem->buffer) {
      if (alloc_size < mem->buffer->vk.size) {
         radv_bo_destroy(device, NULL, mem->bo);
         mem->bo = NULL;
         return VK_ERROR_INVALID_EXTERNAL_HANDLE;
      }
   }

   if (OH_NativeBuffer_Reference(info->buffer) != 0) {
      radv_bo_destroy(device, NULL, mem->bo);
      mem->bo = NULL;
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   mem->ohos_native_buffer = info->buffer;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
radv_GetSwapchainGrallocUsageOHOS(VkDevice device_h,
                                  VkFormat format,
                                  VkImageUsageFlags imageUsage,
                                  uint64_t *grallocUsage)
{
   VK_FROM_HANDLE(radv_device, device, device_h);
   struct radv_physical_device *pdev = radv_device_physical(device);
   VkPhysicalDevice pdev_h = radv_physical_device_to_handle(pdev);

   *grallocUsage = radv_ohos_native_buffer_usage_for_vk_image(0, imageUsage);

   const VkPhysicalDeviceImageFormatInfo2 image_format_info = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
      .format = format,
      .type = VK_IMAGE_TYPE_2D,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = imageUsage,
   };

   VkImageFormatProperties2 image_format_props = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
   };

   VkResult result = radv_GetPhysicalDeviceImageFormatProperties2(
      pdev_h, &image_format_info, &image_format_props);
   if (result != VK_SUCCESS) {
      return vk_errorf(device, result,
                       "radv_GetPhysicalDeviceImageFormatProperties2 failed inside %s",
                       __func__);
   }

   if (unmask32(&imageUsage, VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
      *grallocUsage |= OHOS_BUFFER_USAGE_HW_RENDER;

   if (unmask32(&imageUsage, VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                            VK_IMAGE_USAGE_SAMPLED_BIT |
                            VK_IMAGE_USAGE_STORAGE_BIT |
                            VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT))
      *grallocUsage |= OHOS_BUFFER_USAGE_HW_TEXTURE;

   if (imageUsage != 0) {
      return vk_errorf(device, VK_ERROR_FORMAT_NOT_SUPPORTED,
                       "unsupported VkImageUsageFlags(0x%x) for OHOS native-buffer swapchain",
                       imageUsage);
   }

   if (radv_ohos_native_buffer_supports_format(format, imageUsage))
      return VK_SUCCESS;

   return vk_errorf(device, VK_ERROR_FORMAT_NOT_SUPPORTED,
                    "unsupported VkFormat(%d) for OHOS native-buffer swapchain",
                    format);
}

VKAPI_ATTR VkResult VKAPI_CALL
radv_GetNativeBufferPropertiesOHOS(VkDevice device_h,
                                   const struct OH_NativeBuffer *buffer,
                                   VkNativeBufferPropertiesOHOS *pProperties)
{
   VK_FROM_HANDLE(radv_device, device, device_h);
   struct radv_physical_device *pdev = radv_device_physical(device);
   const BufferHandle *handle = OH_NativeBuffer_GetBufferHandle(buffer);

   if (!handle || handle->fd < 0)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;

   VkNativeBufferFormatPropertiesOHOS *format_prop =
      vk_find_struct(pProperties->pNext, NATIVE_BUFFER_FORMAT_PROPERTIES_OHOS);
   if (format_prop) {
      VkFormat format = vk_format_from_ohos_native_buffer(handle->format);
      if (format == VK_FORMAT_UNDEFINED)
         return VK_ERROR_INVALID_EXTERNAL_HANDLE;

      void *next = format_prop->pNext;
      memset(format_prop, 0, sizeof(*format_prop));
      format_prop->sType = VK_STRUCTURE_TYPE_NATIVE_BUFFER_FORMAT_PROPERTIES_OHOS;
      format_prop->pNext = next;
      format_prop->format = format;
      format_prop->externalFormat = (uint64_t)(uintptr_t)format;

      VkFormatProperties2 format_properties = {
         .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
      };
      radv_GetPhysicalDeviceFormatProperties2(
         radv_physical_device_to_handle(pdev), format, &format_properties);
      format_prop->formatFeatures =
         format_properties.formatProperties.optimalTilingFeatures;

      format_prop->samplerYcbcrConversionComponents.r = VK_COMPONENT_SWIZZLE_IDENTITY;
      format_prop->samplerYcbcrConversionComponents.g = VK_COMPONENT_SWIZZLE_IDENTITY;
      format_prop->samplerYcbcrConversionComponents.b = VK_COMPONENT_SWIZZLE_IDENTITY;
      format_prop->samplerYcbcrConversionComponents.a = VK_COMPONENT_SWIZZLE_IDENTITY;
      format_prop->suggestedYcbcrModel =
         VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY;
      format_prop->suggestedYcbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
      format_prop->suggestedXChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
      format_prop->suggestedYChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
   }

   VkDeviceSize allocation_size = ohos_native_buffer_allocation_size(handle);
   if (allocation_size == 0)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;

   pProperties->allocationSize = allocation_size;
   pProperties->memoryTypeBits =
      ((1u << pdev->memory_properties.memoryTypeCount) - 1) &
      ~pdev->memory_types_32bit;

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
radv_GetMemoryNativeBufferOHOS(VkDevice device_h,
                               const VkMemoryGetNativeBufferInfoOHOS *pInfo,
                               struct OH_NativeBuffer **pBuffer)
{
   VK_FROM_HANDLE(radv_device_memory, mem, pInfo->memory);
   (void)device_h;

   if (!mem->ohos_native_buffer)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;

   if (OH_NativeBuffer_Reference(mem->ohos_native_buffer) != 0)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   *pBuffer = mem->ohos_native_buffer;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
radv_AcquireImageOHOS(VkDevice device,
                      VkImage image,
                      int32_t nativeFenceFd,
                      VkSemaphore semaphore,
                      VkFence fence)
{
   return vk_common_AcquireImageOHOS(device, image, nativeFenceFd,
                                     semaphore, fence);
}

static VkResult
radv_ohos_release_semaphore_init_once(struct radv_queue *queue)
{
   struct radv_device *device = radv_queue_device(queue);

   if (queue->ohos_release_semaphore != VK_NULL_HANDLE)
      return VK_SUCCESS;

   const VkExportSemaphoreCreateInfo export_info = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
      .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
   };
   const VkSemaphoreCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
      .pNext = &export_info,
   };

   return device->vk.dispatch_table.CreateSemaphore(radv_device_to_handle(device),
                                                    &create_info, NULL,
                                                    &queue->ohos_release_semaphore);
}

VKAPI_ATTR VkResult VKAPI_CALL
radv_QueueSignalReleaseImageOHOS(VkQueue queue_h,
                                 uint32_t waitSemaphoreCount,
                                 const VkSemaphore *pWaitSemaphores,
                                 VkImage image,
                                 int32_t *pNativeFenceFd)
{
   VK_FROM_HANDLE(radv_queue, queue, queue_h);
   struct radv_device *device = radv_queue_device(queue);
   VkResult result;

   (void)image;

   STACK_ARRAY(VkPipelineStageFlags, stage_flags, MAX2(1, waitSemaphoreCount));
   for (uint32_t i = 0; i < MAX2(1, waitSemaphoreCount); i++)
      stage_flags[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

   result = radv_ohos_release_semaphore_init_once(queue);
   if (result != VK_SUCCESS) {
      STACK_ARRAY_FINISH(stage_flags);
      return result;
   }

   const VkSubmitInfo submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .waitSemaphoreCount = waitSemaphoreCount,
      .pWaitSemaphores = pWaitSemaphores,
      .pWaitDstStageMask = stage_flags,
      .signalSemaphoreCount = 1,
      .pSignalSemaphores = &queue->ohos_release_semaphore,
   };
   result = device->vk.dispatch_table.QueueSubmit(queue_h, 1, &submit_info,
                                                  VK_NULL_HANDLE);
   STACK_ARRAY_FINISH(stage_flags);
   if (result != VK_SUCCESS)
      return result;

   const VkSemaphoreGetFdInfoKHR get_fd = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
      .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
      .semaphore = queue->ohos_release_semaphore,
   };
   return device->vk.dispatch_table.GetSemaphoreFdKHR(radv_device_to_handle(device),
                                                      &get_fd, pNativeFenceFd);
}

VkFormat
radv_select_ohos_external_format(const void *next, VkFormat default_format)
{
   const VkExternalFormatOHOS *ohos_format =
      vk_find_struct_const(next, EXTERNAL_FORMAT_OHOS);

   if (ohos_format && ohos_format->externalFormat)
      return (VkFormat)ohos_format->externalFormat;

   return default_format;
}

#endif /* DETECT_OS_OHOS */
