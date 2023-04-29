#include "render/pipeline/tonemap.hpp"

#include <imgui.h>

#define NUM_THREADS_X 8
#define NUM_THREADS_Y 8

static unsigned char g_tonemap_comp_spv_data[] = {
#include "tonemap.comp.spv.h"
};

Tonemap::Tonemap(const Context &context) :
    m_context(&context)
{
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
		vmaCreateImage(context.vma_allocator, &image_create_info, &allocation_create_info, &tonemapped_image.vk_image, &tonemapped_image.vma_allocation, nullptr);
		VkImageViewCreateInfo view_create_info = {
		    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image            = tonemapped_image.vk_image,
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
		vkCreateImageView(context.vk_device, &view_create_info, nullptr, &tonemapped_image_view);
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) tonemapped_image.vk_image, "Tonemapped Image");
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) tonemapped_image_view, "Tonemapped Image View");
	}

	// Tonemapping pass
	{
		// Create shader module
		VkShaderModule shader = VK_NULL_HANDLE;
		{
			VkShaderModuleCreateInfo create_info = {
			    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			    .codeSize = sizeof(g_tonemap_comp_spv_data),
			    .pCode    = reinterpret_cast<uint32_t *>(g_tonemap_comp_spv_data),
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
			};
			VkDescriptorSetLayoutCreateInfo create_info = {
			    .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			    .flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
			    .bindingCount = 2,
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
			vkAllocateDescriptorSets(m_context->vk_device, &allocate_info, m_pt_descriptor_sets);
			allocate_info.descriptorSetCount = 1;
			vkAllocateDescriptorSets(m_context->vk_device, &allocate_info, &m_hybrid_descriptor_set);
		}

		// Create pipeline layout
		{
			VkPushConstantRange range = {
			    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			    .offset     = 0,
			    .size       = sizeof(m_push_constants),
			};
			VkPipelineLayoutCreateInfo create_info = {
			    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			    .setLayoutCount         = 1,
			    .pSetLayouts            = &m_descriptor_set_layout,
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

Tonemap::~Tonemap()
{
	vkDestroyPipelineLayout(m_context->vk_device, m_pipeline_layout, nullptr);
	vkDestroyPipeline(m_context->vk_device, m_pipeline, nullptr);
	vkDestroyDescriptorSetLayout(m_context->vk_device, m_descriptor_set_layout, nullptr);
	vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 2, m_pt_descriptor_sets);
	vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 1, &m_hybrid_descriptor_set);
	vkDestroyImageView(m_context->vk_device, tonemapped_image_view, nullptr);
	vmaDestroyImage(m_context->vma_allocator, tonemapped_image.vk_image, tonemapped_image.vma_allocation);
}

void Tonemap::init(VkCommandBuffer cmd_buffer)
{
	VkImageMemoryBarrier image_barrier = {
	    .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask       = 0,
	    .dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
	    .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
	    .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
	    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .image               = tonemapped_image.vk_image,
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
	    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	    0, 0, nullptr, 0, nullptr, 1, &image_barrier);
}

void Tonemap::update(const Scene &scene, VkImageView pt_result[2], VkImageView hybrid_result)
{
	VkDescriptorImageInfo pt_result_info[] = {
	    {
	        .sampler     = scene.linear_sampler,
	        .imageView   = pt_result[0],
	        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    },
	    {
	        .sampler     = scene.linear_sampler,
	        .imageView   = pt_result[1],
	        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    },
	};

	VkDescriptorImageInfo hybrid_result_info = {
	    .sampler     = scene.linear_sampler,
	    .imageView   = hybrid_result,
	    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};

	VkDescriptorImageInfo tonemap_info = {
	    .sampler     = VK_NULL_HANDLE,
	    .imageView   = tonemapped_image_view,
	    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	};

	for (uint32_t i = 0; i < 2; i++)
	{
		VkWriteDescriptorSet writes[] = {
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_pt_descriptor_sets[i],
		        .dstBinding       = 0,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		        .pImageInfo       = &tonemap_info,
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_pt_descriptor_sets[i],
		        .dstBinding       = 1,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .pImageInfo       = &pt_result_info[i],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		};
		vkUpdateDescriptorSets(m_context->vk_device, 2, writes, 0, nullptr);
	}

	{
		VkWriteDescriptorSet writes[] = {
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_hybrid_descriptor_set,
		        .dstBinding       = 0,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		        .pImageInfo       = &tonemap_info,
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_hybrid_descriptor_set,
		        .dstBinding       = 1,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .pImageInfo       = &hybrid_result_info,
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		};
		vkUpdateDescriptorSets(m_context->vk_device, 2, writes, 0, nullptr);
	}
}

void Tonemap::draw(VkCommandBuffer cmd_buffer)
{
	m_context->begin_marker(cmd_buffer, "Tone Mapping");
	{
		if (m_is_pathtracing)
		{
			vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline_layout, 0, 1, &m_pt_descriptor_sets[m_context->ping_pong], 0, nullptr);
		}
		else
		{
			vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline_layout, 0, 1, &m_hybrid_descriptor_set, 0, nullptr);
		}
		vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
		vkCmdPushConstants(cmd_buffer, m_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(m_push_constants), &m_push_constants);
		vkCmdDispatch(cmd_buffer, static_cast<uint32_t>(ceil(float(m_context->extent.width) / float(NUM_THREADS_X))), static_cast<uint32_t>(ceil(float(m_context->extent.height) / float(NUM_THREADS_Y))), 1);
	}
	m_context->end_marker(cmd_buffer);
}

bool Tonemap::draw_ui()
{
	bool update = false;
	if (ImGui::TreeNode("Tonemapping"))
	{
		update |= ImGui::SliderFloat("Exposure", &m_push_constants.avg_lum, 0.001f, 5.0f);
		update |= ImGui::SliderFloat("Brightness", &m_push_constants.brightness, 0.0f, 2.0f);
		update |= ImGui::SliderFloat("Contrast", &m_push_constants.contrast, 0.0f, 2.0f);
		update |= ImGui::SliderFloat("Saturation", &m_push_constants.saturation, 0.0f, 5.0f);
		update |= ImGui::SliderFloat("Vignette", &m_push_constants.vignette, 0.0f, 2.0f);
		ImGui::TreePop();
	}
	return update;
}

void Tonemap::set_pathtracing(bool enable)
{
	m_is_pathtracing = enable;
}
