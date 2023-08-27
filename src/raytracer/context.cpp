#define VOLK_IMPLEMENTATION
#define VMA_IMPLEMENTATION

#include "context.hpp"
#include "shader_compiler.hpp"

#include <spdlog/spdlog.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include <glm/gtx/hash.hpp>

#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <vector>

namespace std
{
template <>
struct hash<std::unordered_map<std::string, std::string>>
{
	size_t operator()(const std::unordered_map<std::string, std::string> &m) const
	{
		size_t hash = 0;
		for (const auto &[key, val] : m)
		{
			glm::detail::hash_combine(hash, std::hash<std::string>{}(key));
			glm::detail::hash_combine(hash, std::hash<std::string>{}(val));
		}
		return hash;
	}
};
}        // namespace std

static VkDebugUtilsMessengerEXT vkDebugUtilsMessengerEXT;

inline size_t align(size_t x, size_t alignment)
{
	return (x + (alignment - 1)) & ~(alignment - 1);
}

inline const std::vector<const char *> get_instance_extension_supported(const std::vector<const char *> &extensions)
{
	uint32_t extension_count = 0;
	vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr);

	std::vector<VkExtensionProperties> device_extensions(extension_count);
	vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, device_extensions.data());

	std::vector<const char *> result;

	for (const auto &extension : extensions)
	{
		bool found = false;
		for (const auto &device_extension : device_extensions)
		{
			if (strcmp(extension, device_extension.extensionName) == 0)
			{
				result.emplace_back(extension);
				found = true;
				break;
			}
		}
	}

	return result;
}

inline bool check_layer_supported(const char *layer_name)
{
	uint32_t layer_count;
	vkEnumerateInstanceLayerProperties(&layer_count, nullptr);

	std::vector<VkLayerProperties> layers(layer_count);
	vkEnumerateInstanceLayerProperties(&layer_count, layers.data());

	for (const auto &layer : layers)
	{
		if (strcmp(layer.layerName, layer_name) == 0)
		{
			return true;
		}
	}

	return false;
}

static inline VKAPI_ATTR VkBool32 VKAPI_CALL validation_callback(VkDebugUtilsMessageSeverityFlagBitsEXT msg_severity, VkDebugUtilsMessageTypeFlagsEXT msg_type, const VkDebugUtilsMessengerCallbackDataEXT *callback_data, void *user_data)
{
	if (msg_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)
	{
		spdlog::info(callback_data->pMessage);
	}
	else if (msg_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
	{
		spdlog::warn(callback_data->pMessage);
	}
	else if (msg_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
	{
		spdlog::error(callback_data->pMessage);
	}

	return VK_FALSE;
}

inline uint32_t score_physical_device(VkPhysicalDevice physical_device, const std::vector<const char *> &device_extensions, std::vector<const char *> &support_device_extensions)
{
	uint32_t score = 0;

	// Check extensions
	uint32_t device_extension_properties_count = 0;
	vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &device_extension_properties_count, nullptr);

	std::vector<VkExtensionProperties> extension_properties(device_extension_properties_count);
	vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &device_extension_properties_count, extension_properties.data());

	for (auto &device_extension : device_extensions)
	{
		for (auto &support_extension : extension_properties)
		{
			if (std::strcmp(device_extension, support_extension.extensionName) == 0)
			{
				support_device_extensions.push_back(device_extension);
				score += 100;
				break;
			}
		}
	}

	VkPhysicalDeviceProperties properties = {};

	vkGetPhysicalDeviceProperties(physical_device, &properties);

	// Score discrete gpu
	if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
	{
		score += 1000;
	}

	score += properties.limits.maxImageDimension2D;
	return score;
}

inline VkPhysicalDevice select_physical_device(const std::vector<VkPhysicalDevice> &physical_devices, const std::vector<const char *> &device_extensions)
{
	// Score - GPU
	uint32_t         score  = 0;
	VkPhysicalDevice handle = VK_NULL_HANDLE;
	for (auto &gpu : physical_devices)
	{
		std::vector<const char *> support_extensions;

		uint32_t tmp_score = score_physical_device(gpu, device_extensions, support_extensions);
		if (tmp_score > score)
		{
			score  = tmp_score;
			handle = gpu;
		}
	}

	return handle;
}

inline std::optional<uint32_t> get_queue_family_index(const std::vector<VkQueueFamilyProperties> &queue_family_properties, VkQueueFlagBits queue_flag)
{
	// Dedicated queue for compute
	// Try to find a queue family index that supports compute but not graphics
	if (queue_flag & VK_QUEUE_COMPUTE_BIT)
	{
		for (uint32_t i = 0; i < static_cast<uint32_t>(queue_family_properties.size()); i++)
		{
			if ((queue_family_properties[i].queueFlags & queue_flag) && ((queue_family_properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0))
			{
				return i;
				break;
			}
		}
	}

	// Dedicated queue for transfer
	// Try to find a queue family index that supports transfer but not graphics and compute
	if (queue_flag & VK_QUEUE_TRANSFER_BIT)
	{
		for (uint32_t i = 0; i < static_cast<uint32_t>(queue_family_properties.size()); i++)
		{
			if ((queue_family_properties[i].queueFlags & queue_flag) && ((queue_family_properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) && ((queue_family_properties[i].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0))
			{
				return i;
				break;
			}
		}
	}

	// For other queue types or if no separate compute queue is present, return the first one to support the requested flags
	for (uint32_t i = 0; i < static_cast<uint32_t>(queue_family_properties.size()); i++)
	{
		if (queue_family_properties[i].queueFlags & queue_flag)
		{
			return i;
			break;
		}
	}

	return std::optional<uint32_t>();
}

inline const std::vector<const char *> get_device_extension_support(VkPhysicalDevice physical_device, const std::vector<const char *> &extensions)
{
	std::vector<const char *> result;

	uint32_t device_extension_properties_count = 0;
	vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &device_extension_properties_count, nullptr);

	std::vector<VkExtensionProperties> extension_properties(device_extension_properties_count);
	vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &device_extension_properties_count, extension_properties.data());

	for (auto &device_extension : extensions)
	{
		bool enable = false;
		for (auto &support_extension : extension_properties)
		{
			if (std::strcmp(device_extension, support_extension.extensionName) == 0)
			{
				result.push_back(device_extension);
				enable = true;
				break;
			}
		}
	}

	return result;
}

BarrierBuilder::BarrierBuilder(CommandBufferRecorder &recorder) :
    recorder(recorder)
{
}

BarrierBuilder &BarrierBuilder::add_image_barrier(VkImage image, VkAccessFlags src_mask, VkAccessFlags dst_mask, VkImageLayout old_layout, VkImageLayout new_layout, const VkImageSubresourceRange &range)
{
	image_barriers.push_back(VkImageMemoryBarrier{
	    .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask       = src_mask,
	    .dstAccessMask       = dst_mask,
	    .oldLayout           = old_layout,
	    .newLayout           = new_layout,
	    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .image               = image,
	    .subresourceRange    = range,
	});
	return *this;
}

BarrierBuilder &BarrierBuilder::add_buffer_barrier(VkBuffer buffer, VkAccessFlags src_mask, VkAccessFlags dst_mask, size_t size, size_t offset)
{
	buffer_barriers.push_back(VkBufferMemoryBarrier{
	    .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
	    .pNext               = nullptr,
	    .srcAccessMask       = src_mask,
	    .dstAccessMask       = dst_mask,
	    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .buffer              = buffer,
	    .offset              = offset,
	    .size                = size,
	});
	return *this;
}

CommandBufferRecorder &BarrierBuilder::insert(VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
	vkCmdPipelineBarrier(
	    recorder.cmd_buffer,
	    src_stage,
	    dst_stage,
	    0, 0, nullptr,
	    static_cast<uint32_t>(buffer_barriers.size()), buffer_barriers.data(),
	    static_cast<uint32_t>(image_barriers.size()), image_barriers.data());
	return recorder;
}

CommandBufferRecorder::CommandBufferRecorder(const Context &context, bool compute) :
    context(&context), compute(compute)
{
	VkCommandBufferAllocateInfo allocate_info =
	    {
	        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	        .commandPool        = compute ? this->context->compute_cmd_pool : this->context->graphics_cmd_pool,
	        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	        .commandBufferCount = 1,
	    };
	vkAllocateCommandBuffers(this->context->vk_device, &allocate_info, &cmd_buffer);
}

CommandBufferRecorder &CommandBufferRecorder::begin()
{
	VkCommandBufferBeginInfo begin_info = {
	    .sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	    .flags            = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	    .pInheritanceInfo = nullptr,
	};
	vkBeginCommandBuffer(cmd_buffer, &begin_info);
	return *this;
}

CommandBufferRecorder &CommandBufferRecorder::end()
{
	vkEndCommandBuffer(cmd_buffer);
	return *this;
}

CommandBufferRecorder &CommandBufferRecorder::begin_marker(const std::string &name)
{
#ifdef DEBUG
	VkDebugUtilsLabelEXT label = {
	    .sType      = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
	    .pLabelName = name.c_str(),
	    .color      = {0, 1, 0, 0},
	};
	vkCmdBeginDebugUtilsLabelEXT(cmd_buffer, &label);
#endif        // DEBUG
	return *this;
}

CommandBufferRecorder &CommandBufferRecorder::end_marker()
{
#ifdef DEBUG
	vkCmdEndDebugUtilsLabelEXT(cmd_buffer);
#endif        // DEBUG
	return *this;
}

CommandBufferRecorder &CommandBufferRecorder::add_color_attachment(VkImageView view, VkAttachmentLoadOp load_op, VkAttachmentStoreOp store_op, VkClearColorValue clear_value)
{
	color_attachments.push_back(VkRenderingAttachmentInfo{
	    .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
	    .imageView   = view,
	    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	    .loadOp      = load_op,
	    .storeOp     = store_op,
	    .clearValue  = {.color = clear_value},
	});
	return *this;
}

CommandBufferRecorder &CommandBufferRecorder::add_depth_attachment(VkImageView view, VkAttachmentLoadOp load_op, VkAttachmentStoreOp store_op, VkClearDepthStencilValue clear_value)
{
	depth_stencil_attachment = VkRenderingAttachmentInfo{
	    .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
	    .imageView   = view,
	    .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
	    .loadOp      = load_op,
	    .storeOp     = store_op,
	    .clearValue  = {.depthStencil = clear_value},
	};
	return *this;
}

CommandBufferRecorder &CommandBufferRecorder::begin_render_pass(uint32_t width, uint32_t height, VkRenderPass render_pass, VkFramebuffer frame_buffer, VkClearValue clear_value)
{
	VkRect2D area      = {};
	area.extent.width  = width;
	area.extent.height = height;

	VkRenderPassBeginInfo begin_info = {};
	begin_info.sType                 = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	begin_info.renderPass            = render_pass;
	begin_info.renderArea            = area;
	begin_info.framebuffer           = frame_buffer;
	begin_info.clearValueCount       = 1;
	begin_info.pClearValues          = &clear_value;

	vkCmdBeginRenderPass(cmd_buffer, &begin_info, VK_SUBPASS_CONTENTS_INLINE);

	return *this;
}

CommandBufferRecorder &CommandBufferRecorder::end_render_pass()
{
	vkCmdEndRenderPass(cmd_buffer);
	return *this;
}

CommandBufferRecorder &CommandBufferRecorder::begin_rendering(uint32_t width, uint32_t height, uint32_t layer)
{
	VkRenderingInfo rendering_info = {
	    .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
	    .renderArea           = {0, 0, width, height},
	    .layerCount           = layer,
	    .colorAttachmentCount = static_cast<uint32_t>(color_attachments.size()),
	    .pColorAttachments    = color_attachments.data(),
	    .pDepthAttachment     = depth_stencil_attachment.has_value() ? &depth_stencil_attachment.value() : nullptr,
	};
	vkCmdBeginRendering(cmd_buffer, &rendering_info);
	return *this;
}

CommandBufferRecorder &CommandBufferRecorder::end_rendering()
{
	vkCmdEndRendering(cmd_buffer);
	color_attachments.clear();
	depth_stencil_attachment = std::optional<VkRenderingAttachmentInfo>{};
	return *this;
}

CommandBufferRecorder &CommandBufferRecorder::update_buffer(VkBuffer buffer, void *data, size_t size, size_t offset)
{
	vkCmdUpdateBuffer(cmd_buffer, buffer, 0, size, data);
	return *this;
}

CommandBufferRecorder &CommandBufferRecorder::push_constants(VkPipelineLayout pipeline_layout, VkShaderStageFlags stages, void *data, uint32_t size)
{
	vkCmdPushConstants(cmd_buffer, pipeline_layout, stages, 0, size, data);
	return *this;
}

CommandBufferRecorder &CommandBufferRecorder::copy_buffer_to_image(VkBuffer buffer, VkImage image, const VkExtent3D &extent, const VkOffset3D &offset, const VkImageSubresourceLayers &range)
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
	    .imageOffset = offset,
	    .imageExtent = extent,
	};
	vkCmdCopyBufferToImage(cmd_buffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_info);
	return *this;
}

