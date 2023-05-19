#include "render/pipeline/pathtracing.hpp"

#include <spdlog/fmt/fmt.h>

#include <imgui.h>

#define RAY_TRACE_NUM_THREADS_X 8
#define RAY_TRACE_NUM_THREADS_Y 8

static unsigned char g_path_tracing_comp_spv_data[] = {
#include "path_tracing.comp.spv.h"
};

PathTracing::PathTracing(const Context &context, const Scene &scene, const GBufferPass &gbuffer_pass) :
    m_context(&context)
{
	// Create pathtracing image
	{
		for (uint32_t i = 0; i < 2; i++)
		{
			VkImageCreateInfo image_create_info = {
			    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			    .imageType     = VK_IMAGE_TYPE_2D,
			    .format        = VK_FORMAT_R32G32B32A32_SFLOAT,
			    .extent        = VkExtent3D{static_cast<uint32_t>(m_context->renderExtent.width), static_cast<uint32_t>(m_context->renderExtent.height), 1},
			    .mipLevels     = 1,
			    .arrayLayers   = 1,
			    .samples       = VK_SAMPLE_COUNT_1_BIT,
			    .tiling        = VK_IMAGE_TILING_OPTIMAL,
			    .usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			    .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
			    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			};
			VmaAllocationCreateInfo allocation_create_info = {
			    .usage = VMA_MEMORY_USAGE_GPU_ONLY,
			};
			vmaCreateImage(context.vma_allocator, &image_create_info, &allocation_create_info, &path_tracing_image[i].vk_image, &path_tracing_image[i].vma_allocation, nullptr);
			VkImageViewCreateInfo view_create_info = {
			    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			    .image            = path_tracing_image[i].vk_image,
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
			vkCreateImageView(context.vk_device, &view_create_info, nullptr, &path_tracing_image_view[i]);
			m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) path_tracing_image[i].vk_image, fmt::format("Path Tracing Image - {}", i).c_str());
			m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) path_tracing_image_view[i], fmt::format("Path Tracing Image View - {}", i).c_str());
		}
	}

	// Create shader module
	VkShaderModule shader = VK_NULL_HANDLE;
	{
		VkShaderModuleCreateInfo create_info = {
		    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		    .codeSize = sizeof(g_path_tracing_comp_spv_data),
		    .pCode    = reinterpret_cast<uint32_t *>(g_path_tracing_comp_spv_data),
		};
		vkCreateShaderModule(context.vk_device, &create_info, nullptr, &shader);
	}

	// Create descriptor set layout
	{
		VkDescriptorSetLayoutBinding bindings[] = {
		    // Previous Path Tracing Image
		    {
		        .binding         = 0,
		        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		        .descriptorCount = 1,
		        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
		    },
		    // Path Tracing Image
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
		vkCreateDescriptorSetLayout(m_context->vk_device, &create_info, nullptr, &m_descriptor_set_layout);
	}

	// Allocate descriptor set
	{
		VkDescriptorSetLayout descriptor_set_layouts[] = {m_descriptor_set_layout, m_descriptor_set_layout};

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
		VkPushConstantRange range = {
		    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
		    .offset     = 0,
		    .size       = sizeof(m_push_constant),
		};
		VkDescriptorSetLayout descriptor_set_layouts[] = {
		    scene.descriptor.layout,
			gbuffer_pass.descriptor.layout,
		    m_descriptor_set_layout,
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

PathTracing::~PathTracing()
{
	vkDestroyPipelineLayout(m_context->vk_device, m_pipeline_layout, nullptr);
	vkDestroyPipeline(m_context->vk_device, m_pipeline, nullptr);
	vkDestroyDescriptorSetLayout(m_context->vk_device, m_descriptor_set_layout, nullptr);
	vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 2, m_descriptor_sets);
	for (uint32_t i = 0; i < 2; i++)
	{
		vkDestroyImageView(m_context->vk_device, path_tracing_image_view[i], nullptr);
		vmaDestroyImage(m_context->vma_allocator, path_tracing_image[i].vk_image, path_tracing_image[i].vma_allocation);
	}
}

void PathTracing::init(VkCommandBuffer cmd_buffer)
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
	        .image               = path_tracing_image[0].vk_image,
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
	        .image               = path_tracing_image[1].vk_image,
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

void PathTracing::update(const Scene &scene, const BlueNoise &blue_noise, const GBufferPass &gbuffer_pass)
{
	VkDescriptorImageInfo path_tracing_image_info[2] = {
	    {
	        .sampler     = VK_NULL_HANDLE,
	        .imageView   = path_tracing_image_view[0],
	        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	    },
	    {
	        .sampler     = VK_NULL_HANDLE,
	        .imageView   = path_tracing_image_view[1],
	        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	    },
	};

	VkDescriptorImageInfo sobol_sequence_info = {
	    .sampler     = scene.linear_sampler,
	    .imageView   = blue_noise.sobol_image_view,
	    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};

	VkDescriptorImageInfo scrambling_ranking_tile_info = {
	    .sampler     = scene.linear_sampler,
	    .imageView   = blue_noise.scrambling_ranking_image_views[BLUE_NOISE_1SPP],
	    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};

	{
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
			        .pImageInfo       = &path_tracing_image_info[!i],
			        .pBufferInfo      = nullptr,
			        .pTexelBufferView = nullptr,
			    },
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_descriptor_sets[i],
			        .dstBinding       = 1,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			        .pImageInfo       = &path_tracing_image_info[i],
			        .pBufferInfo      = nullptr,
			        .pTexelBufferView = nullptr,
			    },
			};
			vkUpdateDescriptorSets(m_context->vk_device, 2, writes, 0, nullptr);
		}
	}
}

