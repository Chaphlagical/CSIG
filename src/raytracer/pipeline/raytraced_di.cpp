#include "render/pipeline/raytraced_di.hpp"

#include <imgui.h>

#define NUM_THREADS_X 8
#define NUM_THREADS_Y 8

static unsigned char g_di_temporal_comp_spv_data[] = {
#include "di_temporal.comp.spv.h"
};

static unsigned char g_di_spatial_comp_spv_data[] = {
#include "di_spatial.comp.spv.h"
};

static unsigned char g_di_composite_comp_spv_data[] = {
#include "di_composite.comp.spv.h"
};

RayTracedDI::RayTracedDI(const Context &context, const Scene &scene, const GBufferPass &gbuffer_pass) :
    m_context(&context)
{
	size_t reservoir_size = static_cast<size_t>(context.extent.width * context.extent.height) * sizeof(Reservoir);
	{
		VkBufferCreateInfo buffer_create_info = {
		    .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		    .size        = reservoir_size,
		    .usage       = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};
		VmaAllocationCreateInfo allocation_create_info = {
		    .usage = VMA_MEMORY_USAGE_GPU_ONLY,
		};
		VmaAllocationInfo allocation_info = {};
		{
			vmaCreateBuffer(context.vma_allocator, &buffer_create_info, &allocation_create_info, &temporal_reservoir_buffer.vk_buffer, &temporal_reservoir_buffer.vma_allocation, &allocation_info);
			VkBufferDeviceAddressInfoKHR buffer_device_address_info = {
			    .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
			    .buffer = temporal_reservoir_buffer.vk_buffer,
			};
			temporal_reservoir_buffer.device_address = vkGetBufferDeviceAddress(context.vk_device, &buffer_device_address_info);
			m_context->set_object_name(VK_OBJECT_TYPE_BUFFER, (uint64_t) temporal_reservoir_buffer.vk_buffer, "Temporal Reservoir Buffer");
		}
		{
			vmaCreateBuffer(context.vma_allocator, &buffer_create_info, &allocation_create_info, &passthrough_reservoir_buffer.vk_buffer, &passthrough_reservoir_buffer.vma_allocation, &allocation_info);
			VkBufferDeviceAddressInfoKHR buffer_device_address_info = {
			    .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
			    .buffer = passthrough_reservoir_buffer.vk_buffer,
			};
			passthrough_reservoir_buffer.device_address = vkGetBufferDeviceAddress(context.vk_device, &buffer_device_address_info);
			m_context->set_object_name(VK_OBJECT_TYPE_BUFFER, (uint64_t) passthrough_reservoir_buffer.vk_buffer, "Passthrough Reservoir Buffer");
		}
		{
			vmaCreateBuffer(context.vma_allocator, &buffer_create_info, &allocation_create_info, &spatial_reservoir_buffer.vk_buffer, &spatial_reservoir_buffer.vma_allocation, &allocation_info);
			VkBufferDeviceAddressInfoKHR buffer_device_address_info = {
			    .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
			    .buffer = spatial_reservoir_buffer.vk_buffer,
			};
			spatial_reservoir_buffer.device_address = vkGetBufferDeviceAddress(context.vk_device, &buffer_device_address_info);
			m_context->set_object_name(VK_OBJECT_TYPE_BUFFER, (uint64_t) spatial_reservoir_buffer.vk_buffer, "Spatial Reservoir Buffer");
		}
	}

	{
		VkImageCreateInfo image_create_info = {
		    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		    .imageType     = VK_IMAGE_TYPE_2D,
		    .format        = VK_FORMAT_R32G32B32A32_SFLOAT,
		    .extent        = VkExtent3D{static_cast<uint32_t>(context.extent.width), static_cast<uint32_t>(context.extent.height), 1},
		    .mipLevels     = 1,
		    .arrayLayers   = 1,
		    .samples       = VK_SAMPLE_COUNT_1_BIT,
		    .tiling        = VK_IMAGE_TILING_OPTIMAL,
		    .usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		    .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
		    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};
		VmaAllocationCreateInfo allocation_create_info = {
		    .usage = VMA_MEMORY_USAGE_GPU_ONLY,
		};
		vmaCreateImage(m_context->vma_allocator, &image_create_info, &allocation_create_info, &output_image.vk_image, &output_image.vma_allocation, nullptr);
		VkImageViewCreateInfo view_create_info = {
		    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image            = output_image.vk_image,
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
		vkCreateImageView(m_context->vk_device, &view_create_info, nullptr, &output_view);
	}

	// Create temporal pass
	{
		// Create shader module
		VkShaderModule shader = VK_NULL_HANDLE;
		{
			VkShaderModuleCreateInfo create_info = {
			    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			    .codeSize = sizeof(g_di_temporal_comp_spv_data),
			    .pCode    = reinterpret_cast<uint32_t *>(g_di_temporal_comp_spv_data),
			};
			vkCreateShaderModule(context.vk_device, &create_info, nullptr, &shader);
		}

		// Create pipeline layout
		{
			VkPushConstantRange range = {
			    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			    .offset     = 0,
			    .size       = sizeof(m_temporal_pass.push_constants),
			};
			VkDescriptorSetLayout descriptor_set_layouts[] = {
			    scene.descriptor.layout,
			    gbuffer_pass.descriptor.layout,
			};
			VkPipelineLayoutCreateInfo create_info = {
			    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			    .setLayoutCount         = 2,
			    .pSetLayouts            = descriptor_set_layouts,
			    .pushConstantRangeCount = 1,
			    .pPushConstantRanges    = &range,
			};
			vkCreatePipelineLayout(m_context->vk_device, &create_info, nullptr, &m_temporal_pass.pipeline_layout);
		}

		// Create pipeline
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
			    .layout             = m_temporal_pass.pipeline_layout,
			    .basePipelineHandle = VK_NULL_HANDLE,
			    .basePipelineIndex  = -1,
			};
			vkCreateComputePipelines(m_context->vk_device, m_context->vk_pipeline_cache, 1, &create_info, nullptr, &m_temporal_pass.pipeline);
			vkDestroyShaderModule(m_context->vk_device, shader, nullptr);
		}
	}

	// Create spatial pass
	{
		// Create shader module
		VkShaderModule shader = VK_NULL_HANDLE;
		{
			VkShaderModuleCreateInfo create_info = {
			    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			    .codeSize = sizeof(g_di_spatial_comp_spv_data),
			    .pCode    = reinterpret_cast<uint32_t *>(g_di_spatial_comp_spv_data),
			};
			vkCreateShaderModule(context.vk_device, &create_info, nullptr, &shader);
		}

		// Create pipeline layout
		{
			VkPushConstantRange range = {
			    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			    .offset     = 0,
			    .size       = sizeof(m_spatial_pass.push_constants),
			};
			VkDescriptorSetLayout descriptor_set_layouts[] = {
			    scene.descriptor.layout,
			    gbuffer_pass.descriptor.layout,
			};
			VkPipelineLayoutCreateInfo create_info = {
			    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			    .setLayoutCount         = 2,
			    .pSetLayouts            = descriptor_set_layouts,
			    .pushConstantRangeCount = 1,
			    .pPushConstantRanges    = &range,
			};
			vkCreatePipelineLayout(m_context->vk_device, &create_info, nullptr, &m_spatial_pass.pipeline_layout);
		}

		// Create pipeline
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
			    .layout             = m_spatial_pass.pipeline_layout,
			    .basePipelineHandle = VK_NULL_HANDLE,
			    .basePipelineIndex  = -1,
			};
			vkCreateComputePipelines(m_context->vk_device, m_context->vk_pipeline_cache, 1, &create_info, nullptr, &m_spatial_pass.pipeline);
			vkDestroyShaderModule(m_context->vk_device, shader, nullptr);
		}
	}

	// Create composite pass
	{
		// Create shader module
		VkShaderModule shader = VK_NULL_HANDLE;
		{
			VkShaderModuleCreateInfo create_info = {
			    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			    .codeSize = sizeof(g_di_composite_comp_spv_data),
			    .pCode    = reinterpret_cast<uint32_t *>(g_di_composite_comp_spv_data),
			};
			vkCreateShaderModule(context.vk_device, &create_info, nullptr, &shader);
		}

		// Create descriptor set layout
		{
			VkDescriptorSetLayoutBinding bindings[] = {
			    // Output Image
			    {
			        .binding         = 0,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			};
			VkDescriptorSetLayoutCreateInfo create_info = {
			    .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			    .bindingCount = 1,
			    .pBindings    = bindings,
			};
			vkCreateDescriptorSetLayout(m_context->vk_device, &create_info, nullptr, &m_composite_pass.descriptor_set_layout);
		}

		// Allocate descriptor set
		{
			VkDescriptorSetLayout descriptor_set_layouts[] = {m_composite_pass.descriptor_set_layout};

			VkDescriptorSetAllocateInfo allocate_info = {
			    .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			    .pNext              = nullptr,
			    .descriptorPool     = m_context->vk_descriptor_pool,
			    .descriptorSetCount = 1,
			    .pSetLayouts        = descriptor_set_layouts,
			};
			vkAllocateDescriptorSets(m_context->vk_device, &allocate_info, &m_composite_pass.descriptor_set);
		}

		// Create pipeline layout
		{
			VkPushConstantRange range = {
			    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			    .offset     = 0,
			    .size       = sizeof(m_composite_pass.push_constants),
			};
			VkDescriptorSetLayout descriptor_set_layouts[] = {
			    scene.descriptor.layout,
			    gbuffer_pass.descriptor.layout,
			    m_composite_pass.descriptor_set_layout,
			};
			VkPipelineLayoutCreateInfo create_info = {
			    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			    .setLayoutCount         = 3,
			    .pSetLayouts            = descriptor_set_layouts,
			    .pushConstantRangeCount = 1,
			    .pPushConstantRanges    = &range,
			};
			vkCreatePipelineLayout(m_context->vk_device, &create_info, nullptr, &m_composite_pass.pipeline_layout);
		}

		// Create pipeline
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
			    .layout             = m_composite_pass.pipeline_layout,
			    .basePipelineHandle = VK_NULL_HANDLE,
			    .basePipelineIndex  = -1,
			};
			vkCreateComputePipelines(m_context->vk_device, m_context->vk_pipeline_cache, 1, &create_info, nullptr, &m_composite_pass.pipeline);
			vkDestroyShaderModule(m_context->vk_device, shader, nullptr);
		}
	}
}

