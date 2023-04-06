#include "render/pipeline/raytraced_ao.hpp"

#include "imgui.h"

static const int RAY_TRACE_NUM_THREADS_X = 8;
static const int RAY_TRACE_NUM_THREADS_Y = 8;

static const uint32_t TEMPORAL_ACCUMULATION_NUM_THREADS_X = 8;
static const uint32_t TEMPORAL_ACCUMULATION_NUM_THREADS_Y = 8;

static unsigned char g_raytraced_ao_comp_spv_data[] = {
#include "raytraced_ao.comp.spv.h"
};

RayTracedAO::RayTracedAO(const Context &context, RayTracedScale scale) :
    m_context(&context)
{
	float scale_divisor = powf(2.0f, float(scale));

	m_width  = static_cast<uint32_t>(static_cast<float>(context.extent.width) / scale_divisor);
	m_height = static_cast<uint32_t>(static_cast<float>(context.extent.height) / scale_divisor);

	m_gbuffer_mip = static_cast<uint32_t>(scale);

	// Ray Traced Pass
	{
		// Create shader module
		VkShaderModule shader = VK_NULL_HANDLE;
		{
			VkShaderModuleCreateInfo create_info = {
			    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			    .codeSize = sizeof(g_raytraced_ao_comp_spv_data),
			    .pCode    = reinterpret_cast<uint32_t *>(g_raytraced_ao_comp_spv_data),
			};
			vkCreateShaderModule(context.vk_device, &create_info, nullptr, &shader);
		}

		// Create descriptor set layout
		{
			VkDescriptorSetLayoutBinding bindings[] = {
			    // Global buffer
			    {
			        .binding         = 0,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Raytraced image
			    {
			        .binding         = 1,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // GBufferB
			    {
			        .binding         = 2,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Depth Stencil
			    {
			        .binding         = 3,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Sobol Sequence
			    {
			        .binding         = 4,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Scrambling Ranking Tile
			    {
			        .binding         = 5,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Top Levell Acceleration Structure
			    {
			        .binding         = 6,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			};
			VkDescriptorSetLayoutCreateInfo create_info = {
			    .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			    .flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
			    .bindingCount = 7,
			    .pBindings    = bindings,
			};
			vkCreateDescriptorSetLayout(m_context->vk_device, &create_info, nullptr, &m_raytraced.descriptor_set_layout);
		}

		// Allocate descriptor set
		{
			VkDescriptorSetAllocateInfo allocate_info = {
			    .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			    .pNext              = nullptr,
			    .descriptorPool     = m_context->vk_descriptor_pool,
			    .descriptorSetCount = 1,
			    .pSetLayouts        = &m_raytraced.descriptor_set_layout,
			};
			vkAllocateDescriptorSets(m_context->vk_device, &allocate_info, &m_raytraced.descriptor_set);
		}

		// Create pipeline layout
		{
			VkPushConstantRange range = {
			    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			    .offset     = 0,
			    .size       = sizeof(m_raytraced.push_constant),
			};
			VkPipelineLayoutCreateInfo create_info = {
			    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			    .setLayoutCount         = 1,
			    .pSetLayouts            = &m_raytraced.descriptor_set_layout,
			    .pushConstantRangeCount = 1,
			    .pPushConstantRanges    = &range,
			};
			vkCreatePipelineLayout(m_context->vk_device, &create_info, nullptr, &m_raytraced.pipeline_layout);
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
			    .layout             = m_raytraced.pipeline_layout,
			    .basePipelineHandle = VK_NULL_HANDLE,
			    .basePipelineIndex  = -1,
			};
			vkCreateComputePipelines(m_context->vk_device, m_context->vk_pipeline_cache, 1, &create_info, nullptr, &m_raytraced.pipeline);
		}
		vkDestroyShaderModule(m_context->vk_device, shader, nullptr);
	}

	// Create raytraced image
	{
		VkImageCreateInfo image_create_info = {
		    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		    .imageType     = VK_IMAGE_TYPE_2D,
		    .format        = VK_FORMAT_R32_UINT,
		    .extent        = VkExtent3D{static_cast<uint32_t>(ceil(float(m_width) / float(RAY_TRACE_NUM_THREADS_X))), static_cast<uint32_t>(ceil(float(m_height) / float(RAY_TRACE_NUM_THREADS_Y))), 1},
		    .mipLevels     = 1,
		    .arrayLayers   = 1,
		    .samples       = VK_SAMPLE_COUNT_1_BIT,
		    .tiling        = VK_IMAGE_TILING_OPTIMAL,
		    .usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		    .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
		    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};
		VmaAllocationCreateInfo allocation_create_info = {
		    .usage = VMA_MEMORY_USAGE_GPU_ONLY,
		};
		vmaCreateImage(context.vma_allocator, &image_create_info, &allocation_create_info, &raytraced_image.vk_image, &raytraced_image.vma_allocation, nullptr);
		VkImageViewCreateInfo view_create_info = {
		    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image            = raytraced_image.vk_image,
		    .viewType         = VK_IMAGE_VIEW_TYPE_2D,
		    .format           = VK_FORMAT_R32_UINT,
		    .components       = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
		    .subresourceRange = {
		        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		        .baseMipLevel   = 0,
		        .levelCount     = 1,
		        .baseArrayLayer = 0,
		        .layerCount     = 1,
		    },
		};
		vkCreateImageView(context.vk_device, &view_create_info, nullptr, &raytraced_image_view);
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) raytraced_image.vk_image, "RayTraceAO");
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) raytraced_image_view, "RayTraceAO View");
	}
}

RayTracedAO::~RayTracedAO()
{
	vkDestroyPipelineLayout(m_context->vk_device, m_raytraced.pipeline_layout, nullptr);
	vkDestroyPipeline(m_context->vk_device, m_raytraced.pipeline, nullptr);
	vkDestroyDescriptorSetLayout(m_context->vk_device, m_raytraced.descriptor_set_layout, nullptr);
	vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 1, &m_raytraced.descriptor_set);
	vkDestroyImageView(m_context->vk_device, raytraced_image_view, nullptr);
	vmaDestroyImage(m_context->vma_allocator, raytraced_image.vk_image, raytraced_image.vma_allocation);
}

void RayTracedAO::init(VkCommandBuffer cmd_buffer)
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
	        .image               = raytraced_image.vk_image,
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

void RayTracedAO::draw(VkCommandBuffer cmd_buffer)
{
	// Ray Traced
	m_context->begin_marker(cmd_buffer, "Ray Traced AO");
	{
		m_context->begin_marker(cmd_buffer, "Ray Traced");
		{
			m_raytraced.push_constant.gbuffer_mip = m_gbuffer_mip;

			vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_raytraced.pipeline_layout, 0, 1, &m_raytraced.descriptor_set, 0, nullptr);
			vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_raytraced.pipeline);
			vkCmdPushConstants(cmd_buffer, m_raytraced.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(m_raytraced.push_constant), &m_raytraced.push_constant);
			vkCmdDispatch(cmd_buffer, static_cast<uint32_t>(ceil(float(m_width) / float(RAY_TRACE_NUM_THREADS_X))), static_cast<uint32_t>(ceil(float(m_height) / float(RAY_TRACE_NUM_THREADS_Y))), 1);
		}
		m_context->end_marker(cmd_buffer);
	}
	m_context->end_marker(cmd_buffer);
}

