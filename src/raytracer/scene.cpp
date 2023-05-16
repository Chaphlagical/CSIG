#include "render/scene.hpp"
#include "core/log.hpp"
#include "render/context.hpp"

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <stb/stb_image.h>

#include <filesystem>
#include <fstream>
#include <queue>

#define CUBEMAP_SIZE 1024
#define IRRADIANCE_CUBEMAP_SIZE 128
#define IRRADIANCE_WORK_GROUP_SIZE 8
#define SH_INTERMEDIATE_SIZE (IRRADIANCE_CUBEMAP_SIZE / IRRADIANCE_WORK_GROUP_SIZE)
#define CUBEMAP_FACE_NUM 6
#define PREFILTER_MAP_SIZE 256
#define PREFILTER_MIP_LEVELS 5

static unsigned char g_equirectangular_to_cubemap_vert_spv_data[] = {
#include "equirectangular_to_cubemap.vert.spv.h"
};

static unsigned char g_equirectangular_to_cubemap_frag_spv_data[] = {
#include "equirectangular_to_cubemap.frag.spv.h"
};

static unsigned char g_cubemap_sh_projection_comp_spv_data[] = {
#include "cubemap_sh_projection.comp.spv.h"
};

static unsigned char g_cubemap_sh_add_comp_spv_data[] = {
#include "cubemap_sh_add.comp.spv.h"
};

static unsigned char g_cubemap_prefilter_comp_spv_data[] = {
#include "cubemap_prefilter.comp.spv.h"
};

struct Mesh
{
	uint32_t vertices_offset = 0;
	uint32_t vertices_count  = 0;
	uint32_t indices_offset  = 0;
	uint32_t indices_count   = 0;
	uint32_t material        = ~0u;
	float    area            = 0.f;
};

static inline size_t align(size_t x, size_t alignment)
{
	return (x + (alignment - 1)) & ~(alignment - 1);
}

inline const std::string get_path_dictionary(const std::string &path)
{
	if (std::filesystem::exists(path) &&
	    std::filesystem::is_directory(path))
	{
		return path;
	}

	size_t last_index = path.find_last_of("\\/");

	if (last_index != std::string::npos)
	{
		return path.substr(0, last_index + 1);
	}

	return "";
}

inline uint32_t load_texture(const Context &context, const std::string &filename, cgltf_texture *gltf_texture, std::vector<Texture> &textures, std::vector<VkImageView> &texture_views, std::unordered_map<cgltf_texture *, uint32_t> &texture_map)
{
	if (!gltf_texture)
	{
		return ~0u;
	}
	if (texture_map.find(gltf_texture) != texture_map.end())
	{
		return texture_map.at(gltf_texture);
	}

	uint8_t *raw_data = nullptr;
	size_t   raw_size = 0;
	int32_t  width = 0, height = 0, channel = 0, req_channel = 4;

	if (gltf_texture->image->uri)
	{
		// Load external texture
		std::string path = get_path_dictionary(filename) + gltf_texture->image->uri;
		raw_data         = stbi_load(path.c_str(), &width, &height, &channel, req_channel);
	}
	else if (gltf_texture->image->buffer_view)
	{
		// Load internal texture
		uint8_t *data = static_cast<uint8_t *>(gltf_texture->image->buffer_view->buffer->data) + gltf_texture->image->buffer_view->offset;
		size_t   size = gltf_texture->image->buffer_view->size;

		raw_data = stbi_load_from_memory(static_cast<stbi_uc *>(data), static_cast<int32_t>(size), &width, &height, &channel, req_channel);
	}

	raw_size = static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(req_channel) * sizeof(uint8_t);

	uint32_t mip_level = static_cast<uint32_t>(std::floor(std::log2(std::max(width, height))) + 1);

	// Create texture
	Texture texture;
	{
		VkImageCreateInfo image_create_info = {
		    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		    .imageType     = VK_IMAGE_TYPE_2D,
		    .format        = VK_FORMAT_R8G8B8A8_UNORM,
		    .extent        = VkExtent3D{static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1},
		    .mipLevels     = mip_level,
		    .arrayLayers   = 1,
		    .samples       = VK_SAMPLE_COUNT_1_BIT,
		    .tiling        = VK_IMAGE_TILING_OPTIMAL,
		    .usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		    .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
		    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};
		VmaAllocationCreateInfo allocation_create_info = {
		    .usage = VMA_MEMORY_USAGE_GPU_ONLY,
		};
		vmaCreateImage(context.vma_allocator, &image_create_info, &allocation_create_info, &texture.vk_image, &texture.vma_allocation, nullptr);
	}

	// Create staging buffer
	Buffer staging_buffer;
	{
		VkBufferCreateInfo buffer_create_info = {
		    .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		    .size        = raw_size,
		    .usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};
		VmaAllocationCreateInfo allocation_create_info = {
		    .usage = VMA_MEMORY_USAGE_CPU_TO_GPU};
		VmaAllocationInfo allocation_info = {};
		vmaCreateBuffer(context.vma_allocator, &buffer_create_info, &allocation_create_info, &staging_buffer.vk_buffer, &staging_buffer.vma_allocation, &allocation_info);
	}

	// Copy host data to device
	{
		uint8_t *mapped_data = nullptr;
		vmaMapMemory(context.vma_allocator, staging_buffer.vma_allocation, reinterpret_cast<void **>(&mapped_data));
		std::memcpy(mapped_data, raw_data, raw_size);
		vmaUnmapMemory(context.vma_allocator, staging_buffer.vma_allocation);
		vmaFlushAllocation(context.vma_allocator, staging_buffer.vma_allocation, 0, raw_size);
		mapped_data = nullptr;
	}

	// Allocate command buffer
	VkCommandBuffer cmd_buffer = VK_NULL_HANDLE;
	{
		VkCommandBufferAllocateInfo allocate_info =
		    {
		        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		        .commandPool        = context.graphics_cmd_pool,
		        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		        .commandBufferCount = 1,
		    };
		vkAllocateCommandBuffers(context.vk_device, &allocate_info, &cmd_buffer);
	}

	// Create fence
	VkFence fence = VK_NULL_HANDLE;
	{
		VkFenceCreateInfo create_info = {
		    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		    .flags = 0,
		};
		vkCreateFence(context.vk_device, &create_info, nullptr, &fence);
	}

	VkCommandBufferBeginInfo begin_info = {
	    .sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	    .flags            = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	    .pInheritanceInfo = nullptr,
	};

	vkBeginCommandBuffer(cmd_buffer, &begin_info);

	// Texture transition: undefined -> transfer dest
	{
		VkImageMemoryBarrier image_barrier = {
		    .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask       = 0,
		    .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
		    .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
		    .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		    .image               = texture.vk_image,
		    .subresourceRange    = VkImageSubresourceRange{
		           .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		           .baseMipLevel   = 0,
		           .levelCount     = mip_level,
		           .baseArrayLayer = 0,
		           .layerCount     = 1,
            },
		};
		vkCmdPipelineBarrier(
		    cmd_buffer,
		    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		    0, 0, nullptr, 0, nullptr, 1, &image_barrier);
	}

	// Copy buffer to texture
	{
		VkBufferImageCopy copy_info = {
		    .bufferOffset      = 0,
		    .bufferRowLength   = 0,
		    .bufferImageHeight = 0,
		    .imageSubresource  = VkImageSubresourceLayers{
		         .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		         .mipLevel       = 0,
		         .baseArrayLayer = 0,
		         .layerCount     = 1,
            },
		    .imageOffset = {0, 0, 0},
		    .imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1},
		};
		vkCmdCopyBufferToImage(cmd_buffer, staging_buffer.vk_buffer, texture.vk_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_info);
	}

	// Texture transition: transfer dest -> transfer src
	{
		VkImageMemoryBarrier image_barrier = {
		    .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
		    .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
		    .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		    .image               = texture.vk_image,
		    .subresourceRange    = VkImageSubresourceRange{
		           .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		           .baseMipLevel   = 0,
		           .levelCount     = mip_level,
		           .baseArrayLayer = 0,
		           .layerCount     = 1,
            },
		};
		vkCmdPipelineBarrier(
		    cmd_buffer,
		    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		    0, 0, nullptr, 0, nullptr, 1, &image_barrier);
	}

	// Generate mipmap
	{
		for (uint32_t i = 1; i < mip_level; i++)
		{
			VkImageBlit blit_info = {
			    .srcSubresource = VkImageSubresourceLayers{
			        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
			        .mipLevel       = i - 1,
			        .baseArrayLayer = 0,
			        .layerCount     = 1,
			    },
			    .srcOffsets = {
			        VkOffset3D{
			            .x = 0,
			            .y = 0,
			            .z = 0,
			        },
			        VkOffset3D{
			            .x = static_cast<int32_t>(width >> (i - 1)),
			            .y = static_cast<int32_t>(height >> (i - 1)),
			            .z = 1,
			        },
			    },
			    .dstSubresource = VkImageSubresourceLayers{
			        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
			        .mipLevel       = i,
			        .baseArrayLayer = 0,
			        .layerCount     = 1,
			    },
			    .dstOffsets = {
			        VkOffset3D{
			            .x = 0,
			            .y = 0,
			            .z = 0,
			        },
			        VkOffset3D{
			            .x = static_cast<int32_t>(width >> i),
			            .y = static_cast<int32_t>(height >> i),
			            .z = 1,
			        },
			    },
			};

			{
				VkImageMemoryBarrier image_barrier = VkImageMemoryBarrier{
				    .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				    .srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
				    .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
				    .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				    .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				    .image               = texture.vk_image,
				    .subresourceRange    = VkImageSubresourceRange{
				           .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
				           .baseMipLevel   = i,
				           .levelCount     = 1,
				           .baseArrayLayer = 0,
				           .layerCount     = 1,
                    },
				};
				vkCmdPipelineBarrier(
				    cmd_buffer,
				    VK_PIPELINE_STAGE_TRANSFER_BIT,
				    VK_PIPELINE_STAGE_TRANSFER_BIT,
				    0, 0, nullptr, 0, nullptr, 1, &image_barrier);
			}

			vkCmdBlitImage(
			    cmd_buffer,
			    texture.vk_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			    texture.vk_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    1, &blit_info, VK_FILTER_LINEAR);

			{
				VkImageMemoryBarrier image_barrier = VkImageMemoryBarrier{
				    .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				    .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
				    .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
				    .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				    .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				    .image               = texture.vk_image,
				    .subresourceRange    = VkImageSubresourceRange{
				           .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
				           .baseMipLevel   = i,
				           .levelCount     = 1,
				           .baseArrayLayer = 0,
				           .layerCount     = 1,
                    },
				};
				vkCmdPipelineBarrier(
				    cmd_buffer,
				    VK_PIPELINE_STAGE_TRANSFER_BIT,
				    VK_PIPELINE_STAGE_TRANSFER_BIT,
				    0, 0, nullptr, 0, nullptr, 1, &image_barrier);
			}
		}
	}

	// Texture transition: transfer dest -> shader read
	{
		VkImageMemoryBarrier image_barrier =
		    {
		        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
		        .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
		        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image               = texture.vk_image,
		        .subresourceRange    = VkImageSubresourceRange{
		               .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		               .baseMipLevel   = 0,
		               .levelCount     = mip_level,
		               .baseArrayLayer = 0,
		               .layerCount     = 1,
                },
		    };
		vkCmdPipelineBarrier(
		    cmd_buffer,
		    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		    0, 0, nullptr, 0, nullptr, 1, &image_barrier);
	}

	vkEndCommandBuffer(cmd_buffer);

	// Submit command buffer
	{
		VkSubmitInfo submit_info = {
		    .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		    .waitSemaphoreCount   = 0,
		    .pWaitSemaphores      = nullptr,
		    .pWaitDstStageMask    = 0,
		    .commandBufferCount   = 1,
		    .pCommandBuffers      = &cmd_buffer,
		    .signalSemaphoreCount = 0,
		    .pSignalSemaphores    = nullptr,
		};
		vkQueueSubmit(context.graphics_queue, 1, &submit_info, fence);
	}

	// Wait
	vkWaitForFences(context.vk_device, 1, &fence, VK_TRUE, UINT64_MAX);
	vkResetFences(context.vk_device, 1, &fence);

	// Release resource
	vkDestroyFence(context.vk_device, fence, nullptr);
	vkFreeCommandBuffers(context.vk_device, context.graphics_cmd_pool, 1, &cmd_buffer);
	vmaDestroyBuffer(context.vma_allocator, staging_buffer.vk_buffer, staging_buffer.vma_allocation);

	stbi_image_free(raw_data);

	textures.push_back(texture);
	texture_map[gltf_texture] = static_cast<uint32_t>(textures.size() - 1);

	// Create views
	{
		VkImageView           view             = VK_NULL_HANDLE;
		VkImageViewCreateInfo view_create_info = {
		    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image            = texture.vk_image,
		    .viewType         = VK_IMAGE_VIEW_TYPE_2D,
		    .format           = VK_FORMAT_R8G8B8A8_UNORM,
		    .components       = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
		    .subresourceRange = {
		        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		        .baseMipLevel   = 0,
		        .levelCount     = mip_level,
		        .baseArrayLayer = 0,
		        .layerCount     = 1,
		    },
		};
		vkCreateImageView(context.vk_device, &view_create_info, nullptr, &view);
		texture_views.push_back(view);
	}

	return texture_map.at(gltf_texture);
}

