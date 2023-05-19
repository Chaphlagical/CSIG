#include "render/pipeline/composite.hpp"

#define NUM_THREADS_X 8
#define NUM_THREADS_Y 8

static unsigned char g_composite_comp_spv_data[] = {
#include "composite.comp.spv.h"
};

Composite::Composite(const Context &context, const LUT &lut, const Scene &scene, const GBufferPass &gbuffer_pass) :
    m_context(&context)
{
	{
		VkImageCreateInfo image_create_info = {
		    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		    .imageType     = VK_IMAGE_TYPE_2D,
		    .format        = VK_FORMAT_R16G16B16A16_SFLOAT,
		    .extent        = VkExtent3D{m_context->extent.width, m_context->extent.height, 1},
		    .mipLevels     = 1,
		    .arrayLayers   = 1,
		    .samples       = VK_SAMPLE_COUNT_1_BIT,
		    .tiling        = VK_IMAGE_TILING_OPTIMAL,
		    .usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		    .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
		    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};
		VmaAllocationCreateInfo allocation_create_info = {
		    .usage = VMA_MEMORY_USAGE_GPU_ONLY,
		};
		vmaCreateImage(context.vma_allocator, &image_create_info, &allocation_create_info, &output_image.vk_image, &output_image.vma_allocation, nullptr);
		VkImageViewCreateInfo view_create_info = {
		    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image            = output_image.vk_image,
		    .viewType         = VK_IMAGE_VIEW_TYPE_2D,
		    .format           = VK_FORMAT_R16G16B16A16_SFLOAT,
		    .components       = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
		    .subresourceRange = {
		        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		        .baseMipLevel   = 0,
		        .levelCount     = 1,
		        .baseArrayLayer = 0,
		        .layerCount     = 1,
		    },
		};
		vkCreateImageView(context.vk_device, &view_create_info, nullptr, &output_view);
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) output_image.vk_image, "Composite Output Image");
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) output_view, "Composite Output Image View");
	}

	// Tonemapping pass
	{
		// Create shader module
		VkShaderModule shader = VK_NULL_HANDLE;
		{
			VkShaderModuleCreateInfo create_info = {
			    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			    .codeSize = sizeof(g_composite_comp_spv_data),
			    .pCode    = reinterpret_cast<uint32_t *>(g_composite_comp_spv_data),
			};
			vkCreateShaderModule(context.vk_device, &create_info, nullptr, &shader);
		}

		// Create descriptor set layout
		{
			VkDescriptorSetLayoutBinding bindings[] = {
			    // Output
			    {
			        .binding         = 0,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Direct light
			    {
			        .binding         = 1,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Reflection
			    {
			        .binding         = 2,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // AO
			    {
			        .binding         = 3,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // GI
			    {
			        .binding         = 4,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			};
			VkDescriptorSetLayoutCreateInfo create_info = {
			    .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			    .flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
			    .bindingCount = 5,
			    .pBindings    = bindings,
			};
			vkCreateDescriptorSetLayout(m_context->vk_device, &create_info, nullptr, &m_descriptor_set_layout);
		}

		// Allocate descriptor set
		{
			VkDescriptorSetLayout descriptor_set_layouts[] = {
			    m_descriptor_set_layout,
			};

			VkDescriptorSetAllocateInfo allocate_info = {
			    .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			    .pNext              = nullptr,
			    .descriptorPool     = m_context->vk_descriptor_pool,
			    .descriptorSetCount = 1,
			    .pSetLayouts        = descriptor_set_layouts,
			};
			vkAllocateDescriptorSets(m_context->vk_device, &allocate_info, &m_descriptor_set);
		}

		// Create pipeline layout
		{
			VkDescriptorSetLayout descriptor_set_layouts[] = {
			    scene.descriptor.layout,
			    gbuffer_pass.descriptor.layout,
			    lut.descriptor.layout,
			    m_descriptor_set_layout,
			};
			VkPushConstantRange range = {
			    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			    .offset     = 0,
			    .size       = sizeof(m_push_constants),
			};
			VkPipelineLayoutCreateInfo create_info = {
			    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			    .setLayoutCount         = 4,
			    .pSetLayouts            = descriptor_set_layouts,
			    .pushConstantRangeCount = 1,
			    .pPushConstantRanges    = &range,
			};
			vkCreatePipelineLayout(m_context->vk_device, &create_info, nullptr, &m_pipeline_layout);
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
			    .layout             = m_pipeline_layout,
			    .basePipelineHandle = VK_NULL_HANDLE,
			    .basePipelineIndex  = -1,
			};
			vkCreateComputePipelines(m_context->vk_device, m_context->vk_pipeline_cache, 1, &create_info, nullptr, &m_pipeline);
			vkDestroyShaderModule(m_context->vk_device, shader, nullptr);
		}
	}
}

Composite::~Composite()
{
	vkDestroyImageView(m_context->vk_device, output_view, nullptr);
	vmaDestroyImage(m_context->vma_allocator, output_image.vk_image, output_image.vma_allocation);
	vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 1, &m_descriptor_set);
	vkDestroyDescriptorSetLayout(m_context->vk_device, m_descriptor_set_layout, nullptr);
	vkDestroyPipelineLayout(m_context->vk_device, m_pipeline_layout, nullptr);
	vkDestroyPipeline(m_context->vk_device, m_pipeline, nullptr);
}

void Composite::init(VkCommandBuffer cmd_buffer)
{
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
	    0, 0, nullptr, 0, nullptr, 1, image_barriers);
}

void Composite::update(const Scene &scene, VkImageView direct, VkImageView reflection, VkImageView ao, VkImageView gi)
{
	VkDescriptorImageInfo direct_info = {
	    .sampler     = scene.linear_sampler,
	    .imageView   = direct,
	    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};

	VkDescriptorImageInfo reflection_info = {
	    .sampler     = scene.linear_sampler,
	    .imageView   = reflection,
	    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};

	VkDescriptorImageInfo ao_info = {
	    .sampler     = scene.linear_sampler,
	    .imageView   = ao,
	    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};

	VkDescriptorImageInfo gi_info = {
	    .sampler     = scene.linear_sampler,
	    .imageView   = gi,
	    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};

	VkDescriptorImageInfo output_info = {
	    .sampler     = VK_NULL_HANDLE,
	    .imageView   = output_view,
	    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	};

	{
		VkWriteDescriptorSet writes[] = {
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_descriptor_set,
		        .dstBinding       = 0,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		        .pImageInfo       = &output_info,
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_descriptor_set,
		        .dstBinding       = 1,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .pImageInfo       = &direct_info,
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_descriptor_set,
		        .dstBinding       = 2,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .pImageInfo       = &reflection_info,
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_descriptor_set,
		        .dstBinding       = 3,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .pImageInfo       = &ao_info,
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_descriptor_set,
		        .dstBinding       = 4,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .pImageInfo       = &gi_info,
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		};
		vkUpdateDescriptorSets(m_context->vk_device, 5, writes, 0, nullptr);
	}
}

void Composite::draw(VkCommandBuffer cmd_buffer, const Scene &scene, const GBufferPass &gbuffer_pass, const LUT &lut)
{
	{
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
		    0, 0, nullptr, 0, nullptr, 1, image_barriers);
	}

	m_context->begin_marker(cmd_buffer, "Composite");
	{
		VkDescriptorSet descriptors[] = {
		    scene.descriptor.set,
		    gbuffer_pass.descriptor.sets[m_context->ping_pong],
		    lut.descriptor.set,
		    m_descriptor_set,
		};
		vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline_layout, 0, 4, descriptors, 0, nullptr);
		vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
		vkCmdPushConstants(cmd_buffer, m_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(m_push_constants), &m_push_constants);
		vkCmdDispatch(cmd_buffer, static_cast<uint32_t>(ceil(float(m_context->extent.width) / float(NUM_THREADS_X))), static_cast<uint32_t>(ceil(float(m_context->extent.height) / float(NUM_THREADS_Y))), 1);
	}
	m_context->end_marker(cmd_buffer);

	{
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
		    0, 0, nullptr, 0, nullptr, 1, image_barriers);
	}
}

bool Composite::draw_ui()
{
	return false;
}