void RayTracedAO::draw_ui()
{
	if (ImGui::TreeNode("Ray Traced AO"))
	{
		if (ImGui::TreeNode("Ray Traced"))
		{
			ImGui::SliderFloat("Ray Length", &m_raytraced.push_constant.ray_length, 0.0f, 10.0f);
			ImGui::DragFloat("Ray Traced Bias", &m_raytraced.push_constant.bias, 0.2f, 0.0f, 100.0f, "%.2f");
		}
		ImGui::TreePop();
	}
}

void RayTracedAO::update(const Scene &scene, const BlueNoise &blue_noise, VkImageView gbufferB, VkImageView depth_buffer)
{
	VkDescriptorBufferInfo global_buffer_info = {
	    .buffer = scene.global_buffer.vk_buffer,
	    .offset = 0,
	    .range  = sizeof(GlobalBuffer),
	};

	VkDescriptorImageInfo raytraced_image_info = {
	    .sampler     = VK_NULL_HANDLE,
	    .imageView   = raytraced_image_view,
	    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	};

	VkDescriptorImageInfo gbufferB_info = {
	    .sampler     = scene.default_sampler,
	    .imageView   = gbufferB,
	    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};

	VkDescriptorImageInfo depth_stencil_info = {
	    .sampler     = scene.default_sampler,
	    .imageView   = depth_buffer,
	    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};

	VkDescriptorImageInfo sobol_sequence_info = {
	    .sampler     = scene.default_sampler,
	    .imageView   = blue_noise.sobol_image_view,
	    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};

	VkDescriptorImageInfo scrambling_ranking_tile_info = {
	    .sampler     = scene.default_sampler,
	    .imageView   = blue_noise.scrambling_ranking_image_views[BLUE_NOISE_1SPP],
	    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};

	// Raytraced
	{
		VkWriteDescriptorSetAccelerationStructureKHR as_write = {
		    .sType                      = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
		    .accelerationStructureCount = 1,
		    .pAccelerationStructures    = &scene.tlas.vk_as,
		};
		VkWriteDescriptorSet writes[] = {
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_raytraced.descriptor_set,
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
		        .dstSet           = m_raytraced.descriptor_set,
		        .dstBinding       = 1,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		        .pImageInfo       = &raytraced_image_info,
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_raytraced.descriptor_set,
		        .dstBinding       = 2,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .pImageInfo       = &gbufferB_info,
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_raytraced.descriptor_set,
		        .dstBinding       = 3,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .pImageInfo       = &depth_stencil_info,
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_raytraced.descriptor_set,
		        .dstBinding       = 4,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .pImageInfo       = &sobol_sequence_info,
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_raytraced.descriptor_set,
		        .dstBinding       = 5,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .pImageInfo       = &scrambling_ranking_tile_info,
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .pNext            = &as_write,
		        .dstSet           = m_raytraced.descriptor_set,
		        .dstBinding       = 6,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
		        .pImageInfo       = nullptr,
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		};
		vkUpdateDescriptorSets(m_context->vk_device, 7, writes, 0, nullptr);
	}
}