inline Buffer create_buffer(const Context &context, VkBufferUsageFlags usage, void *data, size_t size)
{
	Buffer result;
	{
		VkBufferCreateInfo buffer_create_info = {
		    .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		    .size        = size,
		    .usage       = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};
		VmaAllocationCreateInfo allocation_create_info = {
		    .usage = VMA_MEMORY_USAGE_GPU_ONLY,
		};
		VmaAllocationInfo allocation_info = {};
		vmaCreateBuffer(context.vma_allocator, &buffer_create_info, &allocation_create_info, &result.vk_buffer, &result.vma_allocation, &allocation_info);
		if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
		{
			VkBufferDeviceAddressInfoKHR buffer_device_address_info = {
			    .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
			    .buffer = result.vk_buffer,
			};
			result.device_address = vkGetBufferDeviceAddress(context.vk_device, &buffer_device_address_info);
		}
	}

	Buffer staging_buffer;
	{
		VkBufferCreateInfo buffer_create_info = {
		    .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		    .size        = size,
		    .usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};
		VmaAllocationCreateInfo allocation_create_info = {
		    .usage = VMA_MEMORY_USAGE_CPU_TO_GPU};
		VmaAllocationInfo allocation_info = {};
		vmaCreateBuffer(context.vma_allocator, &buffer_create_info, &allocation_create_info, &staging_buffer.vk_buffer, &staging_buffer.vma_allocation, &allocation_info);
	}

	if (data)
	{
		uint8_t *mapped_data = nullptr;
		vmaMapMemory(context.vma_allocator, staging_buffer.vma_allocation, reinterpret_cast<void **>(&mapped_data));
		std::memcpy(mapped_data, data, size);
		vmaUnmapMemory(context.vma_allocator, staging_buffer.vma_allocation);
		vmaFlushAllocation(context.vma_allocator, staging_buffer.vma_allocation, 0, size);
		mapped_data = nullptr;
	}

	// Allocate command buffer
	VkCommandBuffer cmd_buffer = VK_NULL_HANDLE;
	{
		VkCommandBufferAllocateInfo allocate_info =
		    {
		        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		        .commandPool        = context.graphics_cmd_pool,
		        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		        .commandBufferCount = 1,
		    };
		vkAllocateCommandBuffers(context.vk_device, &allocate_info, &cmd_buffer);
	}

	// Create fence
	VkFence fence = VK_NULL_HANDLE;
	{
		VkFenceCreateInfo create_info = {
		    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		    .flags = 0,
		};
		vkCreateFence(context.vk_device, &create_info, nullptr, &fence);
	}

	VkCommandBufferBeginInfo begin_info = {
	    .sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	    .flags            = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	    .pInheritanceInfo = nullptr,
	};

	vkBeginCommandBuffer(cmd_buffer, &begin_info);

	{
		VkBufferCopy copy_info = {
		    .srcOffset = 0,
		    .dstOffset = 0,
		    .size      = size,
		};
		vkCmdCopyBuffer(cmd_buffer, staging_buffer.vk_buffer, result.vk_buffer, 1, &copy_info);
	}

	vkEndCommandBuffer(cmd_buffer);

	// Submit command buffer
	{
		VkSubmitInfo submit_info = {
		    .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		    .waitSemaphoreCount   = 0,
		    .pWaitSemaphores      = nullptr,
		    .pWaitDstStageMask    = 0,
		    .commandBufferCount   = 1,
		    .pCommandBuffers      = &cmd_buffer,
		    .signalSemaphoreCount = 0,
		    .pSignalSemaphores    = nullptr,
		};
		vkQueueSubmit(context.graphics_queue, 1, &submit_info, fence);
	}

	// Wait
	vkWaitForFences(context.vk_device, 1, &fence, VK_TRUE, UINT64_MAX);
	vkResetFences(context.vk_device, 1, &fence);

	// Release resource
	vkDestroyFence(context.vk_device, fence, nullptr);
	vkFreeCommandBuffers(context.vk_device, context.graphics_cmd_pool, 1, &cmd_buffer);
	vmaDestroyBuffer(context.vma_allocator, staging_buffer.vk_buffer, staging_buffer.vma_allocation);

	return result;
}

inline AccelerationStructure create_acceleration_structure(const Context &context, VkAccelerationStructureTypeKHR type, VkAccelerationStructureBuildSizesInfoKHR build_size_info)
{
	AccelerationStructure as                 = {};
	VkBufferCreateInfo    buffer_create_info = {
	       .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
	       .size  = build_size_info.accelerationStructureSize,
	       .usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    };
	VmaAllocationCreateInfo allocation_create_info = {
	    .usage = VMA_MEMORY_USAGE_GPU_ONLY,
	};
	VmaAllocationInfo allocation_info = {};
	vmaCreateBuffer(context.vma_allocator, &buffer_create_info, &allocation_create_info, &as.buffer.vk_buffer, &as.buffer.vma_allocation, &allocation_info);
	VkAccelerationStructureCreateInfoKHR as_create_info = {
	    .sType  = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
	    .buffer = as.buffer.vk_buffer,
	    .size   = build_size_info.accelerationStructureSize,
	    .type   = type,
	};
	vkCreateAccelerationStructureKHR(context.vk_device, &as_create_info, nullptr, &as.vk_as);
	VkAccelerationStructureDeviceAddressInfoKHR as_device_address_info = {
	    .sType                 = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
	    .accelerationStructure = as.vk_as,
	};
	as.device_address = vkGetAccelerationStructureDeviceAddressKHR(context.vk_device, &as_device_address_info);
	return as;
}

inline Buffer create_scratch_buffer(const Context &context, VkDeviceSize size)
{
	VkPhysicalDeviceAccelerationStructurePropertiesKHR properties = {};

	properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
	properties.pNext = NULL;

	VkPhysicalDeviceProperties2 dev_props2 = {};

	dev_props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	dev_props2.pNext = &properties;

	vkGetPhysicalDeviceProperties2(context.vk_physical_device, &dev_props2);

	Buffer buffer = {};

	VkBufferCreateInfo buffer_create_info = {
	    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
	    .size  = size_t(std::ceil(size / properties.minAccelerationStructureScratchOffsetAlignment) + 1) * properties.minAccelerationStructureScratchOffsetAlignment,
	    .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
	};
	VmaAllocationCreateInfo allocation_create_info = {
	    .usage = VMA_MEMORY_USAGE_GPU_ONLY,
	};
	VmaAllocationInfo allocation_info = {};
	vmaCreateBuffer(context.vma_allocator, &buffer_create_info, &allocation_create_info, &buffer.vk_buffer, &buffer.vma_allocation, &allocation_info);
	VkBufferDeviceAddressInfoKHR buffer_device_address_info = {
	    .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
	    .buffer = buffer.vk_buffer,
	};
	buffer.device_address = vkGetBufferDeviceAddressKHR(context.vk_device, &buffer_device_address_info);
	buffer.device_address = align(buffer.device_address, properties.minAccelerationStructureScratchOffsetAlignment);
	return buffer;
}

inline std::vector<AliasTable> create_alias_table_buffer(const Context &context, std::vector<float> &probs, float total_weight)
{
	std::vector<AliasTable> alias_table(probs.size());
	std::queue<uint32_t>    greater_queue;
	std::queue<uint32_t>    smaller_queue;
	for (uint32_t i = 0; i < probs.size(); i++)
	{
		alias_table[i].ori_prob = probs[i] / total_weight;
		probs[i] *= static_cast<float>(probs.size()) / total_weight;
		if (probs[i] >= 1.f)
		{
			greater_queue.push(i);
		}
		else
		{
			smaller_queue.push(i);
		}
	}
	while (!greater_queue.empty() && !smaller_queue.empty())
	{
		uint32_t g = greater_queue.front();
		uint32_t s = smaller_queue.front();

		greater_queue.pop();
		smaller_queue.pop();

		alias_table[s].prob  = probs[s];
		alias_table[s].alias = g;

		probs[g] = (probs[s] + probs[g]) - 1.f;

		if (probs[g] < 1.f)
		{
			smaller_queue.push(g);
		}
		else
		{
			greater_queue.push(g);
		}
	}
	while (!greater_queue.empty())
	{
		uint32_t g = greater_queue.front();
		greater_queue.pop();
		alias_table[g].prob  = 1.f;
		alias_table[g].alias = g;
	}
	while (!greater_queue.empty())
	{
		uint32_t s = smaller_queue.front();
		smaller_queue.pop();
		alias_table[s].prob  = 1.f;
		alias_table[s].alias = s;
	}

	for (auto &table : alias_table)
	{
		table.alias_ori_prob = alias_table[table.alias].ori_prob;
	}

	return alias_table;
}

Scene::Scene(const Context &context) :
    m_context(&context)
{
	// Create sampler
	{
		VkSamplerCreateInfo create_info = {
		    .sType            = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		    .magFilter        = VK_FILTER_LINEAR,
		    .minFilter        = VK_FILTER_LINEAR,
		    .mipmapMode       = VK_SAMPLER_MIPMAP_MODE_LINEAR,
		    .addressModeU     = VK_SAMPLER_ADDRESS_MODE_REPEAT,
		    .addressModeV     = VK_SAMPLER_ADDRESS_MODE_REPEAT,
		    .addressModeW     = VK_SAMPLER_ADDRESS_MODE_REPEAT,
		    .mipLodBias       = 0.f,
		    .anisotropyEnable = VK_FALSE,
		    .maxAnisotropy    = 1.f,
		    .compareEnable    = VK_FALSE,
		    .compareOp        = VK_COMPARE_OP_NEVER,
		    .minLod           = 0.f,
		    .maxLod           = 12.f,
		    .borderColor      = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
		};
		vkCreateSampler(m_context->vk_device, &create_info, nullptr, &linear_sampler);
		create_info.magFilter  = VK_FILTER_NEAREST;
		create_info.minFilter  = VK_FILTER_NEAREST;
		create_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		vkCreateSampler(m_context->vk_device, &create_info, nullptr, &nearest_sampler);
	}

	// Create global buffer
	{
		VkBufferCreateInfo create_info = {
		    .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		    .size        = sizeof(GlobalData),
		    .usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};
		VmaAllocationCreateInfo allocation_create_info = {
		    .usage = VMA_MEMORY_USAGE_CPU_TO_GPU};
		VmaAllocationInfo allocation_info = {};
		vmaCreateBuffer(m_context->vma_allocator, &create_info, &allocation_create_info, &global_buffer.vk_buffer, &global_buffer.vma_allocation, &allocation_info);
	}

	// Create descriptor set layout
	{
		VkDescriptorBindingFlags descriptor_binding_flags[] = {
		    0,
		    0,
		    0,
		    VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
		    0,
		};
		VkDescriptorSetLayoutBinding bindings[] = {
		    // Global buffer
		    {
		        .binding         = 0,
		        .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		        .descriptorCount = 1,
		        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		    },
		    // Top Levell Acceleration Structure
		    {
		        .binding         = 1,
		        .descriptorType  = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
		        .descriptorCount = 1,
		        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		    },
		    // Scene Buffer
		    {
		        .binding         = 2,
		        .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		        .descriptorCount = 1,
		        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		    },
		    // Bindless textures
		    {
		        .binding         = 3,
		        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .descriptorCount = 1024,
		        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		    },
		    // Skybox
		    {
		        .binding         = 4,
		        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .descriptorCount = 1,
		        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		    },
		};
		VkDescriptorSetLayoutBindingFlagsCreateInfo descriptor_set_layout_binding_flag_create_info = {
		    .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
		    .bindingCount  = 5,
		    .pBindingFlags = descriptor_binding_flags,
		};
		VkDescriptorSetLayoutCreateInfo create_info = {
		    .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		    .pNext        = &descriptor_set_layout_binding_flag_create_info,
		    .flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
		    .bindingCount = 5,
		    .pBindings    = bindings,
		};
		vkCreateDescriptorSetLayout(m_context->vk_device, &create_info, nullptr, &descriptor.layout);
	}

	// Allocate descriptor set
	{
		VkDescriptorSetAllocateInfo allocate_info = {
		    .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		    .pNext              = nullptr,
		    .descriptorPool     = m_context->vk_descriptor_pool,
		    .descriptorSetCount = 1,
		    .pSetLayouts        = &descriptor.layout,
		};
		vkAllocateDescriptorSets(m_context->vk_device, &allocate_info, &descriptor.set);
	}
}

Scene::~Scene()
{
	destroy_scene();
	destroy_envmap();

	vkDestroySampler(m_context->vk_device, linear_sampler, nullptr);
	vkDestroySampler(m_context->vk_device, nearest_sampler, nullptr);
	vmaDestroyBuffer(m_context->vma_allocator, global_buffer.vk_buffer, global_buffer.vma_allocation);
	vkDestroyDescriptorSetLayout(m_context->vk_device, descriptor.layout, nullptr);
	vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 1, &descriptor.set);
}