CommandBufferRecorder &CommandBufferRecorder::bind_descriptor_set(VkPipelineBindPoint bind_point, VkPipelineLayout pipeline_layout, const std::vector<VkDescriptorSet> &descriptor_sets)
{
	vkCmdBindDescriptorSets(cmd_buffer, bind_point, pipeline_layout, 0, static_cast<uint32_t>(descriptor_sets.size()), descriptor_sets.data(), 0, nullptr);
	return *this;
}

CommandBufferRecorder &CommandBufferRecorder::bind_pipeline(VkPipelineBindPoint bind_point, VkPipeline pipeline)
{
	vkCmdBindPipeline(cmd_buffer, bind_point, pipeline);
	return *this;
}

CommandBufferRecorder &CommandBufferRecorder::bind_vertex_buffers(const std::vector<VkBuffer> &vertex_buffers)
{
	std::vector<size_t> offsets(vertex_buffers.size(), 0);
	vkCmdBindVertexBuffers(cmd_buffer, 0, static_cast<uint32_t>(vertex_buffers.size()), vertex_buffers.data(), offsets.data());
	return *this;
}

CommandBufferRecorder &CommandBufferRecorder::bind_index_buffer(VkBuffer index_buffer, size_t offset, VkIndexType type)
{
	vkCmdBindIndexBuffer(cmd_buffer, index_buffer, offset, type);
	return *this;
}

CommandBufferRecorder &CommandBufferRecorder::dispatch(const glm::uvec3 &thread_num, const glm::uvec3 &group_size)
{
	glm::uvec3 group_count = thread_num / group_size;
	vkCmdDispatch(cmd_buffer, group_count.x, group_count.y, group_count.z);
	return *this;
}

CommandBufferRecorder &CommandBufferRecorder::draw_mesh_task(const glm::uvec3 &thread_num, const glm::uvec3 &group_size)
{
	glm::uvec3 group_count = thread_num / group_size;
	vkCmdDrawMeshTasksEXT(cmd_buffer, group_count.x, group_count.y, group_count.z);
	return *this;
}

CommandBufferRecorder &CommandBufferRecorder::draw_indexed(uint32_t index_count, uint32_t instance_count, uint32_t first_index, int32_t vertex_offset, uint32_t first_instance)
{
	vkCmdDrawIndexed(cmd_buffer, index_count, instance_count, first_index, vertex_offset, first_instance);
	return *this;
}

CommandBufferRecorder &CommandBufferRecorder::draw_indexed_indirect(VkBuffer indirect_buffer, uint32_t count, size_t offset, uint32_t stride)
{
	vkCmdDrawIndexedIndirect(cmd_buffer, indirect_buffer, offset, count, stride);
	return *this;
}

CommandBufferRecorder &CommandBufferRecorder::fill_buffer(VkBuffer buffer, uint32_t data, size_t size, size_t offset)
{
	vkCmdFillBuffer(cmd_buffer, buffer, offset, size, data);
	return *this;
}

CommandBufferRecorder &CommandBufferRecorder::clear_color_image(VkImage image, const VkClearColorValue &clear_value, const VkImageSubresourceRange &range)
{
	vkCmdClearColorImage(cmd_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_value, 1, &range);
	return *this;
}

CommandBufferRecorder &CommandBufferRecorder::build_acceleration_structure(const VkAccelerationStructureBuildGeometryInfoKHR &geometry_info, const VkAccelerationStructureBuildRangeInfoKHR *range_info)
{
	vkCmdBuildAccelerationStructuresKHR(
	    cmd_buffer,
	    1,
	    &geometry_info,
	    &range_info);
	return *this;
}

CommandBufferRecorder &CommandBufferRecorder::execute(std::function<void(VkCommandBuffer)> &&func)
{
	func(cmd_buffer);
	return *this;
}

CommandBufferRecorder &CommandBufferRecorder::execute(std::function<void(CommandBufferRecorder &)> &&func)
{
	func(*this);
	return *this;
}

BarrierBuilder CommandBufferRecorder::insert_barrier()
{
	return BarrierBuilder(*this);
}

