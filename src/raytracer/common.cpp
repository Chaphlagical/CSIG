#include "render/common.hpp"

#include <stb/stb_image.h>
#include <spdlog/fmt/fmt.h>

#include <cmath>
#include <string>

std::pair<Texture, VkImageView> load_texture(const Context &context, const std::string &filename)
{
	uint8_t *raw_data = nullptr;
	size_t   raw_size = 0;
	int32_t  width = 0, height = 0, channel = 0, req_channel = 4;
	raw_data = stbi_load(filename.c_str(), &width, &height, &channel, req_channel);

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

	// Texture transition: transfer dest -> shader read
	{
		VkImageMemoryBarrier image_barrier =
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

	// Create views
	VkImageView view = VK_NULL_HANDLE;
	{
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
	}

	return std::make_pair(texture, view);
}

BlueNoise::BlueNoise(const Context &context) :
    m_context(&context)
{
	static const char *scrambling_ranking_textures[] = {
	    "assets/textures/blue_noise/scrambling_ranking_128x128_2d_1spp.png",
	    "assets/textures/blue_noise/scrambling_ranking_128x128_2d_2spp.png",
	    "assets/textures/blue_noise/scrambling_ranking_128x128_2d_4spp.png",
	    "assets/textures/blue_noise/scrambling_ranking_128x128_2d_8spp.png",
	    "assets/textures/blue_noise/scrambling_ranking_128x128_2d_16spp.png",
	    "assets/textures/blue_noise/scrambling_ranking_128x128_2d_32spp.png",
	    "assets/textures/blue_noise/scrambling_ranking_128x128_2d_64spp.png",
	    "assets/textures/blue_noise/scrambling_ranking_128x128_2d_128spp.png",
	    "assets/textures/blue_noise/scrambling_ranking_128x128_2d_256spp.png",
	};

	for (size_t i = 0; i < 9; i++)
	{
		std::tie(scrambling_ranking_images[i], scrambling_ranking_image_views[i]) = load_texture(*m_context, scrambling_ranking_textures[i]);
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) scrambling_ranking_images[i].vk_image, fmt::format("Scrambing Rank Image - {}", i).c_str());
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) scrambling_ranking_image_views[i], fmt::format("Scrambing Rank Image View - {}", i).c_str());
	}

	std::tie(sobol_image, sobol_image_view) = load_texture(*m_context, "assets/textures/blue_noise/sobol_256_4d.png");
	m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) sobol_image.vk_image, "Sobol Image");
	m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) sobol_image_view, "Sobol Image View");
}

BlueNoise::~BlueNoise()
{
	for (auto &view : scrambling_ranking_image_views)
	{
		vkDestroyImageView(m_context->vk_device, view, nullptr);
		view = VK_NULL_HANDLE;
	}
	vkDestroyImageView(m_context->vk_device, sobol_image_view, nullptr);
	for (auto &image : scrambling_ranking_images)
	{
		vmaDestroyImage(m_context->vma_allocator, image.vk_image, image.vma_allocation);
		image.vk_image       = VK_NULL_HANDLE;
		image.vma_allocation = VK_NULL_HANDLE;
	}
	vmaDestroyImage(m_context->vma_allocator, sobol_image.vk_image, sobol_image.vma_allocation);
}