void Scene::load_scene(const std::string &filename)
{
	vkDeviceWaitIdle(m_context->vk_device);
	destroy_scene();

	cgltf_options options  = {};
	cgltf_data   *raw_data = nullptr;
	cgltf_result  result   = cgltf_parse_file(&options, filename.c_str(), &raw_data);
	if (result != cgltf_result_success)
	{
		LOG_WARN("Failed to load gltf {}", filename);
		return;
	}
	result = cgltf_load_buffers(&options, raw_data, filename.c_str());
	if (result != cgltf_result_success)
	{
		LOG_WARN("Failed to load gltf {}", filename);
		return;
	}

	std::unordered_map<cgltf_texture *, uint32_t>           texture_map;
	std::unordered_map<cgltf_material *, uint32_t>          material_map;
	std::unordered_map<cgltf_mesh *, std::vector<uint32_t>> mesh_map;

	std::vector<Emitter>  emitters;
	std::vector<Material> materials;
	std::vector<Mesh>     meshes;
	std::vector<Instance> instances;
	std::vector<uint32_t> indices;
	std::vector<Vertex>   vertices;

	// Load material
	{
		for (size_t i = 0; i < raw_data->materials_count; i++)
		{
			auto    &raw_material = raw_data->materials[i];
			Material material;
			material.normal_texture = load_texture(*m_context, filename, raw_material.normal_texture.texture, textures, texture_views, texture_map);
			material.double_sided   = raw_material.double_sided;
			material.alpha_mode     = raw_material.alpha_mode;
			material.cutoff         = raw_material.alpha_cutoff;
			std::memcpy(glm::value_ptr(material.emissive_factor), raw_material.emissive_factor, sizeof(glm::vec3));
			if (raw_material.has_pbr_metallic_roughness)
			{
				material.metallic_factor  = raw_material.pbr_metallic_roughness.metallic_factor;
				material.roughness_factor = raw_material.pbr_metallic_roughness.roughness_factor;
				std::memcpy(glm::value_ptr(material.base_color), raw_material.pbr_metallic_roughness.base_color_factor, sizeof(glm::vec4));
				material.base_color_texture         = load_texture(*m_context, filename, raw_material.pbr_metallic_roughness.base_color_texture.texture, textures, texture_views, texture_map);
				material.metallic_roughness_texture = load_texture(*m_context, filename, raw_material.pbr_metallic_roughness.metallic_roughness_texture.texture, textures, texture_views, texture_map);
			}
			if (raw_material.has_clearcoat)
			{
				material.clearcoat_factor           = raw_material.clearcoat.clearcoat_factor;
				material.clearcoat_roughness_factor = raw_material.clearcoat.clearcoat_roughness_factor;
			}
			if (raw_material.has_transmission)
			{
				material.transmission_factor = raw_material.transmission.transmission_factor;
			}

			materials.emplace_back(material);
			material_map[&raw_material] = static_cast<uint32_t>(materials.size() - 1);
		}

		// Create material buffer
		{
			material_buffer                 = create_buffer(*m_context, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, materials.data(), materials.size() * sizeof(Material));
			scene_info.material_count       = static_cast<uint32_t>(materials.size());
			scene_info.material_buffer_addr = material_buffer.device_address;
		}
	}

	// Load geometry
	{
		for (uint32_t mesh_id = 0; mesh_id < raw_data->meshes_count; mesh_id++)
		{
			auto &raw_mesh      = raw_data->meshes[mesh_id];
			mesh_map[&raw_mesh] = {};
			for (uint32_t prim_id = 0; prim_id < raw_mesh.primitives_count; prim_id++)
			{
				const cgltf_primitive &primitive = raw_mesh.primitives[prim_id];

				if (!primitive.indices)
				{
					continue;
				}

				Mesh mesh = {
				    .vertices_offset = static_cast<uint32_t>(vertices.size()),
				    .vertices_count  = 0,
				    .indices_offset  = static_cast<uint32_t>(indices.size()),
				    .indices_count   = static_cast<uint32_t>(primitive.indices->count),
				    .material        = material_map.at(primitive.material),
				    .area            = 0.f,
				};

				indices.resize(indices.size() + primitive.indices->count);
				for (size_t i = 0; i < primitive.indices->count; i++)
				{
					indices[mesh.indices_offset + i] = static_cast<uint32_t>(cgltf_accessor_read_index(primitive.indices, i));
				}

				for (size_t attr_id = 0; attr_id < primitive.attributes_count; attr_id++)
				{
					const cgltf_attribute &attribute = primitive.attributes[attr_id];

					const char *attr_name = attribute.name;

					if (strcmp(attr_name, "POSITION") == 0)
					{
						mesh.vertices_count = std::max(mesh.vertices_count, static_cast<uint32_t>(attribute.data->count));
						vertices.resize(mesh.vertices_offset + attribute.data->count);
						for (size_t i = 0; i < attribute.data->count; ++i)
						{
							cgltf_accessor_read_float(attribute.data, i, &vertices[mesh.vertices_offset + i].position.x, 3);
						}
					}
					else if (strcmp(attr_name, "NORMAL") == 0)
					{
						mesh.vertices_count = std::max(mesh.vertices_count, static_cast<uint32_t>(attribute.data->count));
						vertices.resize(mesh.vertices_offset + attribute.data->count);
						for (size_t i = 0; i < attribute.data->count; ++i)
						{
							cgltf_accessor_read_float(attribute.data, i, &vertices[mesh.vertices_offset + i].normal.x, 3);
						}
					}
					else if (strcmp(attr_name, "TEXCOORD_0") == 0)
					{
						mesh.vertices_count = std::max(mesh.vertices_count, static_cast<uint32_t>(attribute.data->count));
						vertices.resize(mesh.vertices_offset + attribute.data->count);
						for (size_t i = 0; i < attribute.data->count; ++i)
						{
							glm::vec2 texcoord = glm::vec2(0);
							cgltf_accessor_read_float(attribute.data, i, &texcoord.x, 2);
							vertices[mesh.vertices_offset + i].position.w = texcoord.x;
							vertices[mesh.vertices_offset + i].normal.w   = texcoord.y;
						}
					}
				}

				meshes.push_back(mesh);
				mesh_map[&raw_mesh].push_back(static_cast<uint32_t>(meshes.size() - 1));
			}
		}

		scene_info.vertices_count = static_cast<uint32_t>(vertices.size());
		scene_info.indices_count  = static_cast<uint32_t>(indices.size());
		scene_info.mesh_count     = static_cast<uint32_t>(meshes.size());

		vertex_buffer = create_buffer(*m_context, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, vertices.data(), vertices.size() * sizeof(Vertex));
		index_buffer  = create_buffer(*m_context, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, indices.data(), indices.size() * sizeof(uint32_t));

		scene_info.vertex_buffer_addr = vertex_buffer.device_address;
		scene_info.index_buffer_addr  = index_buffer.device_address;

		m_context->set_object_name(
		    VK_OBJECT_TYPE_BUFFER,
		    (uint64_t) vertex_buffer.vk_buffer,
		    "Vertex Buffer");

		m_context->set_object_name(
		    VK_OBJECT_TYPE_BUFFER,
		    (uint64_t) index_buffer.vk_buffer,
		    "Index Buffer");
	}

	// Build mesh alias table buffer
	{
		std::vector<AliasTable> alias_table;
		for (uint32_t i = 0; i < meshes.size(); i++)
		{
			float              total_weight = 0.f;
			std::vector<float> mesh_probs(meshes[i].indices_count / 3);
			for (uint32_t j = 0; j < meshes[i].indices_count / 3; j++)
			{
				glm::vec3 v0 = vertices[meshes[i].vertices_offset + indices[meshes[i].indices_offset + 3 * j + 0]].position;
				glm::vec3 v1 = vertices[meshes[i].vertices_offset + indices[meshes[i].indices_offset + 3 * j + 1]].position;
				glm::vec3 v2 = vertices[meshes[i].vertices_offset + indices[meshes[i].indices_offset + 3 * j + 2]].position;
				mesh_probs[j] += glm::length(glm::cross(v1 - v0, v2 - v1)) * 0.5f;
				total_weight += mesh_probs[j];
			}
			meshes[i].area = total_weight;

			std::vector<AliasTable> mesh_alias_table = create_alias_table_buffer(*m_context, mesh_probs, total_weight);
			alias_table.insert(alias_table.end(), std::make_move_iterator(mesh_alias_table.begin()), std::make_move_iterator(mesh_alias_table.end()));
		}
		mesh_alias_table_buffer                 = create_buffer(*m_context, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, alias_table.data(), alias_table.size() * sizeof(AliasTable));
		scene_info.mesh_alias_table_buffer_addr = mesh_alias_table_buffer.device_address;
	}

	// Load hierarchy
	{
		for (size_t i = 0; i < raw_data->nodes_count; i++)
		{
			const cgltf_node &node = raw_data->nodes[i];

			cgltf_float matrix[16];
			cgltf_node_transform_world(&node, matrix);
			if (node.mesh)
			{
				for (auto &mesh_id : mesh_map.at(node.mesh))
				{
					const auto &mesh = meshes[mesh_id];

					Instance instance = {
					    .vertices_offset = mesh.vertices_offset,
					    .vertices_count  = mesh.vertices_offset,
					    .indices_offset  = mesh.indices_offset,
					    .indices_count   = mesh.indices_count,
					    .mesh            = mesh_id,
					    .material        = mesh.material,
					    .area            = mesh.area,
					};
					std::memcpy(glm::value_ptr(instance.transform), matrix, sizeof(instance.transform));
					instance.transform_inv = glm::inverse(instance.transform);
					int32_t emitter_offset = static_cast<int32_t>(emitters.size());
					if (materials[mesh.material].emissive_factor != glm::vec3(0.f))
					{
						for (uint32_t tri_idx = 0; tri_idx < mesh.indices_count / 3; tri_idx++)
						{
							const uint32_t i0 = indices[mesh.indices_offset + tri_idx * 3 + 0];
							const uint32_t i1 = indices[mesh.indices_offset + tri_idx * 3 + 1];
							const uint32_t i2 = indices[mesh.indices_offset + tri_idx * 3 + 2];

							glm::vec3 p0 = instance.transform * glm::vec4(glm::vec3(vertices[mesh.vertices_offset + i0].position), 1.f);
							glm::vec3 p1 = instance.transform * glm::vec4(glm::vec3(vertices[mesh.vertices_offset + i1].position), 1.f);
							glm::vec3 p2 = instance.transform * glm::vec4(glm::vec3(vertices[mesh.vertices_offset + i2].position), 1.f);

							emitters.push_back(
							    Emitter{
							        .p0 = glm::vec4(p0, materials[mesh.material].emissive_factor.r),
							        .p1 = glm::vec4(p1, materials[mesh.material].emissive_factor.g),
							        .p2 = glm::vec4(p2, materials[mesh.material].emissive_factor.b),
							    });
						}
						instance.emitter = emitter_offset;
					}
					else
					{
						instance.emitter = -1;
					}
					instances.push_back(instance);
				}
			}
		}
		scene_info.instance_count = static_cast<uint32_t>(instances.size());

		// Compute scene extent
		{
			scene_info.max_extent = -glm::vec3(std::numeric_limits<float>::infinity());
			scene_info.min_extent = glm::vec3(std::numeric_limits<float>::infinity());
			for (auto &instance : instances)
			{
				const auto &mesh = meshes[instance.mesh];
				for (uint32_t vertex_id = 0; vertex_id < mesh.vertices_count; vertex_id++)
				{
					glm::vec3 v           = vertices[vertex_id + mesh.vertices_offset].position;
					v                     = instance.transform * glm::vec4(v, 1.f);
					scene_info.max_extent = glm::max(scene_info.max_extent, v);
					scene_info.min_extent = glm::min(scene_info.min_extent, v);
				}
			}
		}

		// Build emitter buffer
		{
			emitter_buffer                 = create_buffer(*m_context, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, emitters.data(), std::max(emitters.size(), 1ull) * sizeof(Emitter));
			scene_info.emitter_count       = static_cast<uint32_t>(emitters.size());
			scene_info.emitter_buffer_addr = emitter_buffer.device_address;
		}

		// Build emitter alias table buffer
		{
			float              total_weight = 0.f;
			std::vector<float> emitter_probs(emitters.size());
			for (uint32_t i = 0; i < emitters.size(); i++)
			{
				glm::vec3 p0        = emitters[i].p0;
				glm::vec3 p1        = emitters[i].p1;
				glm::vec3 p2        = emitters[i].p2;

				glm::vec3 intensity = glm::vec3(emitters[i].p0.w, emitters[i].p1.w, emitters[i].p2.w);
				float     area      = glm::length(glm::cross(p1 - p0, p2 - p1)) * 0.5f;

				emitter_probs[i] = glm::dot(intensity, glm::vec3(0.212671f, 0.715160f, 0.072169f)) * area;
				total_weight += emitter_probs[i];
			}

			std::vector<AliasTable> alias_table        = create_alias_table_buffer(*m_context, emitter_probs, total_weight);
			emitter_alias_table_buffer                 = create_buffer(*m_context, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, alias_table.data(), std::max(alias_table.size(), 1ull) * sizeof(AliasTable));
			scene_info.emitter_alias_table_buffer_addr = emitter_alias_table_buffer.device_address;
		}

		// Build draw indirect command buffer
		{
			std::vector<VkDrawIndexedIndirectCommand> indirect_commands;
			indirect_commands.resize(instances.size());
			for (uint32_t instance_id = 0; instance_id < instances.size(); instance_id++)
			{
				const auto &mesh               = meshes[instances[instance_id].mesh];
				indirect_commands[instance_id] = VkDrawIndexedIndirectCommand{
				    .indexCount    = mesh.indices_count,
				    .instanceCount = 1,
				    .firstIndex    = mesh.indices_offset,
				    .vertexOffset  = static_cast<int32_t>(mesh.vertices_offset),
				    .firstInstance = instance_id,
				};
			}
			VkBufferCreateInfo buffer_create_info = {
			    .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			    .size        = indirect_commands.size() * sizeof(VkDrawIndexedIndirectCommand),
			    .usage       = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
			    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
			};
			VmaAllocationCreateInfo allocation_create_info = {
			    .usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
			};
			VmaAllocationInfo allocation_info = {};
			vmaCreateBuffer(m_context->vma_allocator, &buffer_create_info, &allocation_create_info, &indirect_draw_buffer.vk_buffer, &indirect_draw_buffer.vma_allocation, &allocation_info);
			{
				uint8_t *mapped_data = nullptr;
				vmaMapMemory(m_context->vma_allocator, indirect_draw_buffer.vma_allocation, reinterpret_cast<void **>(&mapped_data));
				std::memcpy(mapped_data, indirect_commands.data(), indirect_commands.size() * sizeof(VkDrawIndexedIndirectCommand));
				vmaUnmapMemory(m_context->vma_allocator, indirect_draw_buffer.vma_allocation);
				vmaFlushAllocation(m_context->vma_allocator, indirect_draw_buffer.vma_allocation, 0, indirect_commands.size() * sizeof(VkDrawIndexedIndirectCommand));
				mapped_data = nullptr;
			}
		}

		// Create instance buffer
		{
			instance_buffer                 = create_buffer(*m_context, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, instances.data(), instances.size() * sizeof(Instance));
			scene_info.instance_buffer_addr = instance_buffer.device_address;
		}
	}

	// Build acceleration structure
	{
		std::vector<Buffer> scratch_buffers;
		// Build bottom level acceleration structure
		{
			blas.reserve(meshes.size());
			for (auto &mesh : meshes)
			{
				VkAccelerationStructureGeometryKHR as_geometry = {
				    .sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
				    .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
				    .geometry     = {
				            .triangles = {
				                .sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
				                .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
				                .vertexData   = {
				                      .deviceAddress = vertex_buffer.device_address,
                            },
				                .vertexStride = sizeof(Vertex),
				                .maxVertex    = mesh.vertices_count,
				                .indexType    = VK_INDEX_TYPE_UINT32,
				                .indexData    = {
				                       .deviceAddress = index_buffer.device_address,
                            },
				                .transformData = {
				                    .deviceAddress = 0,
                            },
                        },
                    },
				    .flags = VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR,
				};

				VkAccelerationStructureBuildRangeInfoKHR range_info = {
				    .primitiveCount  = mesh.indices_count / 3,
				    .primitiveOffset = mesh.indices_offset * sizeof(uint32_t),
				    .firstVertex     = mesh.vertices_offset,
				    .transformOffset = 0,
				};

				VkAccelerationStructureBuildGeometryInfoKHR build_geometry_info = {
				    .sType                    = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
				    .type                     = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
				    .flags                    = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
				    .mode                     = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
				    .srcAccelerationStructure = VK_NULL_HANDLE,
				    .geometryCount            = 1,
				    .pGeometries              = &as_geometry,
				    .ppGeometries             = nullptr,
				    .scratchData              = {
				                     .deviceAddress = 0,
                    },
				};

				VkAccelerationStructureBuildSizesInfoKHR build_sizes_info = {
				    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
				};

				vkGetAccelerationStructureBuildSizesKHR(
				    m_context->vk_device,
				    VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
				    &build_geometry_info,
				    &range_info.primitiveCount,
				    &build_sizes_info);

				AccelerationStructure as = create_acceleration_structure(*m_context, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, build_sizes_info);

				Buffer scratch_buffer = create_scratch_buffer(*m_context, build_sizes_info.buildScratchSize);

				build_geometry_info.scratchData.deviceAddress = scratch_buffer.device_address;
				build_geometry_info.dstAccelerationStructure  = as.vk_as;

				VkAccelerationStructureBuildRangeInfoKHR *as_build_range_infos = const_cast<VkAccelerationStructureBuildRangeInfoKHR *>(&range_info);

				VkCommandBuffer          cmd_buffer = m_context->create_command_buffer(true);
				VkCommandBufferBeginInfo begin_info = {
				    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
				    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
				};
				vkBeginCommandBuffer(cmd_buffer, &begin_info);
				vkCmdBuildAccelerationStructuresKHR(
				    cmd_buffer,
				    1,
				    &build_geometry_info,
				    &as_build_range_infos);
				vkEndCommandBuffer(cmd_buffer);
				m_context->flush_command_buffer(cmd_buffer, true);
				blas.push_back(as);
				scratch_buffers.push_back(scratch_buffer);
			}
		}

		// Build top level acceleration structure
		{
			std::vector<VkAccelerationStructureInstanceKHR> vk_instances;
			vk_instances.reserve(instances.size());
			for (uint32_t instance_id = 0; instance_id < instances.size(); instance_id++)
			{
				const auto &instance  = instances[instance_id];
				auto        transform = glm::mat3x4(glm::transpose(instance.transform));

				VkTransformMatrixKHR transform_matrix = {};
				std::memcpy(&transform_matrix, &transform, sizeof(VkTransformMatrixKHR));

				VkAccelerationStructureInstanceKHR vk_instance = {
				    .transform                              = transform_matrix,
				    .instanceCustomIndex                    = instance_id,
				    .mask                                   = 0xFF,
				    .instanceShaderBindingTableRecordOffset = 0,
				    .flags                                  = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR,
				    .accelerationStructureReference         = blas.at(instance.mesh).device_address,
				};

				const Material &material = materials[instance.material];

				if (material.alpha_mode == 0 ||
				    (material.base_color.w == 1.f &&
				     material.base_color_texture == ~0u))
				{
					vk_instance.flags |= VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
				}

				if (material.double_sided == 1)
				{
					vk_instance.flags |= VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
				}

				vk_instances.emplace_back(vk_instance);
			}

			Buffer instance_buffer = create_buffer(*m_context, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_TRANSFER_DST_BIT, vk_instances.data(), vk_instances.size() * sizeof(VkAccelerationStructureInstanceKHR));

			VkAccelerationStructureGeometryKHR as_geometry = {
			    .sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
			    .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
			    .geometry     = {
			            .instances = {
			                .sType           = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
			                .arrayOfPointers = VK_FALSE,
			                .data            = instance_buffer.device_address,
                    },
                },
			    .flags = 0,
			};

			VkAccelerationStructureBuildRangeInfoKHR range_info = {
			    .primitiveCount = static_cast<uint32_t>(vk_instances.size()),
			};

			VkAccelerationStructureBuildSizesInfoKHR build_sizes_info = {
			    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
			};

			VkAccelerationStructureBuildGeometryInfoKHR build_geometry_info = {
			    .sType                    = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
			    .type                     = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
			    .flags                    = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
			    .mode                     = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
			    .srcAccelerationStructure = VK_NULL_HANDLE,
			    .geometryCount            = 1,
			    .pGeometries              = &as_geometry,
			    .ppGeometries             = nullptr,
			    .scratchData              = {
			                     .deviceAddress = 0,
                },
			};

			vkGetAccelerationStructureBuildSizesKHR(
			    m_context->vk_device,
			    VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
			    &build_geometry_info,
			    &range_info.primitiveCount,
			    &build_sizes_info);

			tlas = create_acceleration_structure(*m_context, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, build_sizes_info);

			Buffer scratch_buffer = create_scratch_buffer(*m_context, build_sizes_info.buildScratchSize);

			build_geometry_info.scratchData.deviceAddress = scratch_buffer.device_address;
			build_geometry_info.dstAccelerationStructure  = tlas.vk_as;

			VkAccelerationStructureBuildRangeInfoKHR *as_build_range_infos = const_cast<VkAccelerationStructureBuildRangeInfoKHR *>(&range_info);

			VkCommandBuffer          cmd_buffer = m_context->create_command_buffer(true);
			VkCommandBufferBeginInfo begin_info = {
			    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
			};
			vkBeginCommandBuffer(cmd_buffer, &begin_info);
			vkCmdBuildAccelerationStructuresKHR(
			    cmd_buffer,
			    1,
			    &build_geometry_info,
			    &as_build_range_infos);
			vkEndCommandBuffer(cmd_buffer);
			m_context->flush_command_buffer(cmd_buffer, true);

			m_context->set_object_name(
			    VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR,
			    (uint64_t) tlas.vk_as,
			    "Scene TLAS");

			scratch_buffers.push_back(instance_buffer);
			scratch_buffers.push_back(scratch_buffer);
		}

		for (auto &scratch_buffer : scratch_buffers)
		{
			vmaDestroyBuffer(m_context->vma_allocator, scratch_buffer.vk_buffer, scratch_buffer.vma_allocation);
		}

		scratch_buffers.clear();
	}

	// Create scene buffer
	{
		VkBufferCreateInfo buffer_create_info = {
		    .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		    .size        = sizeof(scene_info),
		    .usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
		    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};
		VmaAllocationCreateInfo allocation_create_info = {
		    .usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
		};
		VmaAllocationInfo allocation_info = {};
		vmaCreateBuffer(m_context->vma_allocator, &buffer_create_info, &allocation_create_info, &scene_buffer.vk_buffer, &scene_buffer.vma_allocation, &allocation_info);
		{
			uint8_t *mapped_data = nullptr;
			vmaMapMemory(m_context->vma_allocator, scene_buffer.vma_allocation, reinterpret_cast<void **>(&mapped_data));
			std::memcpy(mapped_data, &scene_info, sizeof(scene_info));
			vmaUnmapMemory(m_context->vma_allocator, scene_buffer.vma_allocation);
			vmaFlushAllocation(m_context->vma_allocator, scene_buffer.vma_allocation, 0, sizeof(scene_info));
			mapped_data = nullptr;
		}
	}
}