CommandBufferRecorder &CommandBufferRecorder::generate_mipmap(VkImage image, uint32_t width, uint32_t height, uint32_t mip_level)
{
	if (mip_level <= 1)
	{
		return *this;
	}

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
			    .image               = image,
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
		    image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		    image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
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
			    .image               = image,
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

	{
		VkImageMemoryBarrier image_barrier =
		    {
		        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
		        .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
		        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image               = image,
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

	return *this;
}

void CommandBufferRecorder::flush()
{
	VkFence           fence       = VK_NULL_HANDLE;
	VkFenceCreateInfo create_info = {
	    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
	    .flags = 0,
	};
	vkCreateFence(context->vk_device, &create_info, nullptr, &fence);
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
	vkQueueSubmit(compute ? context->compute_queue : context->graphics_queue, 1, &submit_info, fence);

	// Wait
	vkWaitForFences(context->vk_device, 1, &fence, VK_TRUE, UINT64_MAX);
	vkResetFences(context->vk_device, 1, &fence);

	// Release resource
	vkDestroyFence(context->vk_device, fence, nullptr);
	vkFreeCommandBuffers(context->vk_device, compute ? context->compute_cmd_pool : context->graphics_cmd_pool, 1, &cmd_buffer);
}

CommandBufferRecorder &CommandBufferRecorder::submit(const std::vector<VkSemaphore> &signal_semaphores, const std::vector<VkSemaphore> &wait_semaphores, const std::vector<VkPipelineStageFlags> &wait_stages, VkFence signal_fence)
{
	VkSubmitInfo submit_info = {
	    .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .waitSemaphoreCount   = static_cast<uint32_t>(wait_semaphores.size()),
	    .pWaitSemaphores      = wait_semaphores.data(),
	    .pWaitDstStageMask    = wait_stages.data(),
	    .commandBufferCount   = 1,
	    .pCommandBuffers      = &cmd_buffer,
	    .signalSemaphoreCount = static_cast<uint32_t>(signal_semaphores.size()),
	    .pSignalSemaphores    = signal_semaphores.data(),
	};
	vkQueueSubmit(context->graphics_queue, 1, &submit_info, signal_fence);
	return *this;
}

CommandBufferRecorder &CommandBufferRecorder::present(const std::vector<VkSemaphore> &wait_semaphores)
{
	VkPresentInfoKHR present_info   = {};
	present_info.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present_info.pNext              = NULL;
	present_info.swapchainCount     = 1;
	present_info.pSwapchains        = &context->vk_swapchain;
	present_info.pImageIndices      = &context->image_index;
	present_info.pWaitSemaphores    = wait_semaphores.data();
	present_info.waitSemaphoreCount = static_cast<uint32_t>(wait_semaphores.size());
	vkQueuePresentKHR(context->present_queue, &present_info);
	return *this;
}

DescriptorLayoutBuilder::DescriptorLayoutBuilder(const Context &context) :
    context(&context)
{
}

DescriptorLayoutBuilder &DescriptorLayoutBuilder::add_descriptor_binding(uint32_t binding, VkDescriptorType type, VkShaderStageFlags stage, uint32_t count)
{
	bindings.emplace_back(VkDescriptorSetLayoutBinding{
	    .binding         = binding,
	    .descriptorType  = type,
	    .descriptorCount = count,
	    .stageFlags      = stage,
	});
	binding_flags.push_back(0);
	return *this;
}

DescriptorLayoutBuilder &DescriptorLayoutBuilder::add_descriptor_bindless_binding(uint32_t binding, VkDescriptorType type, VkShaderStageFlags stage, uint32_t count)
{
	bindless = true;
	bindings.emplace_back(VkDescriptorSetLayoutBinding{
	    .binding         = binding,
	    .descriptorType  = type,
	    .descriptorCount = count,
	    .stageFlags      = stage,
	});
	binding_flags.push_back(VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
	return *this;
}

VkDescriptorSetLayout DescriptorLayoutBuilder::create()
{
	VkDescriptorSetLayout           layout      = VK_NULL_HANDLE;
	VkDescriptorSetLayoutCreateInfo create_info = {
	    .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
	    .bindingCount = static_cast<uint32_t>(bindings.size()),
	    .pBindings    = bindings.data(),
	};
	if (bindless)
	{
		VkDescriptorSetLayoutBindingFlagsCreateInfo descriptor_set_layout_binding_flag_create_info = {
		    .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
		    .bindingCount  = static_cast<uint32_t>(binding_flags.size()),
		    .pBindingFlags = binding_flags.data(),
		};
		create_info.pNext = &descriptor_set_layout_binding_flag_create_info;
		create_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
	}
	vkCreateDescriptorSetLayout(context->vk_device, &create_info, nullptr, &layout);
	return layout;
}

DescriptorUpdateBuilder::DescriptorUpdateBuilder(const Context &context) :
    context(&context)
{
}

DescriptorUpdateBuilder &DescriptorUpdateBuilder::write_storage_images(uint32_t binding, const std::vector<VkImageView> &image_views)
{
	descriptor_index.push_back(image_infos.size());

	for (auto &image_view : image_views)
	{
		image_infos.emplace_back(
		    VkDescriptorImageInfo{
		        .sampler     = VK_NULL_HANDLE,
		        .imageView   = image_view,
		        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
		    });
	}

	write_sets.emplace_back(
	    VkWriteDescriptorSet{
	        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	        .dstSet           = VK_NULL_HANDLE,
	        .dstBinding       = binding,
	        .dstArrayElement  = 0,
	        .descriptorCount  = static_cast<uint32_t>(image_views.size()),
	        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
	        .pImageInfo       = nullptr,
	        .pBufferInfo      = nullptr,
	        .pTexelBufferView = nullptr,
	    });

	return *this;
}

DescriptorUpdateBuilder &DescriptorUpdateBuilder::write_sampled_images(uint32_t binding, const std::vector<VkImageView> &image_views)
{
	descriptor_index.push_back(image_infos.size());

	for (auto &image_view : image_views)
	{
		image_infos.emplace_back(
		    VkDescriptorImageInfo{
		        .sampler     = VK_NULL_HANDLE,
		        .imageView   = image_view,
		        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		    });
	}

	write_sets.emplace_back(
	    VkWriteDescriptorSet{
	        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	        .dstSet           = VK_NULL_HANDLE,
	        .dstBinding       = binding,
	        .dstArrayElement  = 0,
	        .descriptorCount  = static_cast<uint32_t>(image_views.size()),
	        .descriptorType   = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
	        .pImageInfo       = nullptr,
	        .pBufferInfo      = nullptr,
	        .pTexelBufferView = nullptr,
	    });

	return *this;
}

DescriptorUpdateBuilder &DescriptorUpdateBuilder::write_samplers(uint32_t binding, const std::vector<VkSampler> &samplers)
{
	descriptor_index.push_back(image_infos.size());

	for (auto &sampler : samplers)
	{
		image_infos.emplace_back(
		    VkDescriptorImageInfo{
		        .sampler     = sampler,
		        .imageView   = VK_NULL_HANDLE,
		        .imageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		    });
	}

	write_sets.emplace_back(
	    VkWriteDescriptorSet{
	        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	        .dstSet           = VK_NULL_HANDLE,
	        .dstBinding       = binding,
	        .dstArrayElement  = 0,
	        .descriptorCount  = static_cast<uint32_t>(samplers.size()),
	        .descriptorType   = VK_DESCRIPTOR_TYPE_SAMPLER,
	        .pImageInfo       = nullptr,
	        .pBufferInfo      = nullptr,
	        .pTexelBufferView = nullptr,
	    });

	return *this;
}

DescriptorUpdateBuilder &DescriptorUpdateBuilder::write_uniform_buffers(uint32_t binding, const std::vector<VkBuffer> &buffers)
{
	descriptor_index.push_back(buffer_infos.size());

	for (auto &buffer : buffers)
	{
		buffer_infos.emplace_back(
		    VkDescriptorBufferInfo{
		        .buffer = buffer,
		        .offset = 0,
		        .range  = VK_WHOLE_SIZE,
		    });
	}

	write_sets.emplace_back(
	    VkWriteDescriptorSet{
	        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	        .dstSet           = VK_NULL_HANDLE,
	        .dstBinding       = binding,
	        .dstArrayElement  = 0,
	        .descriptorCount  = static_cast<uint32_t>(buffers.size()),
	        .descriptorType   = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
	        .pImageInfo       = nullptr,
	        .pBufferInfo      = nullptr,
	        .pTexelBufferView = nullptr,
	    });

	return *this;
}

DescriptorUpdateBuilder &DescriptorUpdateBuilder::write_storage_buffers(uint32_t binding, const std::vector<VkBuffer> &buffers)
{
	descriptor_index.push_back(buffer_infos.size());

	for (auto &buffer : buffers)
	{
		buffer_infos.emplace_back(
		    VkDescriptorBufferInfo{
		        .buffer = buffer,
		        .offset = 0,
		        .range  = VK_WHOLE_SIZE,
		    });
	}

	write_sets.emplace_back(
	    VkWriteDescriptorSet{
	        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	        .dstSet           = VK_NULL_HANDLE,
	        .dstBinding       = binding,
	        .dstArrayElement  = 0,
	        .descriptorCount  = static_cast<uint32_t>(buffers.size()),
	        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
	        .pImageInfo       = nullptr,
	        .pBufferInfo      = nullptr,
	        .pTexelBufferView = nullptr,
	    });

	return *this;
}

DescriptorUpdateBuilder &DescriptorUpdateBuilder::write_acceleration_structures(uint32_t binding, const std::vector<AccelerationStructure> &as)
{
	descriptor_index.push_back(as_infos.size());

	for (auto &as_ : as)
	{
		as_infos.emplace_back(
		    VkWriteDescriptorSetAccelerationStructureKHR{
		        .sType                      = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
		        .accelerationStructureCount = 1,
		        .pAccelerationStructures    = &as_.vk_as,
		    });
	}

	write_sets.emplace_back(
	    VkWriteDescriptorSet{
	        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	        .dstSet           = VK_NULL_HANDLE,
	        .dstBinding       = binding,
	        .dstArrayElement  = 0,
	        .descriptorCount  = static_cast<uint32_t>(as.size()),
	        .descriptorType   = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
	        .pImageInfo       = nullptr,
	        .pBufferInfo      = nullptr,
	        .pTexelBufferView = nullptr,
	    });

	return *this;
}

DescriptorUpdateBuilder &DescriptorUpdateBuilder::update(VkDescriptorSet set)
{
	for (uint32_t i = 0; i < write_sets.size(); i++)
	{
		auto &write_set  = write_sets[i];
		write_set.dstSet = set;
		switch (write_set.descriptorType)
		{
			case VK_DESCRIPTOR_TYPE_SAMPLER:
			case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
			case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
			case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
				write_set.pImageInfo = image_infos.data() + descriptor_index[i];
				break;
			case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
			case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
				write_set.pBufferInfo = buffer_infos.data() + descriptor_index[i];
				break;
			case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
				write_set.pNext = as_infos.data() + descriptor_index[i];
				break;
			default:
				break;
		}
	}
	vkUpdateDescriptorSets(context->vk_device, static_cast<uint32_t>(write_sets.size()), write_sets.data(), 0, nullptr);
	return *this;
}

GraphicsPipelineBuilder::GraphicsPipelineBuilder(const Context &context, VkPipelineLayout layout) :
    context(&context), pipeline_layout(layout)
{
	depth_stencil_state = {
	    .sType             = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
	    .depthTestEnable   = false,
	    .depthWriteEnable  = false,
	    .depthCompareOp    = {},
	    .stencilTestEnable = false,
	    .front             = {},
	    .back              = {},
	};

	input_assembly_state = {
	    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
	    .flags                  = 0,
	    .topology               = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	    .primitiveRestartEnable = VK_FALSE,
	};

	multisample_state = {
	    .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
	    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	    .sampleShadingEnable  = VK_FALSE,
	};

	rasterization_state = {
	    .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
	    .flags                   = 0,
	    .depthClampEnable        = false,
	    .polygonMode             = VK_POLYGON_MODE_FILL,
	    .cullMode                = VK_CULL_MODE_NONE,
	    .frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE,
	    .depthBiasEnable         = false,
	    .depthBiasConstantFactor = 0.f,
	    .depthBiasClamp          = 0.f,
	    .depthBiasSlopeFactor    = 0.f,
	    .lineWidth               = 1.f,
	};
}

GraphicsPipelineBuilder &GraphicsPipelineBuilder::add_shader(VkShaderStageFlagBits stage, const std::string &shader_path, const std::string &entry_point, const std::unordered_map<std::string, std::string> &macros)
{
	VkShaderModule shader = context->load_slang_shader(shader_path, stage, entry_point, macros);
	add_shader(stage, shader);
	return *this;
}

GraphicsPipelineBuilder &GraphicsPipelineBuilder::add_shader(VkShaderStageFlagBits stage, const uint32_t *spirv_code, size_t size)
{
	VkShaderModule shader = context->load_spirv_shader(spirv_code, size);
	add_shader(stage, shader);
	return *this;
}

GraphicsPipelineBuilder &GraphicsPipelineBuilder::add_shader(VkShaderStageFlagBits stage, VkShaderModule shader)
{
	shader_states.emplace_back(
	    VkPipelineShaderStageCreateInfo{
	        .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	        .stage               = stage,
	        .module              = shader,
	        .pName               = "main",
	        .pSpecializationInfo = nullptr,
	    });
	return *this;
}

GraphicsPipelineBuilder &GraphicsPipelineBuilder::add_color_attachment(VkFormat format, VkPipelineColorBlendAttachmentState blend_state)
{
	color_attachments.push_back(format);
	color_blend_attachment_states.push_back(blend_state);
	return *this;
}

GraphicsPipelineBuilder &GraphicsPipelineBuilder::add_depth_stencil(VkFormat format, bool depth_test, bool depth_write, VkCompareOp compare, bool stencil_test, VkStencilOpState front, VkStencilOpState back)
{
	depth_attachment    = format;
	depth_stencil_state = {
	    .sType             = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
	    .depthTestEnable   = depth_test,
	    .depthWriteEnable  = depth_write,
	    .depthCompareOp    = compare,
	    .stencilTestEnable = stencil_test,
	    .front             = front,
	    .back              = back,
	};
	return *this;
}

GraphicsPipelineBuilder &GraphicsPipelineBuilder::add_viewport(const VkViewport &viewport)
{
	viewports.push_back(viewport);
	return *this;
}

GraphicsPipelineBuilder &GraphicsPipelineBuilder::add_scissor(const VkRect2D &scissor)
{
	scissors.push_back(scissor);
	return *this;
}

GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_input_assembly(VkPrimitiveTopology topology)
{
	input_assembly_state = {
	    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
	    .flags                  = 0,
	    .topology               = topology,
	    .primitiveRestartEnable = VK_FALSE,
	};
	return *this;
}

GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_multisample(VkSampleCountFlagBits sample_count)
{
	multisample_state = {
	    .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
	    .rasterizationSamples = sample_count,
	    .sampleShadingEnable  = VK_FALSE,
	};
	return *this;
}

GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_rasterization(VkPolygonMode polygon, VkCullModeFlags cull, VkFrontFace front_face, float line_width, float depth_bias, float depth_bias_slope, float depth_bias_clamp)
{
	rasterization_state = {
	    .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
	    .flags                   = 0,
	    .depthClampEnable        = (depth_bias_clamp != 0.f),
	    .polygonMode             = polygon,
	    .cullMode                = cull,
	    .frontFace               = front_face,
	    .depthBiasEnable         = (depth_bias != 0.f),
	    .depthBiasConstantFactor = depth_bias,
	    .depthBiasClamp          = depth_bias_clamp,
	    .depthBiasSlopeFactor    = depth_bias_slope,
	    .lineWidth               = line_width,
	};
	return *this;
}

GraphicsPipelineBuilder &GraphicsPipelineBuilder::add_vertex_input_attribute(uint32_t location, uint32_t binding, VkFormat format, uint32_t offset)
{
	vertex_input_attributes.push_back(
	    VkVertexInputAttributeDescription{
	        .location = location,
	        .binding  = binding,
	        .format   = format,
	        .offset   = offset,
	    });
	return *this;
}

GraphicsPipelineBuilder &GraphicsPipelineBuilder::add_vertex_input_binding(uint32_t binding, uint32_t stride, VkVertexInputRate input_rate)
{
	vertex_input_bindings.push_back(
	    VkVertexInputBindingDescription{
	        .binding   = binding,
	        .stride    = stride,
	        .inputRate = input_rate,
	    });
	return *this;
}

VkPipeline GraphicsPipelineBuilder::create()
{
	VkPipelineVertexInputStateCreateInfo vertex_input_state_create_info = {
	    .sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
	    .vertexBindingDescriptionCount   = static_cast<uint32_t>(vertex_input_bindings.size()),
	    .pVertexBindingDescriptions      = vertex_input_bindings.data(),
	    .vertexAttributeDescriptionCount = static_cast<uint32_t>(vertex_input_attributes.size()),
	    .pVertexAttributeDescriptions    = vertex_input_attributes.data(),
	};

	VkPipelineRenderingCreateInfo pipeline_rendering_create_info = {
	    .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
	    .colorAttachmentCount    = static_cast<uint32_t>(color_attachments.size()),
	    .pColorAttachmentFormats = color_attachments.data(),
	    .depthAttachmentFormat   = depth_attachment.has_value() ? depth_attachment.value() : VK_FORMAT_UNDEFINED,
	};

	VkPipelineViewportStateCreateInfo viewport_state_create_info = {
	    .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
	    .viewportCount = static_cast<uint32_t>(viewports.size()),
	    .pViewports    = viewports.data(),
	    .scissorCount  = static_cast<uint32_t>(scissors.size()),
	    .pScissors     = scissors.data(),
	};

	VkPipelineColorBlendStateCreateInfo color_blend_state_create_info = {
	    .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
	    .logicOpEnable   = VK_FALSE,
	    .attachmentCount = static_cast<uint32_t>(color_blend_attachment_states.size()),
	    .pAttachments    = color_blend_attachment_states.data(),
	};

	VkGraphicsPipelineCreateInfo create_info = {
	    .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
	    .pNext               = &pipeline_rendering_create_info,
	    .stageCount          = static_cast<uint32_t>(shader_states.size()),
	    .pStages             = shader_states.data(),
	    .pVertexInputState   = &vertex_input_state_create_info,
	    .pInputAssemblyState = &input_assembly_state,
	    .pTessellationState  = nullptr,
	    .pViewportState      = &viewport_state_create_info,
	    .pRasterizationState = &rasterization_state,
	    .pMultisampleState   = &multisample_state,
	    .pDepthStencilState  = &depth_stencil_state,
	    .pColorBlendState    = &color_blend_state_create_info,
	    .pDynamicState       = nullptr,
	    .layout              = pipeline_layout,
	    .renderPass          = VK_NULL_HANDLE,
	    .subpass             = 0,
	    .basePipelineHandle  = VK_NULL_HANDLE,
	    .basePipelineIndex   = -1,
	};

	VkPipeline pipeline = VK_NULL_HANDLE;
	auto       result   = vkCreateGraphicsPipelines(context->vk_device, context->vk_pipeline_cache, 1, &create_info, nullptr, &pipeline);

	for (auto &shader_state : shader_states)
	{
		vkDestroyShaderModule(context->vk_device, shader_state.module, nullptr);
	}

	return pipeline;
}

Context::Context(uint32_t width, uint32_t height, float upscale_factor)
{
	// Init window
	{
		if (!glfwInit())
		{
			return;
		}

		auto *video_mode = glfwGetVideoMode(glfwGetPrimaryMonitor());

		extent.width  = width == 0 ? static_cast<uint32_t>(video_mode->width * 3 / 4) : width;
		extent.height = height == 0 ? static_cast<uint32_t>(video_mode->height * 3 / 4) : height;
		if (width == 0 || height == 0)
		{
			extent.width  = static_cast<uint32_t>(video_mode->width * 3 / 4);
			extent.height = static_cast<uint32_t>(video_mode->height * 3 / 4);
		}
		else
		{
			extent.width  = width;
			extent.height = height;
		}

		render_extent = VkExtent2D{
		    .width  = (uint32_t) ((float) extent.width * upscale_factor),
		    .height = (uint32_t) ((float) extent.height * upscale_factor),
		};

		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
		glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

		window = glfwCreateWindow(extent.width, extent.height, "Hair Renderer", NULL, NULL);
		if (!window)
		{
			glfwTerminate();
			return;
		}

		glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
	}

	// Init vulkan instance
	{
		// Initialize volk context
		volkInitialize();

		// Config application info
		uint32_t api_version = 0;

		PFN_vkEnumerateInstanceVersion enumerate_instance_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));

		if (enumerate_instance_version)
		{
			enumerate_instance_version(&api_version);
		}
		else
		{
			api_version = VK_VERSION_1_0;
		}

		VkApplicationInfo app_info{
		    .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		    .pApplicationName   = "RayTracer",
		    .applicationVersion = VK_MAKE_VERSION(0, 0, 1),
		    .pEngineName        = "RayTracer",
		    .engineVersion      = VK_MAKE_VERSION(0, 0, 1),
		    .apiVersion         = api_version,
		};

		std::vector<const char *> instance_extensions = get_instance_extension_supported({
#ifdef DEBUG
		    "VK_KHR_surface", "VK_KHR_win32_surface", "VK_EXT_debug_report", "VK_EXT_debug_utils"
#else
		    "VK_KHR_surface", "VK_KHR_win32_surface"
#endif
		});
		VkInstanceCreateInfo create_info{
		    .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		    .pApplicationInfo        = &app_info,
		    .enabledLayerCount       = 0,
		    .enabledExtensionCount   = static_cast<uint32_t>(instance_extensions.size()),
		    .ppEnabledExtensionNames = instance_extensions.data(),
		};

		// Enable validation layers
#ifdef DEBUG
		const std::vector<VkValidationFeatureEnableEXT> validation_extensions =
#	ifdef DEBUG
		    {VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
#	else
		    {};
#	endif        // DEBUG
		VkValidationFeaturesEXT validation_features{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
		validation_features.enabledValidationFeatureCount = static_cast<uint32_t>(validation_extensions.size());
		validation_features.pEnabledValidationFeatures    = validation_extensions.data();

		uint32_t layer_count;
		vkEnumerateInstanceLayerProperties(&layer_count, nullptr);

		std::vector<VkLayerProperties> layers(layer_count);
		vkEnumerateInstanceLayerProperties(&layer_count, layers.data());

		std::vector<const char *> validation_layers = {"VK_LAYER_KHRONOS_validation"};
		for (auto &layer : validation_layers)
		{
			if (check_layer_supported(layer))
			{
				create_info.enabledLayerCount   = static_cast<uint32_t>(validation_layers.size());
				create_info.ppEnabledLayerNames = validation_layers.data();
				create_info.pNext               = &validation_features;
				break;
			}
			else
			{
				spdlog::error("Validation layer was required, but not avaliable, disabling debugging");
			}
		}
#endif        // DEBUG

		// Create instance
		if (vkCreateInstance(&create_info, nullptr, &vk_instance) != VK_SUCCESS)
		{
			spdlog::error("Failed to create vulkan instance!");
			return;
		}
		else
		{
			// Config to volk
			volkLoadInstance(vk_instance);
		}

		// Initialize instance extension functions
		static PFN_vkCreateDebugUtilsMessengerEXT  vkCreateDebugUtilsMessengerEXT  = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(vk_instance, "vkCreateDebugUtilsMessengerEXT"));
		static PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(vk_instance, "vkDestroyDebugUtilsMessengerEXT"));

		// Enable debugger
#ifdef DEBUG
		if (vkCreateDebugUtilsMessengerEXT)
		{
			VkDebugUtilsMessengerCreateInfoEXT create_info{
			    .sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
			    .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
			    .messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
			    .pfnUserCallback = validation_callback,
			};

			vkCreateDebugUtilsMessengerEXT(vk_instance, &create_info, nullptr, &vkDebugUtilsMessengerEXT);
		}
#endif        // DEBUG
	}

	// Init vulkan device
	{
		const std::vector<const char *> device_extensions = {
		    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
		    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
		    VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
		    VK_KHR_RAY_QUERY_EXTENSION_NAME,
		    VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
		    VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
		    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
		    VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME,
		    VK_KHR_SPIRV_1_4_EXTENSION_NAME,
		    VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME,
		    VK_EXT_MESH_SHADER_EXTENSION_NAME,
		};

		// Init vulkan physical device
		{
			uint32_t physical_device_count = 0;
			vkEnumeratePhysicalDevices(vk_instance, &physical_device_count, nullptr);

			// Get all physical devices
			std::vector<VkPhysicalDevice> physical_devices(physical_device_count);
			vkEnumeratePhysicalDevices(vk_instance, &physical_device_count, physical_devices.data());

			// Select suitable physical device
			vk_physical_device = select_physical_device(physical_devices, device_extensions);

			vkGetPhysicalDeviceProperties(vk_physical_device, &physical_device_properties);
		}

		// Init vulkan logical device
		{
			uint32_t queue_family_property_count = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(vk_physical_device, &queue_family_property_count, nullptr);
			std::vector<VkQueueFamilyProperties> queue_family_properties(queue_family_property_count);
			vkGetPhysicalDeviceQueueFamilyProperties(vk_physical_device, &queue_family_property_count, queue_family_properties.data());

			graphics_family = get_queue_family_index(queue_family_properties, VK_QUEUE_GRAPHICS_BIT);
			transfer_family = get_queue_family_index(queue_family_properties, VK_QUEUE_TRANSFER_BIT);
			compute_family  = get_queue_family_index(queue_family_properties, VK_QUEUE_COMPUTE_BIT);

			VkQueueFlags support_queues = 0;

			if (graphics_family.has_value())
			{
				graphics_family = graphics_family.value();
				support_queues |= VK_QUEUE_GRAPHICS_BIT;
			}

			if (compute_family.has_value())
			{
				compute_family = compute_family.value();
				support_queues |= VK_QUEUE_COMPUTE_BIT;
			}

			if (transfer_family.has_value())
			{
				transfer_family = transfer_family.value();
				support_queues |= VK_QUEUE_TRANSFER_BIT;
			}

			if (!graphics_family)
			{
				throw std::runtime_error("Failed to find queue graphics family support!");
			}

			// Create device queue
			std::vector<VkDeviceQueueCreateInfo> queue_create_infos;

			uint32_t max_count = 0;
			for (auto &queue_family_property : queue_family_properties)
			{
				max_count = max_count < queue_family_property.queueCount ? queue_family_property.queueCount : max_count;
			}

			std::vector<float> queue_priorities(max_count, 1.f);

			if (support_queues & VK_QUEUE_GRAPHICS_BIT)
			{
				VkDeviceQueueCreateInfo graphics_queue_create_info = {
				    .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
				    .queueFamilyIndex = graphics_family.value(),
				    .queueCount       = queue_family_properties[graphics_family.value()].queueCount,
				    .pQueuePriorities = queue_priorities.data(),
				};
				queue_create_infos.emplace_back(graphics_queue_create_info);
			}
			else
			{
				graphics_family = 0;
			}

			if (support_queues & VK_QUEUE_COMPUTE_BIT && compute_family != graphics_family)
			{
				VkDeviceQueueCreateInfo compute_queue_create_info = {
				    .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
				    .queueFamilyIndex = compute_family.value(),
				    .queueCount       = queue_family_properties[compute_family.value()].queueCount,
				    .pQueuePriorities = queue_priorities.data(),
				};
				queue_create_infos.emplace_back(compute_queue_create_info);
			}
			else
			{
				compute_family = graphics_family;
			}

			if (support_queues & VK_QUEUE_TRANSFER_BIT && transfer_family != graphics_family && transfer_family != compute_family)
			{
				VkDeviceQueueCreateInfo transfer_queue_create_info = {
				    .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
				    .queueFamilyIndex = transfer_family.value(),
				    .queueCount       = queue_family_properties[transfer_family.value()].queueCount,
				    .pQueuePriorities = queue_priorities.data(),
				};
				queue_create_infos.emplace_back(transfer_queue_create_info);
			}
			else
			{
				transfer_family = graphics_family;
			}

			VkPhysicalDeviceFeatures2        physical_device_features          = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
			VkPhysicalDeviceVulkan12Features physical_device_vulkan12_features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
			VkPhysicalDeviceVulkan13Features physical_device_vulkan13_features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};

			physical_device_features.pNext          = &physical_device_vulkan12_features;
			physical_device_vulkan12_features.pNext = &physical_device_vulkan13_features;

			vkGetPhysicalDeviceFeatures2(vk_physical_device, &physical_device_features);

			VkPhysicalDeviceFeatures2        physical_device_features_enable          = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
			VkPhysicalDeviceVulkan12Features physical_device_vulkan12_features_enable = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
			VkPhysicalDeviceVulkan13Features physical_device_vulkan13_features_enable = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};

#define ENABLE_DEVICE_FEATURE(device_feature, device_feature_enable, feature) \
	if (device_feature.feature)                                               \
	{                                                                         \
		device_feature_enable.feature = VK_TRUE;                              \
	}                                                                         \
	else                                                                      \
	{                                                                         \
		spdlog::warn("Device feature {} is not supported", #feature);         \
	}

			ENABLE_DEVICE_FEATURE(physical_device_features.features, physical_device_features_enable.features, multiViewport);
			ENABLE_DEVICE_FEATURE(physical_device_features.features, physical_device_features_enable.features, shaderInt64);
			ENABLE_DEVICE_FEATURE(physical_device_vulkan12_features, physical_device_vulkan12_features_enable, descriptorIndexing);
			ENABLE_DEVICE_FEATURE(physical_device_vulkan12_features, physical_device_vulkan12_features_enable, bufferDeviceAddress);
			ENABLE_DEVICE_FEATURE(physical_device_vulkan12_features, physical_device_vulkan12_features_enable, runtimeDescriptorArray);
			ENABLE_DEVICE_FEATURE(physical_device_vulkan12_features, physical_device_vulkan12_features_enable, descriptorBindingSampledImageUpdateAfterBind);
			ENABLE_DEVICE_FEATURE(physical_device_vulkan12_features, physical_device_vulkan12_features_enable, descriptorBindingStorageBufferUpdateAfterBind);
			ENABLE_DEVICE_FEATURE(physical_device_vulkan12_features, physical_device_vulkan12_features_enable, descriptorBindingPartiallyBound);
			ENABLE_DEVICE_FEATURE(physical_device_vulkan12_features, physical_device_vulkan12_features_enable, shaderOutputViewportIndex);
			ENABLE_DEVICE_FEATURE(physical_device_vulkan12_features, physical_device_vulkan12_features_enable, shaderOutputLayer);
			ENABLE_DEVICE_FEATURE(physical_device_vulkan13_features, physical_device_vulkan13_features_enable, dynamicRendering);
			ENABLE_DEVICE_FEATURE(physical_device_vulkan13_features, physical_device_vulkan13_features_enable, maintenance4);

			auto support_extensions = get_device_extension_support(vk_physical_device, device_extensions);

			VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration_structure_feature = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
			VkPhysicalDeviceRayTracingPipelineFeaturesKHR    ray_tracing_pipeline_feature   = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
			VkPhysicalDeviceRayQueryFeaturesKHR              ray_query_features             = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
			VkPhysicalDeviceMeshShaderFeaturesEXT            mesh_shader_feature            = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};

			acceleration_structure_feature.accelerationStructure = VK_TRUE;
			ray_tracing_pipeline_feature.rayTracingPipeline      = VK_TRUE;
			ray_query_features.rayQuery                          = VK_TRUE;
			mesh_shader_feature.meshShader                       = VK_TRUE;
			mesh_shader_feature.taskShader                       = VK_TRUE;
			mesh_shader_feature.multiviewMeshShader              = VK_TRUE;

			physical_device_vulkan12_features_enable.pNext = &physical_device_vulkan13_features_enable;
			physical_device_vulkan13_features_enable.pNext = &acceleration_structure_feature;
			acceleration_structure_feature.pNext           = &ray_tracing_pipeline_feature;
			ray_tracing_pipeline_feature.pNext             = &ray_query_features;
			ray_query_features.pNext                       = &mesh_shader_feature;

#ifdef DEBUG
			std::vector<const char *> validation_layers = {"VK_LAYER_KHRONOS_validation"};
#endif

			VkDeviceCreateInfo device_create_info = {
			    .sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
			    .pNext                = &physical_device_vulkan12_features_enable,
			    .queueCreateInfoCount = static_cast<uint32_t>(queue_create_infos.size()),
			    .pQueueCreateInfos    = queue_create_infos.data(),
#ifdef DEBUG
			    .enabledLayerCount   = static_cast<uint32_t>(validation_layers.size()),
			    .ppEnabledLayerNames = validation_layers.data(),
#endif
			    .enabledExtensionCount   = static_cast<uint32_t>(support_extensions.size()),
			    .ppEnabledExtensionNames = support_extensions.data(),
			    .pEnabledFeatures        = &physical_device_features.features,
			};

			if (vkCreateDevice(vk_physical_device, &device_create_info, nullptr, &vk_device) != VK_SUCCESS)
			{
				spdlog::error("Failed to create logical device!");
				return;
			}

			// Volk load context
			volkLoadDevice(vk_device);

			vkGetDeviceQueue(vk_device, graphics_family.value(), 0, &graphics_queue);
			vkGetDeviceQueue(vk_device, compute_family.value(), 0, &compute_queue);
			vkGetDeviceQueue(vk_device, transfer_family.value(), 0, &transfer_queue);
		}
	}

	// Init vma
	{
		// Create Vma allocator
		VmaVulkanFunctions vma_vulkan_func{
		    .vkGetInstanceProcAddr               = vkGetInstanceProcAddr,
		    .vkGetDeviceProcAddr                 = vkGetDeviceProcAddr,
		    .vkGetPhysicalDeviceProperties       = vkGetPhysicalDeviceProperties,
		    .vkGetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties,
		    .vkAllocateMemory                    = vkAllocateMemory,
		    .vkFreeMemory                        = vkFreeMemory,
		    .vkMapMemory                         = vkMapMemory,
		    .vkUnmapMemory                       = vkUnmapMemory,
		    .vkFlushMappedMemoryRanges           = vkFlushMappedMemoryRanges,
		    .vkInvalidateMappedMemoryRanges      = vkInvalidateMappedMemoryRanges,
		    .vkBindBufferMemory                  = vkBindBufferMemory,
		    .vkBindImageMemory                   = vkBindImageMemory,
		    .vkGetBufferMemoryRequirements       = vkGetBufferMemoryRequirements,
		    .vkGetImageMemoryRequirements        = vkGetImageMemoryRequirements,
		    .vkCreateBuffer                      = vkCreateBuffer,
		    .vkDestroyBuffer                     = vkDestroyBuffer,
		    .vkCreateImage                       = vkCreateImage,
		    .vkDestroyImage                      = vkDestroyImage,
		    .vkCmdCopyBuffer                     = vkCmdCopyBuffer,
		};

		VmaAllocatorCreateInfo allocator_info = {
		    .flags            = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
		    .physicalDevice   = vk_physical_device,
		    .device           = vk_device,
		    .pVulkanFunctions = &vma_vulkan_func,
		    .instance         = vk_instance,
		    .vulkanApiVersion = VK_API_VERSION_1_3,
		};

		if (vmaCreateAllocator(&allocator_info, &vma_allocator) != VK_SUCCESS)
		{
			spdlog::critical("Failed to create vulkan memory allocator");
			return;
		}
	}

	// Init vulkan swapchain
	{
#ifdef _WIN32
		{
			VkWin32SurfaceCreateInfoKHR createInfo{
			    .sType     = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
			    .hinstance = GetModuleHandle(nullptr),
			    .hwnd      = glfwGetWin32Window(window),
			};
			vkCreateWin32SurfaceKHR(vk_instance, &createInfo, nullptr, &vk_surface);
		}
#endif        // _WIN32

		VkSurfaceCapabilitiesKHR capabilities;
		vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk_physical_device, vk_surface, &capabilities);

		uint32_t                        format_count;
		std::vector<VkSurfaceFormatKHR> formats;
		vkGetPhysicalDeviceSurfaceFormatsKHR(vk_physical_device, vk_surface, &format_count, nullptr);
		if (format_count != 0)
		{
			formats.resize(format_count);
			vkGetPhysicalDeviceSurfaceFormatsKHR(vk_physical_device, vk_surface, &format_count, formats.data());
		}

		VkSurfaceFormatKHR surface_format = {};
		for (const auto &format : formats)
		{
			if (format.format == VK_FORMAT_B8G8R8A8_UNORM &&
			    format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
			{
				surface_format = format;
			}
		}
		if (surface_format.format == VK_FORMAT_UNDEFINED)
		{
			surface_format = formats[0];
		}
		vk_format = surface_format.format;

		if (capabilities.currentExtent.width != UINT32_MAX)
		{
			extent = capabilities.currentExtent;
		}
		else
		{
			VkExtent2D actualExtent = extent;

			actualExtent.width  = std::clamp(actualExtent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
			actualExtent.height = std::clamp(actualExtent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);

			extent = actualExtent;
		}

		assert(capabilities.maxImageCount >= 3);

		uint32_t queue_family_property_count = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(vk_physical_device, &queue_family_property_count, nullptr);
		std::vector<VkQueueFamilyProperties> queue_family_properties(queue_family_property_count);
		vkGetPhysicalDeviceQueueFamilyProperties(vk_physical_device, &queue_family_property_count, queue_family_properties.data());

		for (uint32_t i = 0; i < queue_family_property_count; i++)
		{
			// Check for presentation support
			VkBool32 present_support;
			vkGetPhysicalDeviceSurfaceSupportKHR(vk_physical_device, i, vk_surface, &present_support);

			if (queue_family_properties[i].queueCount > 0 && present_support)
			{
				present_family = i;
				break;
			}
		}

		vkGetDeviceQueue(vk_device, present_family.value(), 0, &present_queue);

		VkSwapchainCreateInfoKHR createInfo = {};
		createInfo.sType                    = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
		createInfo.surface                  = vk_surface;

		createInfo.minImageCount    = 3;
		createInfo.imageFormat      = surface_format.format;
		createInfo.imageColorSpace  = surface_format.colorSpace;
		createInfo.imageExtent      = extent;
		createInfo.imageArrayLayers = 1;
		createInfo.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

		uint32_t queueFamilyIndices[] = {present_family.value()};

		createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		createInfo.preTransform     = capabilities.currentTransform;
		createInfo.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		createInfo.presentMode      = VK_PRESENT_MODE_FIFO_KHR;
		createInfo.clipped          = VK_TRUE;

		vkCreateSwapchainKHR(vk_device, &createInfo, nullptr, &vk_swapchain);

		uint32_t image_count = 3;
		vkGetSwapchainImagesKHR(vk_device, vk_swapchain, &image_count, swapchain_images.data());

		// Create image view
		for (size_t i = 0; i < 3; i++)
		{
			set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) swapchain_images[i], fmt::format("Swapchain Image {}", i).c_str());
			swapchain_image_views[i] = create_texture_view(fmt::format("Swapchain Image View {}", i), swapchain_images[i], vk_format);
		}
	}

	// init vulkan resource
	{
		{
			VkCommandPoolCreateInfo create_info = {
			    .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			    .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
			    .queueFamilyIndex = graphics_family.value(),
			};
			vkCreateCommandPool(vk_device, &create_info, nullptr, &graphics_cmd_pool);
		}

		{
			VkCommandPoolCreateInfo create_info = {
			    .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			    .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
			    .queueFamilyIndex = compute_family.value(),
			};
			vkCreateCommandPool(vk_device, &create_info, nullptr, &compute_cmd_pool);
		}

		{
			VkPipelineCacheCreateInfo create_info = {
			    .sType           = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
			    .initialDataSize = 0,
			    .pInitialData    = nullptr,
			};
			vkCreatePipelineCache(vk_device, &create_info, nullptr, &vk_pipeline_cache);
		}

		{
			std::vector<VkDescriptorPoolSize> pool_sizes =
			    {
			        {VK_DESCRIPTOR_TYPE_SAMPLER, 1000},
			        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
			        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000},
			        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000},
			        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000},
			        {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000},
			        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000},
			        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000},
			        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000},
			        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000},
			        {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000},
			    };
			VkDescriptorPoolCreateInfo pool_info = {
			    .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			    .flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT | VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
			    .maxSets       = 1000 * static_cast<uint32_t>(pool_sizes.size()),
			    .poolSizeCount = (uint32_t) static_cast<uint32_t>(pool_sizes.size()),
			    .pPoolSizes    = pool_sizes.data(),
			};
			vkCreateDescriptorPool(vk_device, &pool_info, nullptr, &vk_descriptor_pool);
		}
	}

	// Create default sampler
	VkSamplerCreateInfo sampler_create_info = {
	    .sType            = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
	    .magFilter        = VK_FILTER_LINEAR,
	    .minFilter        = VK_FILTER_LINEAR,
	    .mipmapMode       = VK_SAMPLER_MIPMAP_MODE_LINEAR,
	    .addressModeU     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	    .addressModeV     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	    .addressModeW     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	    .mipLodBias       = 0.f,
	    .anisotropyEnable = VK_FALSE,
	    .maxAnisotropy    = 1.f,
	    .compareEnable    = VK_FALSE,
	    .compareOp        = VK_COMPARE_OP_NEVER,
	    .minLod           = 0.f,
	    .maxLod           = 12.f,
	    .borderColor      = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
	};
	vkCreateSampler(vk_device, &sampler_create_info, nullptr, &default_sampler);
}

