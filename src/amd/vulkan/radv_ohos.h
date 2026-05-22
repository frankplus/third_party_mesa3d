/*
 * Copyright © 2026 OpenHarmony
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef RADV_OHOS_H
#define RADV_OHOS_H

#include "util/detect_os.h"

#if DETECT_OS_OHOS

#include <stdbool.h>

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_ohos.h>

struct radv_device;
struct radv_device_memory;

VkResult radv_image_from_ohos_native_buffer(VkDevice device_h,
                                            const VkImageCreateInfo *base_info,
                                            const VkNativeBufferOHOS *native_buffer,
                                            const VkAllocationCallbacks *alloc,
                                            VkImage *out_image_h);

VkResult radv_import_ohos_native_buffer_memory(struct radv_device *device,
                                               struct radv_device_memory *mem,
                                               unsigned priority,
                                               const VkImportNativeBufferInfoOHOS *info);

uint64_t radv_ohos_native_buffer_usage_for_vk_image(VkImageCreateFlags flags,
                                                    VkImageUsageFlags usage);

bool radv_ohos_native_buffer_supports_format(VkFormat format,
                                             VkImageUsageFlags usage);

VkFormat radv_select_ohos_external_format(const void *next,
                                          VkFormat default_format);

#endif /* DETECT_OS_OHOS */

#endif /* RADV_OHOS_H */