void Scene::load_envmap(const std::string &filename)
{
	vkDeviceWaitIdle(m_context->vk_device);
	destroy_envmap();

	int32_t width = 0, height = 0, channel = 0, req_channel = 4;

	float *raw_data = stbi_loadf(filename.c_str(), &width, &height, &channel, req_channel);
	size_t raw_size = static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(req_channel) * sizeof(float);

	// Create hdr texture
	Texture texture;
	{
		VkImageCreateInfo image_create_info = {
		    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		    .imageType     = VK_IMAGE_TYPE_2D,
		    .format        = VK_FORMAT_R32G32B32A32_SFLOAT,
		    .extent        = VkExtent3D{static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1},
		    .mipLevels     = 1,
		    .arrayLayers   = 1,
		    .samples       = VK_SAMPLE_COUNT_1_BIT,
		    .tiling        = VK_IMAGE_TILING_OPTIMAL,
		    .usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		    .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
		    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};
		VmaAllocationCreateInfo allocation_create_info = {
		    .usage = VMA_MEMORY_USAGE_GPU_ONLY,
		};
		vmaCreateImage(m_context->vma_allocator, &image_create_info, &allocation_create_info, &texture.vk_image, &texture.vma_allocation, nullptr);
	}

	VkImageView texture_view = VK_NULL_HANDLE;
	{
		VkImageViewCreateInfo view_create_info = {
		    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image            = texture.vk_image,
		    .viewType         = VK_IMAGE_VIEW_TYPE_2D,
		    .format           = VK_FORMAT_R32G32B32A32_SFLOAT,
		    .components       = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
		    .subresourceRange = {
		        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		        .baseMipLevel   = 0,
		        .levelCount     = 1,
		        .baseArrayLayer = 0,
		        .layerCount     = 1,
		    },
		};
		vkCreateImageView(m_context->vk_device, &view_create_info, nullptr, &texture_view);
	}

	// Create staging buffer
	Buffer staging_buffer;
	{
		VkBufferCreateInfo buffer_create_info = {
		    .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		    .size        = raw_size,
		    .usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};
		VmaAllocationCreateInfo allocation_create_info = {
		    .usage = VMA_MEMORY_USAGE_CPU_TO_GPU};
		VmaAllocationInfo allocation_info = {};
		vmaCreateBuffer(m_context->vma_allocator, &buffer_create_info, &allocation_create_info, &staging_buffer.vk_buffer, &staging_buffer.vma_allocation, &allocation_info);
	}

	// Copy host data to device
	{
		uint8_t *mapped_data = nullptr;
		vmaMapMemory(m_context->vma_allocator, staging_buffer.vma_allocation, reinterpret_cast<void **>(&mapped_data));
		std::memcpy(mapped_data, raw_data, raw_size);
		vmaUnmapMemory(m_context->vma_allocator, staging_buffer.vma_allocation);
		vmaFlushAllocation(m_context->vma_allocator, staging_buffer.vma_allocation, 0, raw_size);
		mapped_data = nullptr;
	}

	// Create cubemap
	VkImageView cubemap_view_2d = VK_NULL_HANDLE;
	{
		VkImageCreateInfo image_create_info = {
		    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		    .flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
		    .imageType     = VK_IMAGE_TYPE_2D,
		    .format        = VK_FORMAT_R32G32B32A32_SFLOAT,
		    .extent        = VkExtent3D{CUBEMAP_SIZE, CUBEMAP_SIZE, 1},
		    .mipLevels     = 5,
		    .arrayLayers   = 6,
		    .samples       = VK_SAMPLE_COUNT_1_BIT,
		    .tiling        = VK_IMAGE_TILING_OPTIMAL,
		    .usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		    .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
		    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};
		VmaAllocationCreateInfo allocation_create_info = {
		    .usage = VMA_MEMORY_USAGE_GPU_ONLY,
		};
		vmaCreateImage(m_context->vma_allocator, &image_create_info, &allocation_create_info, &envmap.texture.vk_image, &envmap.texture.vma_allocation, nullptr);
		VkImageViewCreateInfo view_create_info = {
		    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image            = envmap.texture.vk_image,
		    .viewType         = VK_IMAGE_VIEW_TYPE_CUBE,
		    .format           = VK_FORMAT_R32G32B32A32_SFLOAT,
		    .components       = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
		    .subresourceRange = {
		        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		        .baseMipLevel   = 0,
		        .levelCount     = 5,
		        .baseArrayLayer = 0,
		        .layerCount     = 6,
		    },
		};
		vkCreateImageView(m_context->vk_device, &view_create_info, nullptr, &envmap.texture_view);
		view_create_info.viewType                    = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
		view_create_info.subresourceRange.levelCount = 1;
		vkCreateImageView(m_context->vk_device, &view_create_info, nullptr, &cubemap_view_2d);
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) envmap.texture.vk_image, "Envmap Texture");
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) envmap.texture_view, "Envmap Texture View");
	}

	// Create sh intermediate
	Texture     sh_intermediate;
	VkImageView sh_intermediate_view = VK_NULL_HANDLE;
	{
		VkImageCreateInfo image_create_info = {
		    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		    .imageType     = VK_IMAGE_TYPE_2D,
		    .format        = VK_FORMAT_R32G32B32A32_SFLOAT,
		    .extent        = VkExtent3D{SH_INTERMEDIATE_SIZE * 9, SH_INTERMEDIATE_SIZE, 1},
		    .mipLevels     = 1,
		    .arrayLayers   = 6,
		    .samples       = VK_SAMPLE_COUNT_1_BIT,
		    .tiling        = VK_IMAGE_TILING_OPTIMAL,
		    .usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
		    .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
		    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};
		VmaAllocationCreateInfo allocation_create_info = {
		    .usage = VMA_MEMORY_USAGE_GPU_ONLY,
		};
		vmaCreateImage(m_context->vma_allocator, &image_create_info, &allocation_create_info, &sh_intermediate.vk_image, &sh_intermediate.vma_allocation, nullptr);
		VkImageViewCreateInfo view_create_info = {
		    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image            = sh_intermediate.vk_image,
		    .viewType         = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
		    .format           = VK_FORMAT_R32G32B32A32_SFLOAT,
		    .components       = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
		    .subresourceRange = {
		        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		        .baseMipLevel   = 0,
		        .levelCount     = 1,
		        .baseArrayLayer = 0,
		        .layerCount     = 6,
		    },
		};
		vkCreateImageView(m_context->vk_device, &view_create_info, nullptr, &sh_intermediate_view);
	}

	// Create irradiance sh
	{
		VkImageCreateInfo image_create_info = {
		    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		    .imageType     = VK_IMAGE_TYPE_2D,
		    .format        = VK_FORMAT_R32G32B32A32_SFLOAT,
		    .extent        = VkExtent3D{9, 1, 1},
		    .mipLevels     = 1,
		    .arrayLayers   = 1,
		    .samples       = VK_SAMPLE_COUNT_1_BIT,
		    .tiling        = VK_IMAGE_TILING_OPTIMAL,
		    .usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
		    .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
		    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};
		VmaAllocationCreateInfo allocation_create_info = {
		    .usage = VMA_MEMORY_USAGE_GPU_ONLY,
		};
		vmaCreateImage(m_context->vma_allocator, &image_create_info, &allocation_create_info, &envmap.irradiance_sh.vk_image, &envmap.irradiance_sh.vma_allocation, nullptr);
		VkImageViewCreateInfo view_create_info = {
		    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image            = envmap.irradiance_sh.vk_image,
		    .viewType         = VK_IMAGE_VIEW_TYPE_2D,
		    .format           = VK_FORMAT_R32G32B32A32_SFLOAT,
		    .components       = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
		    .subresourceRange = {
		        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		        .baseMipLevel   = 0,
		        .levelCount     = 1,
		        .baseArrayLayer = 0,
		        .layerCount     = 1,
		    },
		};
		vkCreateImageView(m_context->vk_device, &view_create_info, nullptr, &envmap.irradiance_sh_view);
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) envmap.irradiance_sh.vk_image, "Irradiance SH");
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) envmap.irradiance_sh_view, "Irradiance SH View");
	}

	// Create prefilter map
	VkImageView prefilter_map_view = VK_NULL_HANDLE;
	{
		VkImageCreateInfo image_create_info = {
		    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		    .flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
		    .imageType     = VK_IMAGE_TYPE_2D,
		    .format        = VK_FORMAT_R32G32B32A32_SFLOAT,
		    .extent        = VkExtent3D{PREFILTER_MAP_SIZE, PREFILTER_MAP_SIZE, 1},
		    .mipLevels     = PREFILTER_MIP_LEVELS,
		    .arrayLayers   = CUBEMAP_FACE_NUM,
		    .samples       = VK_SAMPLE_COUNT_1_BIT,
		    .tiling        = VK_IMAGE_TILING_OPTIMAL,
		    .usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
		    .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
		    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};
		VmaAllocationCreateInfo allocation_create_info = {
		    .usage = VMA_MEMORY_USAGE_GPU_ONLY,
		};
		vmaCreateImage(m_context->vma_allocator, &image_create_info, &allocation_create_info, &envmap.prefilter_map.vk_image, &envmap.prefilter_map.vma_allocation, nullptr);
		VkImageViewCreateInfo view_create_info = {
		    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image            = envmap.prefilter_map.vk_image,
		    .viewType         = VK_IMAGE_VIEW_TYPE_CUBE,
		    .format           = VK_FORMAT_R32G32B32A32_SFLOAT,
		    .components       = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
		    .subresourceRange = {
		        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		        .baseMipLevel   = 0,
		        .levelCount     = PREFILTER_MIP_LEVELS,
		        .baseArrayLayer = 0,
		        .layerCount     = CUBEMAP_FACE_NUM,
		    },
		};
		vkCreateImageView(m_context->vk_device, &view_create_info, nullptr, &envmap.prefilter_map_view);
		view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
		vkCreateImageView(m_context->vk_device, &view_create_info, nullptr, &prefilter_map_view);
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) envmap.prefilter_map.vk_image, "Prefilter Map");
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) envmap.prefilter_map_view, "Prefilter Map View");
	}

	// Create equirectangular to cubemap pass
	struct
	{
		VkShaderModule        vert_shader           = VK_NULL_HANDLE;
		VkShaderModule        frag_shader           = VK_NULL_HANDLE;
		VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
		VkDescriptorSet       descriptor_set        = VK_NULL_HANDLE;
		VkPipelineLayout      pipeline_layout       = VK_NULL_HANDLE;
		VkPipeline            pipeline              = VK_NULL_HANDLE;
	} equirectangular_to_cubemap;

	{
		VkShaderModuleCreateInfo create_info = {
		    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		    .codeSize = sizeof(g_equirectangular_to_cubemap_vert_spv_data),
		    .pCode    = reinterpret_cast<uint32_t *>(g_equirectangular_to_cubemap_vert_spv_data),
		};
		vkCreateShaderModule(m_context->vk_device, &create_info, nullptr, &equirectangular_to_cubemap.vert_shader);
	}

	{
		VkShaderModuleCreateInfo create_info = {
		    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		    .codeSize = sizeof(g_equirectangular_to_cubemap_frag_spv_data),
		    .pCode    = reinterpret_cast<uint32_t *>(g_equirectangular_to_cubemap_frag_spv_data),
		};
		vkCreateShaderModule(m_context->vk_device, &create_info, nullptr, &equirectangular_to_cubemap.frag_shader);
	}

	{
		VkDescriptorSetLayoutBinding binding = {
		    .binding         = 0,
		    .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		    .descriptorCount = 1,
		    .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
		};
		VkDescriptorSetLayoutCreateInfo create_info = {
		    .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		    .bindingCount = 1,
		    .pBindings    = &binding,
		};
		vkCreateDescriptorSetLayout(m_context->vk_device, &create_info, nullptr, &equirectangular_to_cubemap.descriptor_set_layout);
	}

	{
		VkDescriptorSetAllocateInfo allocate_info = {
		    .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		    .pNext              = nullptr,
		    .descriptorPool     = m_context->vk_descriptor_pool,
		    .descriptorSetCount = 1,
		    .pSetLayouts        = &equirectangular_to_cubemap.descriptor_set_layout,
		};
		vkAllocateDescriptorSets(m_context->vk_device, &allocate_info, &equirectangular_to_cubemap.descriptor_set);
	}

	{
		VkDescriptorImageInfo image_info{
		    .sampler     = linear_sampler,
		    .imageView   = texture_view,
		    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		};
		VkWriteDescriptorSet write = {
		    .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		    .dstSet           = equirectangular_to_cubemap.descriptor_set,
		    .dstBinding       = 0,
		    .dstArrayElement  = 0,
		    .descriptorCount  = 1,
		    .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		    .pImageInfo       = &image_info,
		    .pBufferInfo      = nullptr,
		    .pTexelBufferView = nullptr,
		};
		vkUpdateDescriptorSets(m_context->vk_device, 1, &write, 0, nullptr);
	}

	{
		VkPipelineLayoutCreateInfo create_info = {
		    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		    .setLayoutCount         = 1,
		    .pSetLayouts            = &equirectangular_to_cubemap.descriptor_set_layout,
		    .pushConstantRangeCount = 0,
		    .pPushConstantRanges    = nullptr,
		};
		vkCreatePipelineLayout(m_context->vk_device, &create_info, nullptr, &equirectangular_to_cubemap.pipeline_layout);
	}

	{
		VkFormat color_attachment_format = VK_FORMAT_R32G32B32A32_SFLOAT;

		VkPipelineRenderingCreateInfo pipeline_rendering_create_info = {
		    .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
		    .colorAttachmentCount    = 1,
		    .pColorAttachmentFormats = &color_attachment_format,
		};

		VkPipelineInputAssemblyStateCreateInfo input_assembly_state_create_info = {
		    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		    .flags                  = 0,
		    .topology               = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		    .primitiveRestartEnable = VK_FALSE,
		};

		VkPipelineRasterizationStateCreateInfo rasterization_state_create_info = {
		    .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		    .depthClampEnable        = VK_FALSE,
		    .rasterizerDiscardEnable = VK_FALSE,
		    .polygonMode             = VK_POLYGON_MODE_FILL,
		    .cullMode                = VK_CULL_MODE_NONE,
		    .frontFace               = VK_FRONT_FACE_CLOCKWISE,
		    .depthBiasEnable         = VK_FALSE,
		    .depthBiasConstantFactor = 0.f,
		    .depthBiasClamp          = 0.f,
		    .depthBiasSlopeFactor    = 0.f,
		    .lineWidth               = 1.f,
		};

		VkPipelineColorBlendAttachmentState color_blend_attachment_states = {
		    .blendEnable    = VK_FALSE,
		    .colorWriteMask = 0xf,
		};

		VkPipelineColorBlendStateCreateInfo color_blend_state_create_info = {
		    .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		    .logicOpEnable   = VK_FALSE,
		    .attachmentCount = 1,
		    .pAttachments    = &color_blend_attachment_states,
		};

		VkPipelineDepthStencilStateCreateInfo depth_stencil_state_create_info = {
		    .sType             = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		    .depthTestEnable   = VK_FALSE,
		    .depthWriteEnable  = VK_FALSE,
		    .stencilTestEnable = VK_FALSE,
		};

		VkViewport viewport = {
		    .x        = 0,
		    .y        = 0,
		    .width    = 1024.f,
		    .height   = 1024.f,
		    .minDepth = 0.f,
		    .maxDepth = 1.f,
		};

		VkRect2D rect = {
		    .offset = VkOffset2D{
		        .x = 0,
		        .y = 0,
		    },
		    .extent = VkExtent2D{
		        .width  = 1024u,
		        .height = 1024u,
		    },
		};

		VkPipelineViewportStateCreateInfo viewport_state_create_info = {
		    .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		    .viewportCount = 1,
		    .pViewports    = &viewport,
		    .scissorCount  = 1,
		    .pScissors     = &rect,
		};

		VkPipelineMultisampleStateCreateInfo multisample_state_create_info = {
		    .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
		    .sampleShadingEnable  = VK_FALSE,
		};

		VkPipelineVertexInputStateCreateInfo vertex_input_state_create_info = {
		    .sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		    .vertexBindingDescriptionCount   = 0,
		    .vertexAttributeDescriptionCount = 0,
		};

		VkPipelineShaderStageCreateInfo pipeline_shader_stage_create_infos[] = {
		    {
		        .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		        .stage               = VK_SHADER_STAGE_VERTEX_BIT,
		        .module              = equirectangular_to_cubemap.vert_shader,
		        .pName               = "main",
		        .pSpecializationInfo = nullptr,
		    },
		    {
		        .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		        .stage               = VK_SHADER_STAGE_FRAGMENT_BIT,
		        .module              = equirectangular_to_cubemap.frag_shader,
		        .pName               = "main",
		        .pSpecializationInfo = nullptr,
		    },
		};

		VkGraphicsPipelineCreateInfo create_info = {
		    .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		    .pNext               = &pipeline_rendering_create_info,
		    .stageCount          = 2,
		    .pStages             = pipeline_shader_stage_create_infos,
		    .pVertexInputState   = &vertex_input_state_create_info,
		    .pInputAssemblyState = &input_assembly_state_create_info,
		    .pTessellationState  = nullptr,
		    .pViewportState      = &viewport_state_create_info,
		    .pRasterizationState = &rasterization_state_create_info,
		    .pMultisampleState   = &multisample_state_create_info,
		    .pDepthStencilState  = &depth_stencil_state_create_info,
		    .pColorBlendState    = &color_blend_state_create_info,
		    .pDynamicState       = nullptr,
		    .layout              = equirectangular_to_cubemap.pipeline_layout,
		    .renderPass          = VK_NULL_HANDLE,
		    .subpass             = 0,
		    .basePipelineHandle  = VK_NULL_HANDLE,
		    .basePipelineIndex   = -1,
		};
		vkCreateGraphicsPipelines(m_context->vk_device, m_context->vk_pipeline_cache, 1, &create_info, nullptr, &equirectangular_to_cubemap.pipeline);
		vkDestroyShaderModule(m_context->vk_device, equirectangular_to_cubemap.vert_shader, nullptr);
		vkDestroyShaderModule(m_context->vk_device, equirectangular_to_cubemap.frag_shader, nullptr);
	}

	// Create cubemap sh projection pass
	struct
	{
		VkShaderModule        shader                = VK_NULL_HANDLE;
		VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
		VkDescriptorSet       descriptor_set        = VK_NULL_HANDLE;
		VkPipelineLayout      pipeline_layout       = VK_NULL_HANDLE;
		VkPipeline            pipeline              = VK_NULL_HANDLE;
	} cubemap_sh_projection;

	{
		VkShaderModule shader = VK_NULL_HANDLE;
		{
			VkShaderModuleCreateInfo create_info = {
			    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			    .codeSize = sizeof(g_cubemap_sh_projection_comp_spv_data),
			    .pCode    = reinterpret_cast<uint32_t *>(g_cubemap_sh_projection_comp_spv_data),
			};
			vkCreateShaderModule(m_context->vk_device, &create_info, nullptr, &shader);
		}

		VkDescriptorSetLayoutBinding bindings[] = {
		    // SH Intermediate
		    {
		        .binding         = 0,
		        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		        .descriptorCount = 1,
		        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
		    },
		    // Skybox
		    {
		        .binding         = 1,
		        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .descriptorCount = 1,
		        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
		    },
		};
		VkDescriptorSetLayoutCreateInfo create_info = {
		    .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		    .bindingCount = 2,
		    .pBindings    = bindings,
		};
		vkCreateDescriptorSetLayout(m_context->vk_device, &create_info, nullptr, &cubemap_sh_projection.descriptor_set_layout);

		{
			VkDescriptorSetAllocateInfo allocate_info = {
			    .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			    .pNext              = nullptr,
			    .descriptorPool     = m_context->vk_descriptor_pool,
			    .descriptorSetCount = 1,
			    .pSetLayouts        = &cubemap_sh_projection.descriptor_set_layout,
			};
			vkAllocateDescriptorSets(m_context->vk_device, &allocate_info, &cubemap_sh_projection.descriptor_set);
		}

		{
			VkDescriptorImageInfo sh_intermediate_info{
			    .sampler     = VK_NULL_HANDLE,
			    .imageView   = sh_intermediate_view,
			    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
			};
			VkDescriptorImageInfo skybox_info{
			    .sampler     = linear_sampler,
			    .imageView   = envmap.texture_view,
			    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			};
			VkWriteDescriptorSet writes[] = {
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = cubemap_sh_projection.descriptor_set,
			        .dstBinding       = 0,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			        .pImageInfo       = &sh_intermediate_info,
			        .pBufferInfo      = nullptr,
			        .pTexelBufferView = nullptr,
			    },
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = cubemap_sh_projection.descriptor_set,
			        .dstBinding       = 1,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .pImageInfo       = &skybox_info,
			        .pBufferInfo      = nullptr,
			        .pTexelBufferView = nullptr,
			    }};
			vkUpdateDescriptorSets(m_context->vk_device, 2, writes, 0, nullptr);
		}

		{
			VkPipelineLayoutCreateInfo create_info = {
			    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			    .setLayoutCount         = 1,
			    .pSetLayouts            = &cubemap_sh_projection.descriptor_set_layout,
			    .pushConstantRangeCount = 0,
			    .pPushConstantRanges    = nullptr,
			};
			vkCreatePipelineLayout(m_context->vk_device, &create_info, nullptr, &cubemap_sh_projection.pipeline_layout);
		}

		{
			VkComputePipelineCreateInfo create_info = {
			    .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			    .stage = {
			        .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			        .stage               = VK_SHADER_STAGE_COMPUTE_BIT,
			        .module              = shader,
			        .pName               = "main",
			        .pSpecializationInfo = nullptr,
			    },
			    .layout             = cubemap_sh_projection.pipeline_layout,
			    .basePipelineHandle = VK_NULL_HANDLE,
			    .basePipelineIndex  = -1,
			};
			vkCreateComputePipelines(m_context->vk_device, m_context->vk_pipeline_cache, 1, &create_info, nullptr, &cubemap_sh_projection.pipeline);
			vkDestroyShaderModule(m_context->vk_device, shader, nullptr);
		}
	}

	// Create sh add pass
	struct
	{
		VkShaderModule        shader                = VK_NULL_HANDLE;
		VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
		VkDescriptorSet       descriptor_set        = VK_NULL_HANDLE;
		VkPipelineLayout      pipeline_layout       = VK_NULL_HANDLE;
		VkPipeline            pipeline              = VK_NULL_HANDLE;
	} cubemap_sh_add;

	{
		VkShaderModule shader = VK_NULL_HANDLE;
		{
			VkShaderModuleCreateInfo create_info = {
			    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			    .codeSize = sizeof(g_cubemap_sh_add_comp_spv_data),
			    .pCode    = reinterpret_cast<uint32_t *>(g_cubemap_sh_add_comp_spv_data),
			};
			vkCreateShaderModule(m_context->vk_device, &create_info, nullptr, &shader);
		}

		VkDescriptorSetLayoutBinding bindings[] = {
		    // Irradiance SH
		    {
		        .binding         = 0,
		        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		        .descriptorCount = 1,
		        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
		    },
		    // SH intermediate
		    {
		        .binding         = 1,
		        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .descriptorCount = 1,
		        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
		    },
		};
		VkDescriptorSetLayoutCreateInfo create_info = {
		    .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		    .bindingCount = 2,
		    .pBindings    = bindings,
		};
		vkCreateDescriptorSetLayout(m_context->vk_device, &create_info, nullptr, &cubemap_sh_add.descriptor_set_layout);

		{
			VkDescriptorSetAllocateInfo allocate_info = {
			    .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			    .pNext              = nullptr,
			    .descriptorPool     = m_context->vk_descriptor_pool,
			    .descriptorSetCount = 1,
			    .pSetLayouts        = &cubemap_sh_add.descriptor_set_layout,
			};
			vkAllocateDescriptorSets(m_context->vk_device, &allocate_info, &cubemap_sh_add.descriptor_set);
		}

		{
			VkDescriptorImageInfo sh_intermediate_info{
			    .sampler     = nearest_sampler,
			    .imageView   = sh_intermediate_view,
			    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			};
			VkDescriptorImageInfo irradiance_sh_info{
			    .sampler     = linear_sampler,
			    .imageView   = envmap.irradiance_sh_view,
			    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
			};
			VkWriteDescriptorSet writes[] = {
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = cubemap_sh_add.descriptor_set,
			        .dstBinding       = 0,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			        .pImageInfo       = &irradiance_sh_info,
			        .pBufferInfo      = nullptr,
			        .pTexelBufferView = nullptr,
			    },
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = cubemap_sh_add.descriptor_set,
			        .dstBinding       = 1,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .pImageInfo       = &sh_intermediate_info,
			        .pBufferInfo      = nullptr,
			        .pTexelBufferView = nullptr,
			    }};
			vkUpdateDescriptorSets(m_context->vk_device, 2, writes, 0, nullptr);
		}

		{
			VkPipelineLayoutCreateInfo create_info = {
			    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			    .setLayoutCount         = 1,
			    .pSetLayouts            = &cubemap_sh_add.descriptor_set_layout,
			    .pushConstantRangeCount = 0,
			    .pPushConstantRanges    = nullptr,
			};
			vkCreatePipelineLayout(m_context->vk_device, &create_info, nullptr, &cubemap_sh_add.pipeline_layout);
		}

		{
			VkComputePipelineCreateInfo create_info = {
			    .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			    .stage = {
			        .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			        .stage               = VK_SHADER_STAGE_COMPUTE_BIT,
			        .module              = shader,
			        .pName               = "main",
			        .pSpecializationInfo = nullptr,
			    },
			    .layout             = cubemap_sh_add.pipeline_layout,
			    .basePipelineHandle = VK_NULL_HANDLE,
			    .basePipelineIndex  = -1,
			};
			vkCreateComputePipelines(m_context->vk_device, m_context->vk_pipeline_cache, 1, &create_info, nullptr, &cubemap_sh_add.pipeline);
			vkDestroyShaderModule(m_context->vk_device, shader, nullptr);
		}
	}

	// Create prefilter map pass
	struct
	{
		VkShaderModule        shader                = VK_NULL_HANDLE;
		VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
		VkDescriptorSet       descriptor_sets[PREFILTER_MIP_LEVELS];
		VkPipelineLayout      pipeline_layout = VK_NULL_HANDLE;
		VkPipeline            pipeline        = VK_NULL_HANDLE;
	} prefilter_map;

	std::vector<VkImageView> prefiltered_views(PREFILTER_MIP_LEVELS);
	std::fill(prefiltered_views.begin(), prefiltered_views.end(), VK_NULL_HANDLE);
	for (uint32_t i = 0; i < PREFILTER_MIP_LEVELS; i++)
	{
		VkImageViewCreateInfo view_create_info = {
		    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image            = envmap.prefilter_map.vk_image,
		    .viewType         = VK_IMAGE_VIEW_TYPE_CUBE,
		    .format           = VK_FORMAT_R32G32B32A32_SFLOAT,
		    .components       = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
		    .subresourceRange = {
		        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		        .baseMipLevel   = i,
		        .levelCount     = 1,
		        .baseArrayLayer = 0,
		        .layerCount     = 6,
		    },
		};
		vkCreateImageView(m_context->vk_device, &view_create_info, nullptr, &prefiltered_views[i]);
	}

	{
		VkShaderModule shader = VK_NULL_HANDLE;
		{
			VkShaderModuleCreateInfo create_info = {
			    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			    .codeSize = sizeof(g_cubemap_prefilter_comp_spv_data),
			    .pCode    = reinterpret_cast<uint32_t *>(g_cubemap_prefilter_comp_spv_data),
			};
			vkCreateShaderModule(m_context->vk_device, &create_info, nullptr, &shader);
		}

		VkDescriptorSetLayoutBinding bindings[] = {
		    // Skybox
		    {
		        .binding         = 0,
		        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .descriptorCount = 1,
		        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
		    },
		    // Prefiltered Image
		    {
		        .binding         = 1,
		        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		        .descriptorCount = 1,
		        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
		    },
		};
		VkDescriptorSetLayoutCreateInfo create_info = {
		    .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		    .bindingCount = 2,
		    .pBindings    = bindings,
		};
		vkCreateDescriptorSetLayout(m_context->vk_device, &create_info, nullptr, &prefilter_map.descriptor_set_layout);

		{
			std::fill(prefilter_map.descriptor_sets, prefilter_map.descriptor_sets + PREFILTER_MIP_LEVELS, VK_NULL_HANDLE);
			std::vector<VkDescriptorSetLayout> layouts(PREFILTER_MIP_LEVELS);
			std::fill(layouts.begin(), layouts.end(), prefilter_map.descriptor_set_layout);
			VkDescriptorSetAllocateInfo allocate_info = {
			    .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			    .pNext              = nullptr,
			    .descriptorPool     = m_context->vk_descriptor_pool,
			    .descriptorSetCount = PREFILTER_MIP_LEVELS,
			    .pSetLayouts        = layouts.data(),
			};
			vkAllocateDescriptorSets(m_context->vk_device, &allocate_info, prefilter_map.descriptor_sets);
		}

		for (uint32_t i = 0; i < PREFILTER_MIP_LEVELS; i++)
		{
			VkDescriptorImageInfo skybox_info{
			    .sampler     = linear_sampler,
			    .imageView   = envmap.texture_view,
			    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			};
			VkDescriptorImageInfo prefiltered_image_info{
			    .sampler     = VK_NULL_HANDLE,
			    .imageView   = prefiltered_views[i],
			    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
			};
			VkWriteDescriptorSet writes[] = {
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = prefilter_map.descriptor_sets[i],
			        .dstBinding       = 0,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .pImageInfo       = &skybox_info,
			        .pBufferInfo      = nullptr,
			        .pTexelBufferView = nullptr,
			    },
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = prefilter_map.descriptor_sets[i],
			        .dstBinding       = 1,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			        .pImageInfo       = &prefiltered_image_info,
			        .pBufferInfo      = nullptr,
			        .pTexelBufferView = nullptr,
			    }};
			vkUpdateDescriptorSets(m_context->vk_device, 2, writes, 0, nullptr);
		}

		{
			VkPushConstantRange range = {
			    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			    .offset     = 0,
			    .size       = sizeof(int32_t),
			};
			VkPipelineLayoutCreateInfo create_info = {
			    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			    .setLayoutCount         = 1,
			    .pSetLayouts            = &prefilter_map.descriptor_set_layout,
			    .pushConstantRangeCount = 1,
			    .pPushConstantRanges    = &range,
			};
			vkCreatePipelineLayout(m_context->vk_device, &create_info, nullptr, &prefilter_map.pipeline_layout);
		}

		{
			VkComputePipelineCreateInfo create_info = {
			    .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			    .stage = {
			        .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			        .stage               = VK_SHADER_STAGE_COMPUTE_BIT,
			        .module              = shader,
			        .pName               = "main",
			        .pSpecializationInfo = nullptr,
			    },
			    .layout             = prefilter_map.pipeline_layout,
			    .basePipelineHandle = VK_NULL_HANDLE,
			    .basePipelineIndex  = -1,
			};
			vkCreateComputePipelines(m_context->vk_device, m_context->vk_pipeline_cache, 1, &create_info, nullptr, &prefilter_map.pipeline);
			vkDestroyShaderModule(m_context->vk_device, shader, nullptr);
		}
	}

	// Allocate command buffer
	VkCommandBuffer cmd_buffer = VK_NULL_HANDLE;
	{
		VkCommandBufferAllocateInfo allocate_info =
		    {
		        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		        .commandPool        = m_context->graphics_cmd_pool,
		        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		        .commandBufferCount = 1,
		    };
		vkAllocateCommandBuffers(m_context->vk_device, &allocate_info, &cmd_buffer);
	}

	// Create fence
	VkFence fence = VK_NULL_HANDLE;
	{
		VkFenceCreateInfo create_info = {
		    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		    .flags = 0,
		};
		vkCreateFence(m_context->vk_device, &create_info, nullptr, &fence);
	}

	VkCommandBufferBeginInfo begin_info = {
	    .sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	    .flags            = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	    .pInheritanceInfo = nullptr,
	};

	vkBeginCommandBuffer(cmd_buffer, &begin_info);

	{
		VkImageMemoryBarrier image_barrier = {
		    .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask       = 0,
		    .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
		    .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
		    .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		    .image               = texture.vk_image,
		    .subresourceRange    = VkImageSubresourceRange{
		           .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		           .baseMipLevel   = 0,
		           .levelCount     = 1,
		           .baseArrayLayer = 0,
		           .layerCount     = 1,
            },
		};
		vkCmdPipelineBarrier(
		    cmd_buffer,
		    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		    VK_PIPELINE_STAGE_TRANSFER_BIT,
		    0, 0, nullptr, 0, nullptr, 1, &image_barrier);
	}

	// Copy buffer to texture
	{
		VkBufferImageCopy copy_info = {
		    .bufferOffset      = 0,
		    .bufferRowLength   = 0,
		    .bufferImageHeight = 0,
		    .imageSubresource  = VkImageSubresourceLayers{
		         .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		         .mipLevel       = 0,
		         .baseArrayLayer = 0,
		         .layerCount     = 1,
            },
		    .imageOffset = {0, 0, 0},
		    .imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1},
		};
		vkCmdCopyBufferToImage(cmd_buffer, staging_buffer.vk_buffer, texture.vk_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_info);
	}

	{
		VkImageMemoryBarrier image_barriers[] = {
		    {
		        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
		        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
		        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image               = texture.vk_image,
		        .subresourceRange    = VkImageSubresourceRange{
		               .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		               .baseMipLevel   = 0,
		               .levelCount     = 1,
		               .baseArrayLayer = 0,
		               .layerCount     = 1,
                },
		    },
		    {
		        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask       = 0,
		        .dstAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
		        .newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image               = envmap.texture.vk_image,
		        .subresourceRange    = VkImageSubresourceRange{
		               .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		               .baseMipLevel   = 0,
		               .levelCount     = 1,
		               .baseArrayLayer = 0,
		               .layerCount     = 6,
                },
		    }};
		vkCmdPipelineBarrier(
		    cmd_buffer,
		    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
		    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		    0, 0, nullptr, 0, nullptr, 2, image_barriers);
	}

	// Equirectangular to cubemap
	{
		VkRenderingAttachmentInfo attachment_info = {
		    .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		    .imageView   = cubemap_view_2d,
		    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		    .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
		    .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
		    .clearValue  = {
		         .color = {
		             .uint32 = {0, 0, 0, 0},
                },
            },
		};
		VkRenderingInfo rendering_info = {
		    .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
		    .renderArea           = {0, 0, 1024, 1024},
		    .layerCount           = 6,
		    .colorAttachmentCount = 1,
		    .pColorAttachments    = &attachment_info,
		    .pDepthAttachment     = nullptr,
		};
		vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, equirectangular_to_cubemap.pipeline_layout, 0, 1, &equirectangular_to_cubemap.descriptor_set, 0, nullptr);
		vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, equirectangular_to_cubemap.pipeline);
		vkCmdBeginRendering(cmd_buffer, &rendering_info);
		vkCmdDraw(cmd_buffer, 3, 6, 0, 0);
		vkCmdEndRendering(cmd_buffer);
	}

	{
		VkImageMemoryBarrier image_barriers[] = {
		    {
		        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		        .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
		        .oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image               = envmap.texture.vk_image,
		        .subresourceRange    = VkImageSubresourceRange{
		               .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		               .baseMipLevel   = 0,
		               .levelCount     = 1,
		               .baseArrayLayer = 0,
		               .layerCount     = 6,
                },
		    },
		    {
		        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask       = 0,
		        .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
		        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
		        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image               = envmap.texture.vk_image,
		        .subresourceRange    = VkImageSubresourceRange{
		               .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		               .baseMipLevel   = 1,
		               .levelCount     = 4,
		               .baseArrayLayer = 0,
		               .layerCount     = 6,
                },
		    }};
		vkCmdPipelineBarrier(
		    cmd_buffer,
		    VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		    VK_PIPELINE_STAGE_TRANSFER_BIT,
		    0, 0, nullptr, 0, nullptr, 2, image_barriers);
	}

	// Generate mipmaps
	for (uint32_t i = 1; i < 5; i++)
	{
		VkImageBlit blit_info = {
		    .srcSubresource = VkImageSubresourceLayers{
		        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		        .mipLevel       = i - 1,
		        .baseArrayLayer = 0,
		        .layerCount     = 6,
		    },
		    .srcOffsets = {
		        VkOffset3D{
		            .x = 0,
		            .y = 0,
		            .z = 0,
		        },
		        VkOffset3D{
		            .x = 1024 >> (i - 1),
		            .y = 1024 >> (i - 1),
		            .z = 1,
		        },
		    },
		    .dstSubresource = VkImageSubresourceLayers{
		        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		        .mipLevel       = i,
		        .baseArrayLayer = 0,
		        .layerCount     = 6,
		    },
		    .dstOffsets = {
		        VkOffset3D{
		            .x = 0,
		            .y = 0,
		            .z = 0,
		        },
		        VkOffset3D{
		            .x = 1024 >> i,
		            .y = 1024 >> i,
		            .z = 1,
		        },
		    },
		};

		{
			VkImageMemoryBarrier image_barrier = VkImageMemoryBarrier{
			    .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			    .srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
			    .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
			    .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			    .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			    .image               = envmap.texture.vk_image,
			    .subresourceRange    = VkImageSubresourceRange{
			           .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
			           .baseMipLevel   = i,
			           .levelCount     = 1,
			           .baseArrayLayer = 0,
			           .layerCount     = 6,
                },
			};
			vkCmdPipelineBarrier(
			    cmd_buffer,
			    VK_PIPELINE_STAGE_TRANSFER_BIT,
			    VK_PIPELINE_STAGE_TRANSFER_BIT,
			    0, 0, nullptr, 0, nullptr, 1, &image_barrier);
		}

		vkCmdBlitImage(
		    cmd_buffer,
		    envmap.texture.vk_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		    envmap.texture.vk_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    1, &blit_info, VK_FILTER_LINEAR);

		{
			VkImageMemoryBarrier image_barrier = VkImageMemoryBarrier{
			    .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			    .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
			    .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
			    .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			    .image               = envmap.texture.vk_image,
			    .subresourceRange    = VkImageSubresourceRange{
			           .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
			           .baseMipLevel   = i,
			           .levelCount     = 1,
			           .baseArrayLayer = 0,
			           .layerCount     = 6,
                },
			};
			vkCmdPipelineBarrier(
			    cmd_buffer,
			    VK_PIPELINE_STAGE_TRANSFER_BIT,
			    VK_PIPELINE_STAGE_TRANSFER_BIT,
			    0, 0, nullptr, 0, nullptr, 1, &image_barrier);
		}
	}

	{
		VkImageMemoryBarrier image_barriers[] =
		    {
		        {
		            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		            .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
		            .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
		            .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		            .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		            .image               = envmap.texture.vk_image,
		            .subresourceRange    = VkImageSubresourceRange{
		                   .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		                   .baseMipLevel   = 0,
		                   .levelCount     = 5,
		                   .baseArrayLayer = 0,
		                   .layerCount     = 6,
                    },
		        },
		        {
		            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		            .srcAccessMask       = 0,
		            .dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
		            .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
		            .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
		            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		            .image               = sh_intermediate.vk_image,
		            .subresourceRange    = VkImageSubresourceRange{
		                   .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		                   .baseMipLevel   = 0,
		                   .levelCount     = 1,
		                   .baseArrayLayer = 0,
		                   .layerCount     = 6,
                    },
		        },
		        {
		            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		            .srcAccessMask       = 0,
		            .dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
		            .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
		            .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
		            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		            .image               = envmap.prefilter_map.vk_image,
		            .subresourceRange    = VkImageSubresourceRange{
		                   .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		                   .baseMipLevel   = 0,
		                   .levelCount     = PREFILTER_MIP_LEVELS,
		                   .baseArrayLayer = 0,
		                   .layerCount     = 6,
                    },
		        },
		    };
		vkCmdPipelineBarrier(
		    cmd_buffer,
		    VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		    0, 0, nullptr, 0, nullptr, 3, image_barriers);
	}

	// Cubemap sh projection
	{
		vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, cubemap_sh_projection.pipeline_layout, 0, 1, &cubemap_sh_projection.descriptor_set, 0, nullptr);
		vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, cubemap_sh_projection.pipeline);
		vkCmdDispatch(cmd_buffer, IRRADIANCE_CUBEMAP_SIZE / IRRADIANCE_WORK_GROUP_SIZE, IRRADIANCE_CUBEMAP_SIZE / IRRADIANCE_WORK_GROUP_SIZE, 6);
	}

	{
		VkImageMemoryBarrier image_barriers[] =
		    {
		        {
		            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		            .srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
		            .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
		            .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
		            .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		            .image               = sh_intermediate.vk_image,
		            .subresourceRange    = VkImageSubresourceRange{
		                   .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		                   .baseMipLevel   = 0,
		                   .levelCount     = 1,
		                   .baseArrayLayer = 0,
		                   .layerCount     = 6,
                    },
		        },
		        {
		            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		            .srcAccessMask       = 0,
		            .dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
		            .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
		            .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
		            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		            .image               = envmap.irradiance_sh.vk_image,
		            .subresourceRange    = VkImageSubresourceRange{
		                   .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		                   .baseMipLevel   = 0,
		                   .levelCount     = 1,
		                   .baseArrayLayer = 0,
		                   .layerCount     = 1,
                    },
		        },
		    };
		vkCmdPipelineBarrier(
		    cmd_buffer,
		    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		    0, 0, nullptr, 0, nullptr, 2, image_barriers);
	}

	// Cubemap sh add
	{
		vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, cubemap_sh_add.pipeline_layout, 0, 1, &cubemap_sh_add.descriptor_set, 0, nullptr);
		vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, cubemap_sh_add.pipeline);
		vkCmdDispatch(cmd_buffer, 9, 1, 1);
	}

	// Cubemap prefilter
	{
		vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, prefilter_map.pipeline);
		for (int32_t i = 0; i < PREFILTER_MIP_LEVELS; i++)
		{
			uint32_t mip_size = PREFILTER_MAP_SIZE * std::pow(0.5, i);
			vkCmdPushConstants(cmd_buffer, prefilter_map.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(int32_t), &i);
			vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, prefilter_map.pipeline_layout, 0, 1, &prefilter_map.descriptor_sets[i], 0, nullptr);
			vkCmdDispatch(cmd_buffer, mip_size / 8, mip_size / 8, 6);
		}
	}

	{
		VkImageMemoryBarrier image_barriers[] =
		    {
		        {
		            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		            .srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
		            .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
		            .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
		            .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		            .image               = envmap.irradiance_sh.vk_image,
		            .subresourceRange    = VkImageSubresourceRange{
		                   .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		                   .baseMipLevel   = 0,
		                   .levelCount     = 1,
		                   .baseArrayLayer = 0,
		                   .layerCount     = 1,
                    },
		        },
		        {
		            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		            .srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
		            .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
		            .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
		            .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		            .image               = envmap.prefilter_map.vk_image,
		            .subresourceRange    = VkImageSubresourceRange{
		                   .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		                   .baseMipLevel   = 0,
		                   .levelCount     = PREFILTER_MIP_LEVELS,
		                   .baseArrayLayer = 0,
		                   .layerCount     = 6,
                    },
		        },
		    };
		vkCmdPipelineBarrier(
		    cmd_buffer,
		    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		    0, 0, nullptr, 0, nullptr, 2, image_barriers);
	}

	vkEndCommandBuffer(cmd_buffer);

	// Submit command buffer
	{
		VkSubmitInfo submit_info = {
		    .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		    .waitSemaphoreCount   = 0,
		    .pWaitSemaphores      = nullptr,
		    .pWaitDstStageMask    = 0,
		    .commandBufferCount   = 1,
		    .pCommandBuffers      = &cmd_buffer,
		    .signalSemaphoreCount = 0,
		    .pSignalSemaphores    = nullptr,
		};
		vkQueueSubmit(m_context->graphics_queue, 1, &submit_info, fence);
	}

	// Wait
	vkWaitForFences(m_context->vk_device, 1, &fence, VK_TRUE, UINT64_MAX);
	vkResetFences(m_context->vk_device, 1, &fence);

	// Release resource
	vkDestroyFence(m_context->vk_device, fence, nullptr);
	vkFreeCommandBuffers(m_context->vk_device, m_context->graphics_cmd_pool, 1, &cmd_buffer);
	vmaDestroyBuffer(m_context->vma_allocator, staging_buffer.vk_buffer, staging_buffer.vma_allocation);
	vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 1, &equirectangular_to_cubemap.descriptor_set);
	vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 1, &cubemap_sh_projection.descriptor_set);
	vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 1, &cubemap_sh_add.descriptor_set);
	vkDestroyDescriptorSetLayout(m_context->vk_device, equirectangular_to_cubemap.descriptor_set_layout, nullptr);
	vkDestroyDescriptorSetLayout(m_context->vk_device, cubemap_sh_projection.descriptor_set_layout, nullptr);
	vkDestroyDescriptorSetLayout(m_context->vk_device, cubemap_sh_add.descriptor_set_layout, nullptr);
	vkDestroyDescriptorSetLayout(m_context->vk_device, prefilter_map.descriptor_set_layout, nullptr);
	vkDestroyPipelineLayout(m_context->vk_device, equirectangular_to_cubemap.pipeline_layout, nullptr);
	vkDestroyPipelineLayout(m_context->vk_device, cubemap_sh_projection.pipeline_layout, nullptr);
	vkDestroyPipelineLayout(m_context->vk_device, cubemap_sh_add.pipeline_layout, nullptr);
	vkDestroyPipelineLayout(m_context->vk_device, prefilter_map.pipeline_layout, nullptr);
	vkDestroyPipeline(m_context->vk_device, equirectangular_to_cubemap.pipeline, nullptr);
	vkDestroyPipeline(m_context->vk_device, cubemap_sh_projection.pipeline, nullptr);
	vkDestroyPipeline(m_context->vk_device, cubemap_sh_add.pipeline, nullptr);
	vkDestroyPipeline(m_context->vk_device, prefilter_map.pipeline, nullptr);
	vkDestroyImageView(m_context->vk_device, cubemap_view_2d, nullptr);
	vkDestroyImageView(m_context->vk_device, texture_view, nullptr);
	vkDestroyImageView(m_context->vk_device, sh_intermediate_view, nullptr);
	vkDestroyImageView(m_context->vk_device, prefilter_map_view, nullptr);
	vmaDestroyImage(m_context->vma_allocator, texture.vk_image, texture.vma_allocation);
	vmaDestroyImage(m_context->vma_allocator, sh_intermediate.vk_image, sh_intermediate.vma_allocation);
	for (uint32_t i = 0; i < PREFILTER_MIP_LEVELS; i++)
	{
		vkDestroyImageView(m_context->vk_device, prefiltered_views[i], nullptr);
		vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 1, &prefilter_map.descriptor_sets[i]);
	}
}