Context::~Context()
{
	vkDeviceWaitIdle(vk_device);

	// Destroy window
	glfwDestroyWindow(window);
	glfwTerminate();

	vkDestroySampler(vk_device, default_sampler, nullptr);

	for (auto &view : swapchain_image_views)
	{
		vkDestroyImageView(vk_device, view, nullptr);
	}

	vkDestroyDescriptorPool(vk_device, vk_descriptor_pool, nullptr);
	vkDestroyPipelineCache(vk_device, vk_pipeline_cache, nullptr);

	vkDestroyCommandPool(vk_device, graphics_cmd_pool, nullptr);
	vkDestroyCommandPool(vk_device, compute_cmd_pool, nullptr);

	vkDestroySwapchainKHR(vk_device, vk_swapchain, nullptr);
	vkDestroySurfaceKHR(vk_instance, vk_surface, nullptr);
	vmaDestroyAllocator(vma_allocator);
	vkDestroyDevice(vk_device, nullptr);
#ifdef DEBUG
	vkDestroyDebugUtilsMessengerEXT(vk_instance, vkDebugUtilsMessengerEXT, nullptr);
#endif        // DEBUG
	vkDestroyInstance(vk_instance, nullptr);
}

CommandBufferRecorder Context::record_command(bool compute) const
{
	return CommandBufferRecorder(*this, compute);
}

