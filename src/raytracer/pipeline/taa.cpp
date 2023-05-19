#include "render/pipeline/taa.hpp"

#define NUM_THREADS_X 8
#define NUM_THREADS_Y 8

static unsigned char g_taa_comp_spv_data[] = {
#include "taa.comp.spv.h"
};

TAA::TAA(const Context &context, const Scene &scene, const GBufferPass &gbuffer_pass) :
    m_context(&context)
{
	for (uint32_t i = 0; i < 2; i++)
	{
		VkImageCreateInfo image_create_info = {
		    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		    .imageType     = VK_IMAGE_TYPE_2D,
		    .format        = VK_FORMAT_R8G8B8A8_UNORM,
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
		vmaCreateImage(context.vma_allocator, &image_create_info, &allocation_create_info, &output_image[i].vk_image, &output_image[i].vma_allocation, nullptr);
		VkImageViewCreateInfo view_create_info = {
		    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image            = output_image[i].vk_image,
		    .viewType         = VK_IMAGE_VIEW_TYPE_2D,
		    .format           = VK_FORMAT_R8G8B8A8_UNORM,
		    .components       = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
		    .subresourceRange = {
		        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		        .baseMipLevel   = 0,
		        .levelCount     = 1,
		        .baseArrayLayer = 0,
		        .layerCount     = 1,
		    },
		};
		vkCreateImageView(context.vk_device, &view_create_info, nullptr, &output_view[i]);
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) output_image[i].vk_image, (std::string("TAA Image - ") + std::to_string(i)).c_str());
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) output_view[i], (std::string("TAA Image View - ") + std::to_string(i)).c_str());
	}

	// Tonemapping pass
	{
		// Create shader module
		VkShaderModule shader = VK_NULL_HANDLE;
		{
			VkShaderModuleCreateInfo create_info = {
			    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			    .codeSize = sizeof(g_taa_comp_spv_data),
			    .pCode    = reinterpret_cast<uint32_t *>(g_taa_comp_spv_data),
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
			    // Input
			    {
			        .binding         = 1,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Prev output
			    {
			        .binding         = 2,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			};
			VkDescriptorSetLayoutCreateInfo create_info = {
			    .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			    .flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
			    .bindingCount = 3,
			    .pBindings    = bindings,
			};
			vkCreateDescriptorSetLayout(m_context->vk_device, &create_info, nullptr, &m_descriptor_set_layout);
		}

		// Allocate descriptor set
		{
			VkDescriptorSetLayout descriptor_set_layouts[] = {
			    m_descriptor_set_layout,
			    m_descriptor_set_layout,
			};
			VkDescriptorSetAllocateInfo allocate_info = {
			    .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			    .pNext              = nullptr,
			    .descriptorPool     = m_context->vk_descriptor_pool,
			    .descriptorSetCount = 2,
			    .pSetLayouts        = descriptor_set_layouts,
			};
			vkAllocateDescriptorSets(m_context->vk_device, &allocate_info, m_descriptor_sets);
		}

		// Create pipeline layout
		{
			VkDescriptorSetLayout descriptor_set_layouts[] = {
			    scene.descriptor.layout,
			    gbuffer_pass.descriptor.layout,
			    m_descriptor_set_layout,
			};
			VkPushConstantRange range = {
			    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			    .offset     = 0,
			    .size       = sizeof(m_push_constants),
			};
			VkPipelineLayoutCreateInfo create_info = {
			    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			    .setLayoutCount         = 3,
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

TAA::~TAA()
{
	vkDestroyImageView(m_context->vk_device, output_view[0], nullptr);
	vkDestroyImageView(m_context->vk_device, output_view[1], nullptr);
	vmaDestroyImage(m_context->vma_allocator, output_image[0].vk_image, output_image[0].vma_allocation);
	vmaDestroyImage(m_context->vma_allocator, output_image[1].vk_image, output_image[1].vma_allocation);
	vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 2, m_descriptor_sets);
	vkDestroyDescriptorSetLayout(m_context->vk_device, m_descriptor_set_layout, nullptr);
	vkDestroyPipelineLayout(m_context->vk_device, m_pipeline_layout, nullptr);
	vkDestroyPipeline(m_context->vk_device, m_pipeline, nullptr);
}

void TAA::init(VkCommandBuffer cmd_buffer)
{
	VkImageMemoryBarrier image_barriers[] = {
	    {
	        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .srcAccessMask       = 0,
	        .dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
	        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
	        .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image               = output_image[0].vk_image,
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
	        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
	        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
	        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image               = output_image[1].vk_image,
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
	    0, 0, nullptr, 0, nullptr, 2, image_barriers);
}

void TAA::update(const Scene &scene, const GBufferPass &gbuffer_pass, VkImageView result)
{
	VkDescriptorImageInfo result_info = {
	    .sampler     = scene.linear_sampler,
	    .imageView   = result,
	    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};

	VkDescriptorImageInfo output_info[2][2] = {
	    {
	        {
	            .sampler     = VK_NULL_HANDLE,
	            .imageView   = output_view[0],
	            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	        },
	        {
	            .sampler     = VK_NULL_HANDLE,
	            .imageView   = output_view[1],
	            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	        },
	    },
	    {
	        {
	            .sampler     = scene.linear_sampler,
	            .imageView   = output_view[0],
	            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        },
	        {
	            .sampler     = scene.linear_sampler,
	            .imageView   = output_view[1],
	            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        },
	    },
	};

	for (uint32_t i = 0; i < 2; i++)
	{
		VkWriteDescriptorSet writes[] = {
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_descriptor_sets[i],
		        .dstBinding       = 0,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		        .pImageInfo       = &output_info[0][i],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_descriptor_sets[i],
		        .dstBinding       = 1,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .pImageInfo       = &result_info,
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_descriptor_sets[i],
		        .dstBinding       = 2,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .pImageInfo       = &output_info[1][!i],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		};
		vkUpdateDescriptorSets(m_context->vk_device, 3, writes, 0, nullptr);
	}
}

void TAA::draw(VkCommandBuffer cmd_buffer, const Scene &scene, const GBufferPass &gbuffer_pass)
{
	m_context->begin_marker(cmd_buffer, "TAA");
	{
		VkDescriptorSet descriptor_sets[] = {
			scene.descriptor.set,
		    gbuffer_pass.descriptor.sets[m_context->ping_pong],
		    m_descriptor_sets[m_context->ping_pong],
		};
		vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline_layout, 0, 3, descriptor_sets, 0, nullptr);
		vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
		vkCmdPushConstants(cmd_buffer, m_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(m_push_constants), &m_push_constants);
		vkCmdDispatch(cmd_buffer, static_cast<uint32_t>(ceil(float(m_context->extent.width) / float(NUM_THREADS_X))), static_cast<uint32_t>(ceil(float(m_context->extent.height) / float(NUM_THREADS_Y))), 1);

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
			        .image               = output_image[m_context->ping_pong].vk_image,
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
			        .srcAccessMask       = VK_ACCESS_SHADER_READ_BIT,
			        .dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
			        .oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			        .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
			        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .image               = output_image[!m_context->ping_pong].vk_image,
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
	}
	m_context->end_marker(cmd_buffer);
}

bool TAA::draw_ui()
{
	return false;
}

void TAA::set_pathtracing(bool enable)
{
	m_is_pathtracing = enable;
}