RayTracedDI::~RayTracedDI()
{
	vmaDestroyBuffer(m_context->vma_allocator, temporal_reservoir_buffer.vk_buffer, temporal_reservoir_buffer.vma_allocation);
	vmaDestroyBuffer(m_context->vma_allocator, passthrough_reservoir_buffer.vk_buffer, passthrough_reservoir_buffer.vma_allocation);
	vmaDestroyBuffer(m_context->vma_allocator, spatial_reservoir_buffer.vk_buffer, spatial_reservoir_buffer.vma_allocation);
	vkDestroyImageView(m_context->vk_device, output_view, nullptr);
	vmaDestroyImage(m_context->vma_allocator, output_image.vk_image, output_image.vma_allocation);
	vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 1, &m_composite_pass.descriptor_set);
	vkDestroyDescriptorSetLayout(m_context->vk_device, m_composite_pass.descriptor_set_layout, nullptr);
	vkDestroyPipelineLayout(m_context->vk_device, m_temporal_pass.pipeline_layout, nullptr);
	vkDestroyPipelineLayout(m_context->vk_device, m_spatial_pass.pipeline_layout, nullptr);
	vkDestroyPipelineLayout(m_context->vk_device, m_composite_pass.pipeline_layout, nullptr);
	vkDestroyPipeline(m_context->vk_device, m_temporal_pass.pipeline, nullptr);
	vkDestroyPipeline(m_context->vk_device, m_spatial_pass.pipeline, nullptr);
	vkDestroyPipeline(m_context->vk_device, m_composite_pass.pipeline, nullptr);
}