void Scene::update_descriptor()
{
	VkDescriptorBufferInfo global_buffer_info = {
	    .buffer = global_buffer.vk_buffer,
	    .offset = 0,
	    .range  = sizeof(GlobalData),
	};
	VkDescriptorBufferInfo scene_buffer_info = {
	    .buffer = scene_buffer.vk_buffer,
	    .offset = 0,
	    .range  = sizeof(scene_info),
	};
	std::vector<VkDescriptorImageInfo> texture_infos;
	texture_infos.reserve(textures.size());
	for (auto &view : texture_views)
	{
		texture_infos.push_back(VkDescriptorImageInfo{
		    .sampler     = linear_sampler,
		    .imageView   = view,
		    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		});
	}
	VkDescriptorImageInfo skybox_info = {
	    .sampler     = linear_sampler,
	    .imageView   = envmap.texture_view,
	    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};
	VkWriteDescriptorSetAccelerationStructureKHR as_write = {
	    .sType                      = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
	    .accelerationStructureCount = 1,
	    .pAccelerationStructures    = &tlas.vk_as,
	};
	VkWriteDescriptorSet writes[] = {
	    {
	        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	        .dstSet           = descriptor.set,
	        .dstBinding       = 0,
	        .dstArrayElement  = 0,
	        .descriptorCount  = 1,
	        .descriptorType   = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
	        .pImageInfo       = nullptr,
	        .pBufferInfo      = &global_buffer_info,
	        .pTexelBufferView = nullptr,
	    },
	    {
	        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	        .pNext            = &as_write,
	        .dstSet           = descriptor.set,
	        .dstBinding       = 1,
	        .dstArrayElement  = 0,
	        .descriptorCount  = 1,
	        .descriptorType   = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
	        .pImageInfo       = nullptr,
	        .pBufferInfo      = nullptr,
	        .pTexelBufferView = nullptr,
	    },
	    {
	        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	        .dstSet           = descriptor.set,
	        .dstBinding       = 2,
	        .dstArrayElement  = 0,
	        .descriptorCount  = 1,
	        .descriptorType   = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
	        .pImageInfo       = nullptr,
	        .pBufferInfo      = &scene_buffer_info,
	        .pTexelBufferView = nullptr,
	    },
	    {
	        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	        .dstSet           = descriptor.set,
	        .dstBinding       = 3,
	        .dstArrayElement  = 0,
	        .descriptorCount  = static_cast<uint32_t>(texture_infos.size()),
	        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	        .pImageInfo       = texture_infos.data(),
	        .pBufferInfo      = nullptr,
	        .pTexelBufferView = nullptr,
	    },
	    {
	        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	        .dstSet           = descriptor.set,
	        .dstBinding       = 4,
	        .dstArrayElement  = 0,
	        .descriptorCount  = 1,
	        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	        .pImageInfo       = &skybox_info,
	        .pBufferInfo      = nullptr,
	        .pTexelBufferView = nullptr,
	    },
	};
	vkUpdateDescriptorSets(m_context->vk_device, 5, writes, 0, nullptr);
}