VkSemaphore Context::create_semaphore(const std::string &name) const
{
	VkSemaphoreCreateInfo create_info = {
	    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
	    .flags = 0,
	};
	VkSemaphore semaphore = VK_NULL_HANDLE;
	vkCreateSemaphore(vk_device, &create_info, nullptr, &semaphore);
	set_object_name(VK_OBJECT_TYPE_SEMAPHORE, (uint64_t) semaphore, name.c_str());
	return semaphore;
}

VkFence Context::create_fence(const std::string &name) const
{
	VkFenceCreateInfo create_info = {
	    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
	    .flags = VK_FENCE_CREATE_SIGNALED_BIT,
	};
	VkFence fence = VK_NULL_HANDLE;
	vkCreateFence(vk_device, &create_info, nullptr, &fence);
	set_object_name(VK_OBJECT_TYPE_FENCE, (uint64_t) fence, name.c_str());
	return fence;
}

Buffer Context::create_buffer(const std::string &name, size_t size, VkBufferUsageFlags buffer_usage, VmaMemoryUsage memory_usage) const
{
	Buffer buffer;

	VkBufferCreateInfo buffer_create_info = {
	    .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
	    .size        = size,
	    .usage       = buffer_usage,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	VmaAllocationCreateInfo allocation_create_info = {
	    .usage = memory_usage,
	};
	VmaAllocationInfo allocation_info = {};
	vmaCreateBuffer(vma_allocator, &buffer_create_info, &allocation_create_info, &buffer.vk_buffer, &buffer.vma_allocation, &allocation_info);
	if (buffer_usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
	{
		VkBufferDeviceAddressInfoKHR buffer_device_address_info = {
		    .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
		    .buffer = buffer.vk_buffer,
		};
		buffer.device_address = vkGetBufferDeviceAddress(vk_device, &buffer_device_address_info);
	}
	set_object_name(VK_OBJECT_TYPE_BUFFER, (uint64_t) buffer.vk_buffer, name.c_str());
	return buffer;
}

std::pair<AccelerationStructure, Buffer> Context::create_acceleration_structure(const std::string &name, VkAccelerationStructureTypeKHR type, const VkAccelerationStructureGeometryKHR &geometry, const VkAccelerationStructureBuildRangeInfoKHR &range) const
{
	VkAccelerationStructureBuildGeometryInfoKHR build_geometry_info = {
	    .sType                    = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
	    .type                     = type,
	    .flags                    = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
	    .mode                     = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
	    .srcAccelerationStructure = VK_NULL_HANDLE,
	    .geometryCount            = 1,
	    .pGeometries              = &geometry,
	    .ppGeometries             = nullptr,
	    .scratchData              = {
	                     .deviceAddress = 0,
        },
	};

	VkAccelerationStructureBuildSizesInfoKHR build_sizes_info = {
	    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
	};

	vkGetAccelerationStructureBuildSizesKHR(
	    vk_device,
	    VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
	    &build_geometry_info,
	    &range.primitiveCount,
	    &build_sizes_info);

	// Create acceleration structure
	AccelerationStructure acceleration_structure = {};
	{
		VkBufferCreateInfo buffer_create_info = {
		    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		    .size  = build_sizes_info.accelerationStructureSize,
		    .usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		};
		VmaAllocationCreateInfo allocation_create_info = {
		    .usage = VMA_MEMORY_USAGE_GPU_ONLY,
		};
		VmaAllocationInfo allocation_info = {};
		vmaCreateBuffer(vma_allocator, &buffer_create_info, &allocation_create_info, &acceleration_structure.buffer.vk_buffer, &acceleration_structure.buffer.vma_allocation, &allocation_info);
		VkAccelerationStructureCreateInfoKHR as_create_info = {
		    .sType  = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
		    .buffer = acceleration_structure.buffer.vk_buffer,
		    .size   = build_sizes_info.accelerationStructureSize,
		    .type   = type,
		};
		vkCreateAccelerationStructureKHR(vk_device, &as_create_info, nullptr, &acceleration_structure.vk_as);
		VkAccelerationStructureDeviceAddressInfoKHR as_device_address_info = {
		    .sType                 = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
		    .accelerationStructure = acceleration_structure.vk_as,
		};
		acceleration_structure.device_address = vkGetAccelerationStructureDeviceAddressKHR(vk_device, &as_device_address_info);
	}

	Buffer scratch_buffer = create_scratch_buffer(build_sizes_info.buildScratchSize);

	build_geometry_info.scratchData.deviceAddress = scratch_buffer.device_address;
	build_geometry_info.dstAccelerationStructure  = acceleration_structure.vk_as;

	VkAccelerationStructureBuildRangeInfoKHR *as_build_range_infos = const_cast<VkAccelerationStructureBuildRangeInfoKHR *>(&range);

	record_command(true)
	    .begin()
	    .build_acceleration_structure(build_geometry_info, as_build_range_infos)
	    .end()
	    .flush();

	set_object_name(VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, (uint64_t) acceleration_structure.vk_as, name.c_str());
	return {acceleration_structure, scratch_buffer};
}

void Context::buffer_copy_to_device(const Buffer &buffer, void *data, size_t size, bool staging, size_t offset) const
{
	if (staging)
	{
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
			vmaCreateBuffer(vma_allocator, &buffer_create_info, &allocation_create_info, &staging_buffer.vk_buffer, &staging_buffer.vma_allocation, &allocation_info);
		}

		if (data)
		{
			uint8_t *mapped_data = nullptr;
			vmaMapMemory(vma_allocator, staging_buffer.vma_allocation, reinterpret_cast<void **>(&mapped_data));
			std::memcpy(mapped_data, data, size);
			vmaUnmapMemory(vma_allocator, staging_buffer.vma_allocation);
			vmaFlushAllocation(vma_allocator, staging_buffer.vma_allocation, 0, size);
			mapped_data = nullptr;
		}

		// Allocate command buffer
		VkCommandBuffer cmd_buffer = VK_NULL_HANDLE;
		{
			VkCommandBufferAllocateInfo allocate_info =
			    {
			        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			        .commandPool        = graphics_cmd_pool,
			        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			        .commandBufferCount = 1,
			    };
			vkAllocateCommandBuffers(vk_device, &allocate_info, &cmd_buffer);
		}

		// Create fence
		VkFence fence = VK_NULL_HANDLE;
		{
			VkFenceCreateInfo create_info = {
			    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
			    .flags = 0,
			};
			vkCreateFence(vk_device, &create_info, nullptr, &fence);
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
			    .dstOffset = offset,
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
			vkQueueSubmit(graphics_queue, 1, &submit_info, fence);
		}

		// Wait
		vkWaitForFences(vk_device, 1, &fence, VK_TRUE, UINT64_MAX);
		vkResetFences(vk_device, 1, &fence);

		// Release resource
		vkDestroyFence(vk_device, fence, nullptr);
		vkFreeCommandBuffers(vk_device, graphics_cmd_pool, 1, &cmd_buffer);
		vmaDestroyBuffer(vma_allocator, staging_buffer.vk_buffer, staging_buffer.vma_allocation);
	}
	else
	{
		uint8_t *mapped_data = nullptr;
		vmaMapMemory(vma_allocator, buffer.vma_allocation, reinterpret_cast<void **>(&mapped_data));
		std::memcpy(mapped_data, data, size);
		vmaUnmapMemory(vma_allocator, buffer.vma_allocation);
		vmaFlushAllocation(vma_allocator, buffer.vma_allocation, 0, size);
		mapped_data = nullptr;
	}
}

void Context::buffer_copy_to_host(void *data, size_t size, const Buffer &buffer, bool staging) const
{
	if (staging)
	{
		Buffer staging_buffer;
		{
			VkBufferCreateInfo buffer_create_info = {
			    .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			    .size        = size,
			    .usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
			};
			VmaAllocationCreateInfo allocation_create_info = {
			    .usage = VMA_MEMORY_USAGE_GPU_TO_CPU};
			VmaAllocationInfo allocation_info = {};
			vmaCreateBuffer(vma_allocator, &buffer_create_info, &allocation_create_info, &staging_buffer.vk_buffer, &staging_buffer.vma_allocation, &allocation_info);
		}

		// Allocate command buffer
		VkCommandBuffer cmd_buffer = VK_NULL_HANDLE;
		{
			VkCommandBufferAllocateInfo allocate_info =
			    {
			        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			        .commandPool        = graphics_cmd_pool,
			        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			        .commandBufferCount = 1,
			    };
			vkAllocateCommandBuffers(vk_device, &allocate_info, &cmd_buffer);
		}

		// Create fence
		VkFence fence = VK_NULL_HANDLE;
		{
			VkFenceCreateInfo create_info = {
			    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
			    .flags = 0,
			};
			vkCreateFence(vk_device, &create_info, nullptr, &fence);
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
			vkCmdCopyBuffer(cmd_buffer, buffer.vk_buffer, staging_buffer.vk_buffer, 1, &copy_info);
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
			vkQueueSubmit(graphics_queue, 1, &submit_info, fence);
		}

		// Wait
		vkWaitForFences(vk_device, 1, &fence, VK_TRUE, UINT64_MAX);
		vkResetFences(vk_device, 1, &fence);

		// Release resource
		vkDestroyFence(vk_device, fence, nullptr);
		vkFreeCommandBuffers(vk_device, graphics_cmd_pool, 1, &cmd_buffer);

		if (data)
		{
			uint8_t *mapped_data = nullptr;
			vmaMapMemory(vma_allocator, staging_buffer.vma_allocation, reinterpret_cast<void **>(&mapped_data));
			std::memcpy(data, mapped_data, size);
			vmaUnmapMemory(vma_allocator, staging_buffer.vma_allocation);
			vmaFlushAllocation(vma_allocator, staging_buffer.vma_allocation, 0, size);
			mapped_data = nullptr;
		}

		vmaDestroyBuffer(vma_allocator, staging_buffer.vk_buffer, staging_buffer.vma_allocation);
	}
	else
	{
		uint8_t *mapped_data = nullptr;
		vmaMapMemory(vma_allocator, buffer.vma_allocation, reinterpret_cast<void **>(&mapped_data));
		std::memcpy(data, mapped_data, size);
		vmaUnmapMemory(vma_allocator, buffer.vma_allocation);
		vmaFlushAllocation(vma_allocator, buffer.vma_allocation, 0, size);
		mapped_data = nullptr;
	}
}

Texture Context::load_texture_2d(const std::string &filename, bool mipmap) const
{
	uint8_t *raw_data = nullptr;
	size_t   raw_size = 0;
	int32_t  width = 0, height = 0, channel = 0, req_channel = 4;
	raw_data = stbi_load(filename.c_str(), &width, &height, &channel, req_channel);

	raw_size = static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(req_channel) * sizeof(uint8_t);

	uint32_t mip_level = mipmap ? static_cast<uint32_t>(std::floor(std::log2(std::max(width, height))) + 1) : 1;

	Texture image          = create_texture_2d(filename, (uint32_t) width, (uint32_t) height, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, mipmap);
	Buffer  staging_buffer = create_buffer("Image Staging Buffer", raw_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	buffer_copy_to_device(staging_buffer, raw_data, raw_size);

	record_command()
	    .begin()
	    .insert_barrier()
	    .add_image_barrier(
	        image.vk_image,
	        0, VK_ACCESS_TRANSFER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	        {
	            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	            .baseMipLevel   = 0,
	            .levelCount     = mip_level,
	            .baseArrayLayer = 0,
	            .layerCount     = 1,
	        })
	    .insert()
	    .copy_buffer_to_image(staging_buffer.vk_buffer, image.vk_image, {(uint32_t) width, (uint32_t) height, 1})
	    .insert_barrier()
	    .add_image_barrier(
	        image.vk_image,
	        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
	        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	        {
	            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	            .baseMipLevel   = 0,
	            .levelCount     = mip_level,
	            .baseArrayLayer = 0,
	            .layerCount     = 1,
	        })
	    .insert()
	    .generate_mipmap(image.vk_image, (uint32_t) width, (uint32_t) height, mip_level)
	    .insert_barrier()
	    .add_image_barrier(
	        image.vk_image,
	        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        {
	            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	            .baseMipLevel   = 0,
	            .levelCount     = mip_level,
	            .baseArrayLayer = 0,
	            .layerCount     = 1,
	        })
	    .insert()
	    .end()
	    .flush();

	vmaDestroyBuffer(vma_allocator, staging_buffer.vk_buffer, staging_buffer.vma_allocation);

	return image;
}

Texture Context::create_texture_2d(const std::string &name, uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage, bool mipmap) const
{
	Texture texture;

	VkImageCreateInfo image_create_info = {
	    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .imageType     = VK_IMAGE_TYPE_2D,
	    .format        = format,
	    .extent        = VkExtent3D{width, height, 1},
	    .mipLevels     = mipmap ? static_cast<uint32_t>(std::floor(std::log2(std::max(width, height))) + 1) : 1,
	    .arrayLayers   = 1,
	    .samples       = VK_SAMPLE_COUNT_1_BIT,
	    .tiling        = VK_IMAGE_TILING_OPTIMAL,
	    .usage         = usage,
	    .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VmaAllocationCreateInfo allocation_create_info = {
	    .usage = VMA_MEMORY_USAGE_GPU_ONLY,
	};
	vmaCreateImage(vma_allocator, &image_create_info, &allocation_create_info, &texture.vk_image, &texture.vma_allocation, nullptr);
	set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) texture.vk_image, name.c_str());
	return texture;
}

Texture Context::create_texture_2d_array(const std::string &name, uint32_t width, uint32_t height, uint32_t layer, VkFormat format, VkImageUsageFlags usage) const
{
	Texture texture;

	VkImageCreateInfo image_create_info = {
	    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .imageType     = VK_IMAGE_TYPE_2D,
	    .format        = format,
	    .extent        = VkExtent3D{width, height, 1},
	    .mipLevels     = 1,
	    .arrayLayers   = layer,
	    .samples       = VK_SAMPLE_COUNT_1_BIT,
	    .tiling        = VK_IMAGE_TILING_OPTIMAL,
	    .usage         = usage,
	    .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VmaAllocationCreateInfo allocation_create_info = {
	    .usage = VMA_MEMORY_USAGE_GPU_ONLY,
	};
	vmaCreateImage(vma_allocator, &image_create_info, &allocation_create_info, &texture.vk_image, &texture.vma_allocation, nullptr);
	set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) texture.vk_image, name.c_str());
	return texture;
}

