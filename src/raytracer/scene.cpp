#include "render/scene.hpp"
#include "core/log.hpp"
#include "render/context.hpp"

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#include <glm/gtc/type_ptr.hpp>
#include <stb/stb_image.h>

#include <filesystem>
#include <fstream>

struct Material
{
};

struct Vertex
{
	glm::vec4 position;        // xyz - position, w - texcoord u
	glm::vec4 normal;          // xyz - normal, w - texcoord v
};

struct Mesh
{
	uint32_t vertices_offset = 0;
	uint32_t vertices_count  = 0;
	uint32_t indices_offset  = 0;
	uint32_t indices_count   = 0;
};

struct Instance
{
	glm::mat4 transform;
	uint32_t  mesh = ~0u;
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

inline uint32_t load_texture(const Context &context, const std::string &filename, cgltf_texture *gltf_texture, std::vector<Texture> &textures, std::unordered_map<cgltf_texture *, uint32_t> &texture_map)
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

		raw_data = stbi_load_from_memory(static_cast<stbi_uc *>(raw_data), raw_size, &width, &height, &channel, req_channel);
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

	return texture_map.at(gltf_texture);
}

Buffer upload_buffer(const Context &context, VkBufferUsageFlags usage, void *data, size_t size)
{
	Buffer buffer;
	{
		VkBufferCreateInfo buffer_create_info = {
		    .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		    .size        = size,
		    .usage       = usage,
		    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};
		VmaAllocationCreateInfo allocation_create_info = {
		    .usage = VMA_MEMORY_USAGE_GPU_ONLY,
		};
		VmaAllocationInfo allocation_info = {};
		vmaCreateBuffer(context.vma_allocator, &buffer_create_info, &allocation_create_info, &buffer.vk_buffer, &buffer.vma_allocation, &allocation_info);
		if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
		{
			VkBufferDeviceAddressInfoKHR buffer_device_address_info = {
			    .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
			    .buffer = buffer.vk_buffer,
			};
			buffer.device_address = vkGetBufferDeviceAddress(context.vk_device, &buffer_device_address_info);
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
		vkCmdCopyBuffer(cmd_buffer, staging_buffer.vk_buffer, buffer.vk_buffer, 1, &copy_info);
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

	return buffer;
}

inline AccelerationStructure build_acceleration_structure(
    const Context                                  &context,
    VkCommandBuffer                                 cmd_buffer,
    const VkAccelerationStructureGeometryKHR       &geometry,
    const VkAccelerationStructureBuildRangeInfoKHR &range_info,
    VkAccelerationStructureTypeKHR                  type)
{
	AccelerationStructure acceleration_structure = {};

	VkAccelerationStructureBuildGeometryInfoKHR build_geometry_info = {};

	build_geometry_info.sType         = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	build_geometry_info.type          = type;
	build_geometry_info.flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
	build_geometry_info.mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	build_geometry_info.geometryCount = 1;
	build_geometry_info.pGeometries   = &geometry;

	// Get required build sizes
	VkAccelerationStructureBuildSizesInfoKHR build_sizes_info = {};
	build_sizes_info.sType                                    = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
	vkGetAccelerationStructureBuildSizesKHR(
	    context.vk_device,
	    VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
	    &build_geometry_info,
	    &range_info.primitiveCount,
	    &build_sizes_info);

	// Create a buffer for the acceleration structure
	{
		VkBufferCreateInfo buffer_create_info = {
		    .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		    .size        = build_sizes_info.accelerationStructureSize,
		    .usage       = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
		    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};
		VmaAllocationCreateInfo allocation_create_info = {
		    .usage = VMA_MEMORY_USAGE_GPU_ONLY};
		VmaAllocationInfo allocation_info = {};
		vmaCreateBuffer(context.vma_allocator, &buffer_create_info, &allocation_create_info, &acceleration_structure.buffer.vk_buffer, &acceleration_structure.buffer.vma_allocation, &allocation_info);

		VkAccelerationStructureCreateInfoKHR acceleration_structure_create_info = {};
		acceleration_structure_create_info.sType                                = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
		acceleration_structure_create_info.buffer                               = acceleration_structure.buffer.vk_buffer;
		acceleration_structure_create_info.size                                 = build_sizes_info.accelerationStructureSize;
		acceleration_structure_create_info.type                                 = type;
		vkCreateAccelerationStructureKHR(context.vk_device, &acceleration_structure_create_info, nullptr, &acceleration_structure.vk_as);

		build_geometry_info.mode                     = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
		build_geometry_info.srcAccelerationStructure = VK_NULL_HANDLE;
	}

	// Get the acceleration structure's handle
	VkAccelerationStructureDeviceAddressInfoKHR acceleration_device_address_info = {};
	acceleration_device_address_info.sType                                       = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
	acceleration_device_address_info.accelerationStructure                       = acceleration_structure.vk_as;
	acceleration_structure.device_address                                        = vkGetAccelerationStructureDeviceAddressKHR(context.vk_device, &acceleration_device_address_info);

	{
		VkBufferCreateInfo buffer_create_info = {
		    .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		    .size        = build_sizes_info.buildScratchSize,
		    .usage       = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};
		VmaAllocationCreateInfo allocation_create_info = {
		    .usage = VMA_MEMORY_USAGE_GPU_ONLY};
		VmaAllocationInfo allocation_info = {};
		vmaCreateBuffer(context.vma_allocator, &buffer_create_info, &allocation_create_info, &acceleration_structure.scratch_buffer.vk_buffer, &acceleration_structure.scratch_buffer.vma_allocation, &allocation_info);
		VkBufferDeviceAddressInfoKHR buffer_device_address_info = {
		    .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
		    .buffer = acceleration_structure.scratch_buffer.vk_buffer,
		};
		acceleration_structure.scratch_buffer.device_address = vkGetBufferDeviceAddress(context.vk_device, &buffer_device_address_info);
	}

	build_geometry_info.scratchData.deviceAddress = acceleration_structure.scratch_buffer.device_address;
	build_geometry_info.dstAccelerationStructure  = acceleration_structure.vk_as;

	VkAccelerationStructureBuildRangeInfoKHR *as_build_range_infos = const_cast<VkAccelerationStructureBuildRangeInfoKHR *>(&range_info);

	// Submit build task
	vkCmdBuildAccelerationStructuresKHR(
	    cmd_buffer,
	    1,
	    &build_geometry_info,
	    &as_build_range_infos);

	return acceleration_structure;
}

Scene::Scene(const std::string &filename, const Context &context) :
    m_context(&context)
{
	load_scene(filename);
}

Scene::~Scene()
{
	destroy_scene();
}

void Scene::load_scene(const std::string &filename)
{
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

	std::vector<Material> materials;
	std::vector<Mesh>     meshes;
	std::vector<Instance> instances;

	// Load geometry
	{
		std::vector<uint32_t> indices;
		std::vector<Vertex>   vertices;

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
				};

				indices.resize(indices.size() + primitive.indices->count);
				for (size_t i = 0; i < primitive.indices->count; i += 3)
				{
					indices[mesh.indices_offset + i + 0] = static_cast<uint32_t>(cgltf_accessor_read_index(primitive.indices, i + 0));
					indices[mesh.indices_offset + i + 1] = static_cast<uint32_t>(cgltf_accessor_read_index(primitive.indices, i + 2));
					indices[mesh.indices_offset + i + 2] = static_cast<uint32_t>(cgltf_accessor_read_index(primitive.indices, i + 1));
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

		vertices_count = static_cast<uint32_t>(vertices.size());
		indices_count  = static_cast<uint32_t>(indices.size());

		vertex_buffer = upload_buffer(*m_context, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, vertices.data(), vertices.size() * sizeof(Vertex));
		index_buffer  = upload_buffer(*m_context, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, indices.data(), indices.size() * sizeof(uint32_t));

		m_context->set_object_name(VkDebugUtilsObjectNameInfoEXT{
		    .sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
		    .objectType   = VK_OBJECT_TYPE_BUFFER,
		    .objectHandle = (uint64_t) vertex_buffer.vk_buffer,
		    .pObjectName  = "Vertex Buffer",
		});

		m_context->set_object_name(VkDebugUtilsObjectNameInfoEXT{
		    .sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
		    .objectType   = VK_OBJECT_TYPE_BUFFER,
		    .objectHandle = (uint64_t) index_buffer.vk_buffer,
		    .pObjectName  = "Index Buffer",
		});
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
				for (auto &mesh : mesh_map.at(node.mesh))
				{
					Instance instance = {};
					instance.mesh     = mesh;
					std::memcpy(glm::value_ptr(instance.transform), matrix, sizeof(instance.transform));
					instances.push_back(instance);
				}
			}
		}
	}

	// Build acceleration structure
	{
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
				    .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
				};

				VkAccelerationStructureBuildRangeInfoKHR range_info = {
				    .primitiveCount  = mesh.indices_count / 3,
				    .primitiveOffset = mesh.indices_offset * sizeof(uint32_t),
				    .firstVertex     = mesh.vertices_offset,
				    .transformOffset = 0,
				};

				AccelerationStructure acceleration_structure = build_acceleration_structure(*m_context, cmd_buffer, as_geometry, range_info, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);
				/*
				VkAccelerationStructureBuildSizesInfoKHR build_sizes_info = {
				    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
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

				vkGetAccelerationStructureBuildSizesKHR(
				    m_context->vk_device,
				    VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
				    &build_geometry_info,
				    &range_info.primitiveCount,
				    &build_sizes_info);

				AccelerationStructure acceleration_structure = {};

				// Allocate buffer
				{
				    VkBufferCreateInfo create_info = {
				        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				        .size  = build_sizes_info.accelerationStructureSize,
				        .usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
				                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
				        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
				    };
				    VmaAllocationCreateInfo allocation_create_info = {
				        .usage = VMA_MEMORY_USAGE_GPU_ONLY};
				    VmaAllocationInfo allocation_info = {};
				    vmaCreateBuffer(m_context->vma_allocator, &create_info, &allocation_create_info, &acceleration_structure.buffer.vk_buffer, &acceleration_structure.buffer.vma_allocation, &allocation_info);
				}

				// Create handle
				{
				    VkAccelerationStructureCreateInfoKHR create_info = {
				        .sType  = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
				        .buffer = acceleration_structure.buffer.vk_buffer,
				        .size   = build_sizes_info.accelerationStructureSize,
				        .type   = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
				    };
				    vkCreateAccelerationStructureKHR(m_context->vk_device, &create_info, nullptr, &acceleration_structure.vk_as);
				}

				// Get device address
				{
				    VkAccelerationStructureDeviceAddressInfoKHR address_info = {
				        .sType                 = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
				        .accelerationStructure = acceleration_structure.vk_as,
				    };
				    acceleration_structure.device_address = vkGetAccelerationStructureDeviceAddressKHR(m_context->vk_device, &address_info);
				}

				// Allocate scratch buffer
				{
				    VkBufferCreateInfo create_info = {
				        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				        .size        = build_sizes_info.buildScratchSize,
				        .usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
				        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
				    };
				    VmaAllocationCreateInfo allocation_create_info = {
				        .usage = VMA_MEMORY_USAGE_GPU_ONLY};
				    VmaAllocationInfo allocation_info = {};
				    vmaCreateBuffer(m_context->vma_allocator, &create_info, &allocation_create_info, &acceleration_structure.scratch_buffer.vk_buffer, &acceleration_structure.scratch_buffer.vma_allocation, &allocation_info);
				    VkBufferDeviceAddressInfoKHR buffer_device_address_info = {
				        .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
				        .buffer = acceleration_structure.scratch_buffer.vk_buffer,
				    };
				    acceleration_structure.scratch_buffer.device_address = vkGetBufferDeviceAddress(m_context->vk_device, &buffer_device_address_info);
				}

				{
				    VkPhysicalDeviceAccelerationStructurePropertiesKHR properties = {
				        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR,
				        .pNext = NULL,
				    };
				    VkPhysicalDeviceProperties2 dev_props2 = {
				        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
				        .pNext = &properties,
				    };
				    vkGetPhysicalDeviceProperties2(m_context->vk_physical_device, &dev_props2);
				    build_geometry_info.scratchData.deviceAddress = acceleration_structure.scratch_buffer.device_address;
				    // align(acceleration_structure.scratch_buffer.device_address, properties.minAccelerationStructureScratchOffsetAlignment);
				    build_geometry_info.dstAccelerationStructure = acceleration_structure.vk_as;
				}

				VkAccelerationStructureBuildRangeInfoKHR *as_build_range_infos = const_cast<VkAccelerationStructureBuildRangeInfoKHR *>(&range_info);
				vkCmdBuildAccelerationStructuresKHR(
				    cmd_buffer,
				    1,
				    &build_geometry_info,
				    &as_build_range_infos);*/

				blas.push_back(acceleration_structure);
			}
		}

		// Build top level acceleration structure
		{
			std::vector<VkAccelerationStructureInstanceKHR> vk_instances;
			vk_instances.reserve(instances.size());
			for (auto &instance : instances)
			{
				VkAccelerationStructureInstanceKHR vk_instance = {};

				auto transform = glm::mat3x4(glm::transpose(instance.transform));

				std::memcpy(&vk_instance.transform, &transform, sizeof(VkTransformMatrixKHR));
				vk_instance.instanceCustomIndex                    = 0;
				vk_instance.mask                                   = 0xFF;
				vk_instance.instanceShaderBindingTableRecordOffset = 0;
				vk_instance.flags                                  = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
				vk_instance.accelerationStructureReference         = blas.at(instance.mesh).device_address;

				vk_instances.emplace_back(std::move(vk_instance));
			}

			AccelerationStructure acceleration_structure = {};
			{
				acceleration_structure.instance_buffer = upload_buffer(*m_context, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, vk_instances.data(), vk_instances.size() * sizeof(VkAccelerationStructureInstanceKHR));
			}

			VkAccelerationStructureGeometryKHR as_geometry = {
			    .sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
			    .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
			    .geometry     = {
			            .instances = {
			                .sType           = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
			                .arrayOfPointers = VK_FALSE,
			                .data            = acceleration_structure.instance_buffer.device_address,
                    },
                },
			    .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
			};

			VkAccelerationStructureBuildRangeInfoKHR range_info = {};
			range_info.primitiveCount                           = static_cast<uint32_t>(instances.size());

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

			// Allocate buffer
			{
				VkBufferCreateInfo create_info = {
				    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				    .size  = build_sizes_info.accelerationStructureSize,
				    .usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
				             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
				    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
				};
				VmaAllocationCreateInfo allocation_create_info = {
				    .usage = VMA_MEMORY_USAGE_GPU_ONLY};
				VmaAllocationInfo allocation_info = {};
				vmaCreateBuffer(m_context->vma_allocator, &create_info, &allocation_create_info, &acceleration_structure.buffer.vk_buffer, &acceleration_structure.buffer.vma_allocation, &allocation_info);
			}

			// Create handle
			{
				VkAccelerationStructureCreateInfoKHR create_info = {
				    .sType  = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
				    .buffer = acceleration_structure.buffer.vk_buffer,
				    .size   = build_sizes_info.accelerationStructureSize,
				    .type   = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
				};
				vkCreateAccelerationStructureKHR(m_context->vk_device, &create_info, nullptr, &acceleration_structure.vk_as);
			}

			// Get device address
			{
				VkAccelerationStructureDeviceAddressInfoKHR address_info = {
				    .sType                 = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
				    .accelerationStructure = acceleration_structure.vk_as,
				};
				acceleration_structure.device_address = vkGetAccelerationStructureDeviceAddressKHR(m_context->vk_device, &address_info);
			}

			// Allocate scratch buffer
			{
				VkBufferCreateInfo create_info = {
				    .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				    .size        = build_sizes_info.buildScratchSize,
				    .usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
				    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
				};
				VmaAllocationCreateInfo allocation_create_info = {
				    .usage = VMA_MEMORY_USAGE_GPU_ONLY};
				VmaAllocationInfo allocation_info = {};
				vmaCreateBuffer(m_context->vma_allocator, &create_info, &allocation_create_info, &acceleration_structure.scratch_buffer.vk_buffer, &acceleration_structure.scratch_buffer.vma_allocation, &allocation_info);
				VkBufferDeviceAddressInfoKHR buffer_device_address_info = {
				    .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
				    .buffer = acceleration_structure.scratch_buffer.vk_buffer,
				};
				acceleration_structure.scratch_buffer.device_address = vkGetBufferDeviceAddress(m_context->vk_device, &buffer_device_address_info);
			}

			{
				VkPhysicalDeviceAccelerationStructurePropertiesKHR properties = {
				    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR,
				    .pNext = NULL,
				};
				VkPhysicalDeviceProperties2 dev_props2 = {
				    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
				    .pNext = &properties,
				};
				vkGetPhysicalDeviceProperties2(m_context->vk_physical_device, &dev_props2);
				build_geometry_info.scratchData.deviceAddress = align(acceleration_structure.scratch_buffer.device_address, properties.minAccelerationStructureScratchOffsetAlignment);
				build_geometry_info.dstAccelerationStructure  = acceleration_structure.vk_as;
			}

			VkAccelerationStructureBuildRangeInfoKHR *as_build_range_infos = const_cast<VkAccelerationStructureBuildRangeInfoKHR *>(&range_info);
			vkCmdBuildAccelerationStructuresKHR(
			    cmd_buffer,
			    1,
			    &build_geometry_info,
			    &as_build_range_infos);

			tlas = acceleration_structure;
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
	}

	for (size_t i = 0; i < raw_data->materials_count; i++)
	{
		auto &raw_material = raw_data->materials[i];
		materials.push_back(Material{});
		material_map[&raw_material] = materials.size() - 1;

		if (raw_material.has_pbr_metallic_roughness)
		{
			load_texture(*m_context, filename, raw_material.pbr_metallic_roughness.base_color_texture.texture, textures, texture_map);
		}
	}
}