void Scene::destroy_scene()
{
	for (auto &view : texture_views)
	{
		if (view)
		{
			vkDestroyImageView(m_context->vk_device, view, nullptr);
			view = VK_NULL_HANDLE;
		}
	}
	texture_views.clear();

	for (auto &texture : textures)
	{
		if (texture.vk_image && texture.vma_allocation)
		{
			vmaDestroyImage(m_context->vma_allocator, texture.vk_image, texture.vma_allocation);
			texture.vk_image       = VK_NULL_HANDLE;
			texture.vma_allocation = VK_NULL_HANDLE;
		}
	}
	textures.clear();

	if (vertex_buffer.vk_buffer && vertex_buffer.vma_allocation)
	{
		vmaDestroyBuffer(m_context->vma_allocator, vertex_buffer.vk_buffer, vertex_buffer.vma_allocation);
		vertex_buffer.vk_buffer      = VK_NULL_HANDLE;
		vertex_buffer.vma_allocation = VK_NULL_HANDLE;
		vertex_buffer.device_address = 0;
	}

	if (index_buffer.vk_buffer && index_buffer.vma_allocation)
	{
		vmaDestroyBuffer(m_context->vma_allocator, index_buffer.vk_buffer, index_buffer.vma_allocation);
		index_buffer.vk_buffer      = VK_NULL_HANDLE;
		index_buffer.vma_allocation = VK_NULL_HANDLE;
		index_buffer.device_address = 0;
	}

	for (auto &tlas : blas)
	{
		if (tlas.vk_as)
		{
			vkDestroyAccelerationStructureKHR(m_context->vk_device, tlas.vk_as, nullptr);
			vmaDestroyBuffer(m_context->vma_allocator, tlas.buffer.vk_buffer, tlas.buffer.vma_allocation);

			tlas.vk_as                 = VK_NULL_HANDLE;
			tlas.buffer.vk_buffer      = VK_NULL_HANDLE;
			tlas.buffer.vma_allocation = VK_NULL_HANDLE;
			tlas.buffer.device_address = 0;
		}
	}
	blas.clear();

	if (tlas.vk_as)
	{
		vkDestroyAccelerationStructureKHR(m_context->vk_device, tlas.vk_as, nullptr);
		vmaDestroyBuffer(m_context->vma_allocator, tlas.buffer.vk_buffer, tlas.buffer.vma_allocation);

		tlas.vk_as                 = VK_NULL_HANDLE;
		tlas.buffer.vk_buffer      = VK_NULL_HANDLE;
		tlas.buffer.vma_allocation = VK_NULL_HANDLE;
		tlas.buffer.device_address = 0;
	}

	if (indirect_draw_buffer.vk_buffer && indirect_draw_buffer.vma_allocation)
	{
		vmaDestroyBuffer(m_context->vma_allocator, indirect_draw_buffer.vk_buffer, indirect_draw_buffer.vma_allocation);
		indirect_draw_buffer.vk_buffer      = VK_NULL_HANDLE;
		indirect_draw_buffer.vma_allocation = VK_NULL_HANDLE;
		indirect_draw_buffer.device_address = 0;
	}

	if (instance_buffer.vk_buffer && instance_buffer.vma_allocation)
	{
		vmaDestroyBuffer(m_context->vma_allocator, instance_buffer.vk_buffer, instance_buffer.vma_allocation);
		instance_buffer.vk_buffer      = VK_NULL_HANDLE;
		instance_buffer.vma_allocation = VK_NULL_HANDLE;
		instance_buffer.device_address = 0;
	}

	if (material_buffer.vk_buffer && material_buffer.vma_allocation)
	{
		vmaDestroyBuffer(m_context->vma_allocator, material_buffer.vk_buffer, material_buffer.vma_allocation);
		material_buffer.vk_buffer      = VK_NULL_HANDLE;
		material_buffer.vma_allocation = VK_NULL_HANDLE;
		material_buffer.device_address = 0;
	}

	if (scene_buffer.vk_buffer && scene_buffer.vma_allocation)
	{
		vmaDestroyBuffer(m_context->vma_allocator, scene_buffer.vk_buffer, scene_buffer.vma_allocation);
		scene_buffer.vk_buffer      = VK_NULL_HANDLE;
		scene_buffer.vma_allocation = VK_NULL_HANDLE;
		scene_buffer.device_address = 0;
	}

	if (emitter_buffer.vk_buffer && emitter_buffer.vma_allocation)
	{
		vmaDestroyBuffer(m_context->vma_allocator, emitter_buffer.vk_buffer, emitter_buffer.vma_allocation);
		emitter_buffer.vk_buffer      = VK_NULL_HANDLE;
		emitter_buffer.vma_allocation = VK_NULL_HANDLE;
		emitter_buffer.device_address = 0;
	}

	if (emitter_alias_table_buffer.vk_buffer && emitter_alias_table_buffer.vma_allocation)
	{
		vmaDestroyBuffer(m_context->vma_allocator, emitter_alias_table_buffer.vk_buffer, emitter_alias_table_buffer.vma_allocation);
		emitter_alias_table_buffer.vk_buffer      = VK_NULL_HANDLE;
		emitter_alias_table_buffer.vma_allocation = VK_NULL_HANDLE;
		emitter_alias_table_buffer.device_address = 0;
	}

	if (mesh_alias_table_buffer.vk_buffer && mesh_alias_table_buffer.vma_allocation)
	{
		vmaDestroyBuffer(m_context->vma_allocator, mesh_alias_table_buffer.vk_buffer, mesh_alias_table_buffer.vma_allocation);
		mesh_alias_table_buffer.vk_buffer      = VK_NULL_HANDLE;
		mesh_alias_table_buffer.vma_allocation = VK_NULL_HANDLE;
		mesh_alias_table_buffer.device_address = 0;
	}
}