VkImageView Context::create_texture_view(const std::string &name, VkImage image, VkFormat format, VkImageViewType type, const VkImageSubresourceRange &range) const
{
	VkImageView           texture_view     = VK_NULL_HANDLE;
	VkImageViewCreateInfo view_create_info = {
	    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .image            = image,
	    .viewType         = type,
	    .format           = format,
	    .components       = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
	    .subresourceRange = range,
	};
	vkCreateImageView(vk_device, &view_create_info, nullptr, &texture_view);
	set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) texture_view, name.c_str());
	return texture_view;
}

VkShaderModule Context::load_spirv_shader(const uint32_t *spirv_code, size_t size) const
{
	VkShaderModule           shader      = VK_NULL_HANDLE;
	VkShaderModuleCreateInfo create_info = {
	    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	    .codeSize = size,
	    .pCode    = spirv_code,
	};
	vkCreateShaderModule(vk_device, &create_info, nullptr, &shader);
	return shader;
}

VkShaderModule Context::load_slang_shader(const std::string &path, VkShaderStageFlagBits stage, const std::string &entry_point, const std::unordered_map<std::string, std::string> &macros) const
{
	std::vector<uint32_t> spirv;

#ifndef DEBUG
	size_t hash_val = std::hash<std::unordered_map<std::string, std::string>>{}(macros);

	glm::detail::hash_combine(hash_val, stage);
	glm::detail::hash_combine(hash_val, std::hash<std::string>{}(entry_point));

	std::string spirv_path = fmt::format("spirv/{}.{}.spv", path, hash_val);

	if (std::filesystem::exists(spirv_path))
	{
		spdlog::info("Load SPV file from: {}", spirv_path);
		std::ifstream is;
		is.open(spirv_path, std::ios::in | std::ios::binary);
		is.seekg(0, std::ios::end);
		size_t read_count = static_cast<size_t>(is.tellg());
		is.seekg(0, std::ios::beg);
		spirv.resize(static_cast<size_t>(read_count) / sizeof(uint32_t));
		is.read(reinterpret_cast<char *>(spirv.data()), read_count);
		is.close();
	}
	else
#endif
	{
		spdlog::info("Load Slang file from: {}", path);
		spirv = ShaderCompiler::compile(path, stage, entry_point, macros);
#ifndef DEBUG
		if (!std::filesystem::exists("spirv"))
		{
			std::filesystem::create_directories("spirv");
		}
		std::ofstream os;
		os.open(spirv_path, std::ios::out | std::ios::binary);
		os.write(reinterpret_cast<char *>(spirv.data()), spirv.size() * sizeof(uint32_t));
		os.flush();
		os.close();
#endif
	}

	return load_spirv_shader(spirv.data(), spirv.size());
}