void Scene::load_envmap(const std::string &filename)
{
}

void Scene::destroy_scene()
{
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

	for (auto &acceleration_structure : blas)
	{
		if (acceleration_structure.vk_as)
		{
			vkDestroyAccelerationStructureKHR(m_context->vk_device, acceleration_structure.vk_as, nullptr);
			vmaDestroyBuffer(m_context->vma_allocator, acceleration_structure.buffer.vk_buffer, acceleration_structure.buffer.vma_allocation);
			vmaDestroyBuffer(m_context->vma_allocator, acceleration_structure.scratch_buffer.vk_buffer, acceleration_structure.scratch_buffer.vma_allocation);

			acceleration_structure.vk_as                         = VK_NULL_HANDLE;
			acceleration_structure.buffer.vk_buffer              = VK_NULL_HANDLE;
			acceleration_structure.buffer.vma_allocation         = VK_NULL_HANDLE;
			acceleration_structure.buffer.device_address         = 0;
			acceleration_structure.scratch_buffer.vk_buffer      = VK_NULL_HANDLE;
			acceleration_structure.scratch_buffer.vma_allocation = VK_NULL_HANDLE;
			acceleration_structure.scratch_buffer.device_address = 0;
		}
	}
	blas.clear();

	if (tlas.vk_as)
	{
		vkDestroyAccelerationStructureKHR(m_context->vk_device, tlas.vk_as, nullptr);
		vmaDestroyBuffer(m_context->vma_allocator, tlas.buffer.vk_buffer, tlas.buffer.vma_allocation);
		vmaDestroyBuffer(m_context->vma_allocator, tlas.scratch_buffer.vk_buffer, tlas.scratch_buffer.vma_allocation);
		vmaDestroyBuffer(m_context->vma_allocator, tlas.instance_buffer.vk_buffer, tlas.instance_buffer.vma_allocation);

		tlas.vk_as                          = VK_NULL_HANDLE;
		tlas.buffer.vk_buffer               = VK_NULL_HANDLE;
		tlas.buffer.vma_allocation          = VK_NULL_HANDLE;
		tlas.buffer.device_address          = 0;
		tlas.scratch_buffer.vk_buffer       = VK_NULL_HANDLE;
		tlas.scratch_buffer.vma_allocation  = VK_NULL_HANDLE;
		tlas.scratch_buffer.device_address  = 0;
		tlas.instance_buffer.vk_buffer      = VK_NULL_HANDLE;
		tlas.instance_buffer.vma_allocation = VK_NULL_HANDLE;
		tlas.instance_buffer.device_address = 0;
	}
}
