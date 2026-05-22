/*
 * Copyright © 2026 OpenHarmony
 *
 * SPDX-License-Identifier: MIT
 */

#include "vk_common_entrypoints.h"
#include "vk_device.h"
#include "vk_log.h"
#include "vk_util.h"

#include <errno.h>
#include <unistd.h>

VKAPI_ATTR VkResult VKAPI_CALL
vk_common_AcquireImageOHOS(VkDevice _device,
                           VkImage image,
                           int32_t nativeFenceFd,
                           VkSemaphore semaphore,
                           VkFence fence)
{
   VK_FROM_HANDLE(vk_device, device, _device);
   VkResult result = VK_SUCCESS;

   (void)image;

   int semaphore_fd = -1, fence_fd = -1;
   if (nativeFenceFd >= 0) {
      if (semaphore != VK_NULL_HANDLE && fence != VK_NULL_HANDLE) {
         semaphore_fd = nativeFenceFd;
         fence_fd = dup(nativeFenceFd);
         if (fence_fd < 0) {
            VkResult err = (errno == EMFILE) ? VK_ERROR_TOO_MANY_OBJECTS :
                                               VK_ERROR_OUT_OF_HOST_MEMORY;
            close(nativeFenceFd);
            return vk_error(device, err);
         }
      } else if (semaphore != VK_NULL_HANDLE) {
         semaphore_fd = nativeFenceFd;
      } else if (fence != VK_NULL_HANDLE) {
         fence_fd = nativeFenceFd;
      } else {
         close(nativeFenceFd);
      }
   }

   if (semaphore != VK_NULL_HANDLE) {
      const VkImportSemaphoreFdInfoKHR info = {
         .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
         .semaphore = semaphore,
         .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
         .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
         .fd = semaphore_fd,
      };
      result = device->dispatch_table.ImportSemaphoreFdKHR(_device, &info);
      if (result == VK_SUCCESS)
         semaphore_fd = -1;
   }

   if (result == VK_SUCCESS && fence != VK_NULL_HANDLE) {
      const VkImportFenceFdInfoKHR info = {
         .sType = VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR,
         .fence = fence,
         .flags = VK_FENCE_IMPORT_TEMPORARY_BIT,
         .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
         .fd = fence_fd,
      };
      result = device->dispatch_table.ImportFenceFdKHR(_device, &info);
      if (result == VK_SUCCESS)
         fence_fd = -1;
   }

   if (semaphore_fd >= 0)
      close(semaphore_fd);
   if (fence_fd >= 0)
      close(fence_fd);

   return result;
}