void Scene::destroy_envmap()
{
	if (envmap.texture.vk_image && envmap.texture.vma_allocation)
	{
		vmaDestroyImage(m_context->vma_allocator, envmap.texture.vk_image, envmap.texture.vma_allocation);
		envmap.texture.vk_image       = VK_NULL_HANDLE;
		envmap.texture.vma_allocation = VK_NULL_HANDLE;
	}

	if (envmap.irradiance_sh.vk_image && envmap.irradiance_sh.vma_allocation && envmap.irradiance_sh_view)
	{
		vmaDestroyImage(m_context->vma_allocator, envmap.irradiance_sh.vk_image, envmap.irradiance_sh.vma_allocation);
		vkDestroyImageView(m_context->vk_device, envmap.irradiance_sh_view, nullptr);
		envmap.irradiance_sh.vk_image       = VK_NULL_HANDLE;
		envmap.irradiance_sh.vma_allocation = VK_NULL_HANDLE;
	}

	if (envmap.prefilter_map.vk_image && envmap.prefilter_map.vma_allocation && envmap.prefilter_map_view)
	{
		vmaDestroyImage(m_context->vma_allocator, envmap.prefilter_map.vk_image, envmap.prefilter_map.vma_allocation);
		vkDestroyImageView(m_context->vk_device, envmap.prefilter_map_view, nullptr);
		envmap.prefilter_map.vk_image       = VK_NULL_HANDLE;
		envmap.prefilter_map.vma_allocation = VK_NULL_HANDLE;
	}

	if (envmap.texture_view)
	{
		vkDestroyImageView(m_context->vk_device, envmap.texture_view, nullptr);
		envmap.texture_view = VK_NULL_HANDLE;
	}
}