DescriptorLayoutBuilder Context::create_descriptor_layout() const
{
	DescriptorLayoutBuilder builder(*this);
	return builder;
}

VkDescriptorSet Context::allocate_descriptor_set(const std::vector<VkDescriptorSetLayout> &layouts) const
{
	VkDescriptorSet             descriptor_set = VK_NULL_HANDLE;
	VkDescriptorSetAllocateInfo allocate_info  = {
	     .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
	     .pNext              = nullptr,
	     .descriptorPool     = vk_descriptor_pool,
	     .descriptorSetCount = 1,
	     .pSetLayouts        = layouts.data(),
    };
	vkAllocateDescriptorSets(vk_device, &allocate_info, &descriptor_set);
	return descriptor_set;
}

VkPipelineLayout Context::create_pipeline_layout(const std::vector<VkDescriptorSetLayout> &layouts, VkShaderStageFlags stage, uint32_t push_data_size) const
{
	VkPipelineLayout    layout = VK_NULL_HANDLE;
	VkPushConstantRange range  = {
	     .stageFlags = stage,
	     .offset     = 0,
	     .size       = push_data_size,
    };
	VkPipelineLayoutCreateInfo create_info = {
	    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	    .setLayoutCount         = static_cast<uint32_t>(layouts.size()),
	    .pSetLayouts            = layouts.data(),
	    .pushConstantRangeCount = range.size > 0 ? 1u : 0u,
	    .pPushConstantRanges    = range.size > 0 ? &range : nullptr,
	};
	vkCreatePipelineLayout(vk_device, &create_info, nullptr, &layout);
	return layout;
}

