#include "context.hpp"

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

CommandBufferRecorder::CommandBufferRecorder(const Context &context, VkCommandBuffer cmd_buffer) :
    context(&context), cmd_buffer(cmd_buffer)
{
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

CommandBufferRecorder &CommandBufferRecorder::push_constants(VkPipelineLayout pipeline_layout, VkShaderStageFlags stages, void *data, size_t size)
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

BarrierBuilder CommandBufferRecorder::insert_barrier()
{
	return BarrierBuilder(*this);
}

void CommandBufferRecorder::flush(bool compute)
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
	VkShaderModule shader = context->load_hlsl_shader(shader_path, stage, entry_point, macros);
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