void PathTracing::draw(VkCommandBuffer cmd_buffer, const Scene &scene, const GBufferPass &gbuffer_pass)
{
	{
		{
			VkImageMemoryBarrier image_barriers[] = {
			    {
			        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			        .srcAccessMask       = VK_ACCESS_SHADER_READ_BIT,
			        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
			        .oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			        .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
			        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .image               = path_tracing_image[!m_context->ping_pong].vk_image,
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
			        .image               = path_tracing_image[m_context->ping_pong].vk_image,
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

		m_context->begin_marker(cmd_buffer, "Path Tracing");
		{
			VkDescriptorSet descriptors[] = {
			    scene.descriptor.set,
			    gbuffer_pass.descriptor.sets[m_context->ping_pong],
			    m_descriptor_sets[m_context->ping_pong],
			};
			vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline_layout, 0, 3, descriptors, 0, nullptr);
			vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
			vkCmdPushConstants(cmd_buffer, m_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(m_push_constant), &m_push_constant);
			vkCmdDispatch(
				cmd_buffer,
				static_cast<uint32_t>(ceil(float(m_context->renderExtent.width) / float(RAY_TRACE_NUM_THREADS_X))),
				static_cast<uint32_t>(ceil(float(m_context->renderExtent.height) / float(RAY_TRACE_NUM_THREADS_Y))),
				1
			);
		}
		m_context->end_marker(cmd_buffer);

		{
			VkImageMemoryBarrier image_barriers[] = {
			    {
			        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			        .srcAccessMask       = VK_ACCESS_SHADER_READ_BIT,
			        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
			        .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
			        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .image               = path_tracing_image[!m_context->ping_pong].vk_image,
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
			        .image               = path_tracing_image[m_context->ping_pong].vk_image,
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
	m_push_constant.frame_count++;
}

bool PathTracing::draw_ui()
{
	bool update = false;
	if (ImGui::TreeNode("Path Tracing"))
	{
		ImGui::Text("Iteration: %d", m_push_constant.frame_count);
		update |= ImGui::SliderInt("Max Depth", reinterpret_cast<int32_t *>(&m_push_constant.max_depth), 1, 100);
		update |= ImGui::DragFloat("Bias", &m_push_constant.bias, 0.00001f, -1.f, 1.f, "%.10f");
	}
	return update;
}

void PathTracing::reset_frames()
{
	m_push_constant.frame_count = 0;
}