VkPipeline Context::create_compute_pipeline(VkShaderModule shader, VkPipelineLayout layout) const
{
	VkPipeline pipeline = VK_NULL_HANDLE;

	VkComputePipelineCreateInfo create_info = {
	    .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
	    .stage = {
	        .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	        .stage               = VK_SHADER_STAGE_COMPUTE_BIT,
	        .module              = shader,
	        .pName               = "main",
	        .pSpecializationInfo = nullptr,
	    },
	    .layout             = layout,
	    .basePipelineHandle = VK_NULL_HANDLE,
	    .basePipelineIndex  = -1,
	};
	vkCreateComputePipelines(vk_device, vk_pipeline_cache, 1, &create_info, nullptr, &pipeline);

	return pipeline;
}

VkPipeline Context::create_compute_pipeline(const std::string &shader_path, VkPipelineLayout layout, const std::string &entry_point, const std::unordered_map<std::string, std::string> &macros) const
{
	VkShaderModule shader   = load_slang_shader(shader_path, VK_SHADER_STAGE_COMPUTE_BIT, entry_point, macros);
	VkPipeline     pipeline = create_compute_pipeline(shader, layout);
	vkDestroyShaderModule(vk_device, shader, nullptr);
	return pipeline;
}

VkPipeline Context::create_compute_pipeline(const uint32_t *spirv_code, size_t size, VkPipelineLayout layout) const
{
	VkShaderModule shader   = load_spirv_shader(spirv_code, size);
	VkPipeline     pipeline = create_compute_pipeline(shader, layout);
	vkDestroyShaderModule(vk_device, shader, nullptr);
	return pipeline;
}

GraphicsPipelineBuilder Context::create_graphics_pipeline(VkPipelineLayout layout) const
{
	GraphicsPipelineBuilder builder(*this, layout);
	return builder;
}

DescriptorUpdateBuilder Context::update_descriptor() const
{
	DescriptorUpdateBuilder builder(*this);
	return builder;
}

void Context::wait(VkFence fence) const
{
	vkWaitForFences(vk_device, 1, &fence, VK_TRUE, UINT64_MAX);
	vkResetFences(vk_device, 1, &fence);
}

void Context::wait() const
{
	vkDeviceWaitIdle(vk_device);
}

void Context::acquire_next_image(VkSemaphore semaphore)
{
	image_index = 0;
	vkAcquireNextImageKHR(vk_device, vk_swapchain, UINT64_MAX, semaphore, nullptr, &image_index);
}

void Context::blit_back_buffer(VkCommandBuffer cmd_buffer, VkImage image, VkExtent2D extent_) const
{
	extent_.width  = extent_.width == 0 ? extent.width : extent_.width;
	extent_.height = extent_.height == 0 ? extent.height : extent_.height;

	VkImageBlit image_blit = {
	    .srcSubresource = {
	        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	        .mipLevel       = 0,
	        .baseArrayLayer = 0,
	        .layerCount     = 1,
	    },
	    .srcOffsets     = {{0, static_cast<int32_t>(extent_.height), 0}, {static_cast<int32_t>(extent_.width), 0, 1}},
	    .dstSubresource = {
	        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	        .mipLevel       = 0,
	        .baseArrayLayer = 0,
	        .layerCount     = 1,
	    },
	    .dstOffsets{{0, 0, 0}, {static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height), 1}},
	};

	vkCmdBlitImage(cmd_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapchain_images[image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &image_blit, VK_FILTER_LINEAR);
}

template <typename T>
const Context &Context::destroy(T &data) const
{
	assert(false && "No Implementation");
	return *this;
}

template <>
const Context &Context::destroy(Buffer &buffer) const
{
	if (buffer.vk_buffer)
	{
		vmaDestroyBuffer(vma_allocator, buffer.vk_buffer, buffer.vma_allocation);
		buffer.vk_buffer      = VK_NULL_HANDLE;
		buffer.vma_allocation = VK_NULL_HANDLE;
	}
	return *this;
}

template <>
const Context &Context::destroy(Texture &texture) const
{
	if (texture.vk_image)
	{
		vmaDestroyImage(vma_allocator, texture.vk_image, texture.vma_allocation);
		texture.vk_image       = VK_NULL_HANDLE;
		texture.vma_allocation = VK_NULL_HANDLE;
	}
	return *this;
}

template <>
const Context &Context::destroy(VkImageView &view) const
{
	if (view)
	{
		vkDestroyImageView(vk_device, view, nullptr);
		view = VK_NULL_HANDLE;
	}
	return *this;
}

template <>
const Context &Context::destroy(VkDescriptorSetLayout &layout) const
{
	if (layout)
	{
		vkDestroyDescriptorSetLayout(vk_device, layout, nullptr);
		layout = VK_NULL_HANDLE;
	}
	return *this;
}

template <>
const Context &Context::destroy(VkDescriptorSet &set) const
{
	if (vk_descriptor_pool)
	{
		vkFreeDescriptorSets(vk_device, vk_descriptor_pool, 1, &set);
	}
	return *this;
}

template <>
const Context &Context::destroy(VkPipelineLayout &layout) const
{
	if (layout)
	{
		vkDestroyPipelineLayout(vk_device, layout, nullptr);
		layout = VK_NULL_HANDLE;
	}
	return *this;
}

template <>
const Context &Context::destroy(VkPipeline &pipeline) const
{
	if (pipeline)
	{
		vkDestroyPipeline(vk_device, pipeline, nullptr);
		pipeline = VK_NULL_HANDLE;
	}
	return *this;
}

template <>
const Context &Context::destroy(VkSemaphore &semaphore) const
{
	if (semaphore)
	{
		vkDestroySemaphore(vk_device, semaphore, nullptr);
		semaphore = VK_NULL_HANDLE;
	}
	return *this;
}

template <>
const Context &Context::destroy(VkFence &fence) const
{
	if (fence)
	{
		vkDestroyFence(vk_device, fence, nullptr);
		fence = VK_NULL_HANDLE;
	}
	return *this;
}

template <>
const Context &Context::destroy(AccelerationStructure &as) const
{
	if (as.vk_as)
	{
		vmaDestroyBuffer(vma_allocator, as.buffer.vk_buffer, as.buffer.vma_allocation);
		vkDestroyAccelerationStructureKHR(vk_device, as.vk_as, nullptr);
		as.vk_as                 = VK_NULL_HANDLE;
		as.buffer.vk_buffer      = VK_NULL_HANDLE;
		as.buffer.vma_allocation = VK_NULL_HANDLE;
	}
	return *this;
}

template const Context &Context::destroy<Buffer>(Buffer &) const;
template const Context &Context::destroy<Texture>(Texture &) const;
template const Context &Context::destroy<VkImageView>(VkImageView &) const;
template const Context &Context::destroy<VkDescriptorSetLayout>(VkDescriptorSetLayout &) const;
template const Context &Context::destroy<VkDescriptorSet>(VkDescriptorSet &) const;
template const Context &Context::destroy<VkPipelineLayout>(VkPipelineLayout &) const;
template const Context &Context::destroy<VkPipeline>(VkPipeline &) const;
template const Context &Context::destroy<VkSemaphore>(VkSemaphore &) const;
template const Context &Context::destroy<VkFence>(VkFence &) const;
template const Context &Context::destroy<AccelerationStructure>(AccelerationStructure &) const;

void Context::set_object_name(VkObjectType type, uint64_t handle, const char *name) const
{
#ifdef DEBUG
	VkDebugUtilsObjectNameInfoEXT info = {
	    .sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
	    .objectType   = type,
	    .objectHandle = handle,
	    .pObjectName  = name,
	};
	vkSetDebugUtilsObjectNameEXT(vk_device, &info);
#endif        // DEBUG
}

Buffer Context::create_scratch_buffer(size_t size) const
{
	VkPhysicalDeviceAccelerationStructurePropertiesKHR properties = {};

	properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
	properties.pNext = NULL;

	VkPhysicalDeviceProperties2 dev_props2 = {};

	dev_props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	dev_props2.pNext = &properties;

	vkGetPhysicalDeviceProperties2(vk_physical_device, &dev_props2);

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
	vmaCreateBuffer(vma_allocator, &buffer_create_info, &allocation_create_info, &buffer.vk_buffer, &buffer.vma_allocation, &allocation_info);
	VkBufferDeviceAddressInfoKHR buffer_device_address_info = {
	    .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
	    .buffer = buffer.vk_buffer,
	};
	buffer.device_address = vkGetBufferDeviceAddressKHR(vk_device, &buffer_device_address_info);
	buffer.device_address = align(buffer.device_address, properties.minAccelerationStructureScratchOffsetAlignment);
	return buffer;
}