void RayTracedDI::init(VkCommandBuffer cmd_buffer)
{
	{
		VkBufferMemoryBarrier buffer_barriers[] = {
		    {
		        .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		        .pNext               = nullptr,
		        .srcAccessMask       = 0,
		        .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .buffer              = temporal_reservoir_buffer.vk_buffer,
		        .offset              = 0,
		        .size                = VK_WHOLE_SIZE,
		    },
		    {
		        .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		        .pNext               = nullptr,
		        .srcAccessMask       = 0,
		        .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .buffer              = passthrough_reservoir_buffer.vk_buffer,
		        .offset              = 0,
		        .size                = VK_WHOLE_SIZE,
		    },
		    {
		        .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		        .pNext               = nullptr,
		        .srcAccessMask       = 0,
		        .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .buffer              = spatial_reservoir_buffer.vk_buffer,
		        .offset              = 0,
		        .size                = VK_WHOLE_SIZE,
		    },
		};
		VkImageMemoryBarrier image_barriers[] = {
		    {
		        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask       = 0,
		        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
		        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
		        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image               = output_image.vk_image,
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
		    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		    0, 0, nullptr, 3, buffer_barriers, 1, image_barriers);
	}

	vkCmdFillBuffer(cmd_buffer, temporal_reservoir_buffer.vk_buffer, 0, VK_WHOLE_SIZE, 0);
	vkCmdFillBuffer(cmd_buffer, passthrough_reservoir_buffer.vk_buffer, 0, VK_WHOLE_SIZE, 0);
	vkCmdFillBuffer(cmd_buffer, spatial_reservoir_buffer.vk_buffer, 0, VK_WHOLE_SIZE, 0);

	{
		VkBufferMemoryBarrier buffer_barriers[] = {
		    {
		        .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		        .pNext               = nullptr,
		        .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
		        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .buffer              = temporal_reservoir_buffer.vk_buffer,
		        .offset              = 0,
		        .size                = VK_WHOLE_SIZE,
		    },
		    {
		        .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		        .pNext               = nullptr,
		        .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
		        .dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .buffer              = passthrough_reservoir_buffer.vk_buffer,
		        .offset              = 0,
		        .size                = VK_WHOLE_SIZE,
		    },
		    {
		        .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		        .pNext               = nullptr,
		        .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
		        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .buffer              = spatial_reservoir_buffer.vk_buffer,
		        .offset              = 0,
		        .size                = VK_WHOLE_SIZE,
		    },
		};
		vkCmdPipelineBarrier(
		    cmd_buffer,
		    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		    0, 0, nullptr, 3, buffer_barriers, 0, nullptr);
	}
}

void RayTracedDI::update(const Scene &scene, const BlueNoise &blue_noise, const GBufferPass &gbuffer_pass)
{
	VkDescriptorImageInfo output_image_info = {
	    .sampler     = VK_NULL_HANDLE,
	    .imageView   = output_view,
	    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	};

	VkWriteDescriptorSet writes[] = {
	    {
	        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	        .dstSet           = m_composite_pass.descriptor_set,
	        .dstBinding       = 0,
	        .dstArrayElement  = 0,
	        .descriptorCount  = 1,
	        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
	        .pImageInfo       = &output_image_info,
	        .pBufferInfo      = nullptr,
	        .pTexelBufferView = nullptr,
	    },
	};
	vkUpdateDescriptorSets(m_context->vk_device, 1, writes, 0, nullptr);
}

void RayTracedDI::draw(VkCommandBuffer cmd_buffer, const Scene &scene, const GBufferPass &gbuffer_pass)
{
	m_context->begin_marker(cmd_buffer, "Raytraced DI");
	{
		m_context->begin_marker(cmd_buffer, "Raytraced DI - Temporal Pass");
		{
			VkDescriptorSet descriptors[] = {
			    scene.descriptor.set,
			    gbuffer_pass.descriptor.sets[m_context->ping_pong],
			};
			m_temporal_pass.push_constants.temporal_reservoir_addr    = temporal_reservoir_buffer.device_address;
			m_temporal_pass.push_constants.passthrough_reservoir_addr = passthrough_reservoir_buffer.device_address;
			m_temporal_pass.push_constants.temporal_reuse             = m_temporal_reuse;
			vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_temporal_pass.pipeline_layout, 0, 2, descriptors, 0, nullptr);
			vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_temporal_pass.pipeline);
			vkCmdPushConstants(cmd_buffer, m_temporal_pass.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(m_temporal_pass.push_constants), &m_temporal_pass.push_constants);
			vkCmdDispatch(cmd_buffer, static_cast<uint32_t>(ceil(float(m_context->extent.width) / float(NUM_THREADS_X))), static_cast<uint32_t>(ceil(float(m_context->extent.height) / float(NUM_THREADS_Y))), 1);
		}
		m_context->end_marker(cmd_buffer);

		{
			VkBufferMemoryBarrier buffer_barriers[] = {
			    {
			        .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			        .pNext               = nullptr,
			        .srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
			        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
			        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .buffer              = passthrough_reservoir_buffer.vk_buffer,
			        .offset              = 0,
			        .size                = VK_WHOLE_SIZE,
			    },
			    {
			        .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			        .pNext               = nullptr,
			        .srcAccessMask       = VK_ACCESS_SHADER_READ_BIT,
			        .dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
			        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .buffer              = spatial_reservoir_buffer.vk_buffer,
			        .offset              = 0,
			        .size                = VK_WHOLE_SIZE,
			    },
			};
			vkCmdPipelineBarrier(
			    cmd_buffer,
			    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			    0, 0, nullptr, 2, buffer_barriers, 0, nullptr);
		}

		m_context->begin_marker(cmd_buffer, "Raytraced DI - Spatial Pass");
		{
			VkDescriptorSet descriptors[] = {
			    scene.descriptor.set,
			    gbuffer_pass.descriptor.sets[m_context->ping_pong],
			};
			m_spatial_pass.push_constants.passthrough_reservoir_addr = passthrough_reservoir_buffer.device_address;
			m_spatial_pass.push_constants.spatial_reservoir_addr     = spatial_reservoir_buffer.device_address;
			m_spatial_pass.push_constants.spatial_reuse              = m_spatial_reuse;
			vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_spatial_pass.pipeline_layout, 0, 2, descriptors, 0, nullptr);
			vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_spatial_pass.pipeline);
			vkCmdPushConstants(cmd_buffer, m_spatial_pass.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(m_spatial_pass.push_constants), &m_spatial_pass.push_constants);
			vkCmdDispatch(cmd_buffer, static_cast<uint32_t>(ceil(float(m_context->extent.width) / float(NUM_THREADS_X))), static_cast<uint32_t>(ceil(float(m_context->extent.height) / float(NUM_THREADS_Y))), 1);
		}
		m_context->end_marker(cmd_buffer);

		{
			VkBufferMemoryBarrier buffer_barriers[] = {
			    {
			        .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			        .pNext               = nullptr,
			        .srcAccessMask       = VK_ACCESS_SHADER_READ_BIT,
			        .dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
			        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .buffer              = temporal_reservoir_buffer.vk_buffer,
			        .offset              = 0,
			        .size                = VK_WHOLE_SIZE,
			    },
			    {
			        .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			        .pNext               = nullptr,
			        .srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
			        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
			        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .buffer              = spatial_reservoir_buffer.vk_buffer,
			        .offset              = 0,
			        .size                = VK_WHOLE_SIZE,
			    },
			};
			VkImageMemoryBarrier image_barriers[] = {
			    {
			        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			        .srcAccessMask       = VK_ACCESS_SHADER_READ_BIT,
			        .dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
			        .oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			        .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
			        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .image               = output_image.vk_image,
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
			    0, 0, nullptr, 2, buffer_barriers, 1, image_barriers);
		}

		m_context->begin_marker(cmd_buffer, "Raytraced DI - Composite Pass");
		{
			VkDescriptorSet descriptors[] = {
			    scene.descriptor.set,
			    gbuffer_pass.descriptor.sets[m_context->ping_pong],
			    m_composite_pass.descriptor_set,
			};
			m_composite_pass.push_constants.passthrough_reservoir_addr = passthrough_reservoir_buffer.device_address;
			m_composite_pass.push_constants.temporal_reservoir_addr    = temporal_reservoir_buffer.device_address;
			m_composite_pass.push_constants.spatial_reservoir_addr     = spatial_reservoir_buffer.device_address;
			m_composite_pass.push_constants.normal_bias                = m_normal_bias;
			vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_composite_pass.pipeline_layout, 0, 3, descriptors, 0, nullptr);
			vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_composite_pass.pipeline);
			vkCmdPushConstants(cmd_buffer, m_composite_pass.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(m_composite_pass.push_constants), &m_composite_pass.push_constants);
			vkCmdDispatch(cmd_buffer, static_cast<uint32_t>(ceil(float(m_context->extent.width) / float(NUM_THREADS_X))), static_cast<uint32_t>(ceil(float(m_context->extent.height) / float(NUM_THREADS_Y))), 1);
		}
		m_context->end_marker(cmd_buffer);

		{
			VkBufferMemoryBarrier buffer_barriers[] = {
			    {
			        .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			        .pNext               = nullptr,
			        .srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
			        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
			        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .buffer              = temporal_reservoir_buffer.vk_buffer,
			        .offset              = 0,
			        .size                = VK_WHOLE_SIZE,
			    },
			    {
			        .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			        .pNext               = nullptr,
			        .srcAccessMask       = VK_ACCESS_SHADER_READ_BIT,
			        .dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
			        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .buffer              = passthrough_reservoir_buffer.vk_buffer,
			        .offset              = 0,
			        .size                = VK_WHOLE_SIZE,
			    },
			};
			VkImageMemoryBarrier image_barriers[] = {
			    {
			        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			        .srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
			        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
			        .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
			        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .image               = output_image.vk_image,
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
			    0, 0, nullptr, 2, buffer_barriers, 1, image_barriers);
		}
	}
	m_context->end_marker(cmd_buffer);
}

bool RayTracedDI::draw_ui()
{
	if (ImGui::TreeNode("Raytrace DI"))
	{
		ImGui::DragFloat("Bias", &m_normal_bias, 0.00001f, -1.f, 1.f, "%.10f");
		ImGui::DragInt("M", &m_temporal_pass.push_constants.M, 1, 1, 32);
		ImGui::Checkbox("Temporal Reuse", &m_temporal_reuse);
		if (ImGui::TreeNode("Spatial Reuse"))
		{
			ImGui::Checkbox("Enable", &m_spatial_reuse);
			ImGui::DragInt("Samples", &m_spatial_pass.push_constants.samples, 1, 1, 32);
			ImGui::DragFloat("Radius", &m_spatial_pass.push_constants.radius, 0.1f, 0.f, 30.f, "%.1f");
			ImGui::TreePop();
		}
		ImGui::TreePop();
	}
	return false;
}
