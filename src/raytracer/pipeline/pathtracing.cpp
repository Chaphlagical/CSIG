#include "render/pipeline/pathtracing.hpp"

#include <spdlog/fmt/fmt.h>

#include <imgui.h>

#define RAY_TRACE_NUM_THREADS_X 8
#define RAY_TRACE_NUM_THREADS_Y 8

static unsigned char g_path_tracing_comp_spv_data[] = {
#include "path_tracing.comp.spv.h"
};

PathTracing::PathTracing(const Context &context) :
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
			    .extent        = VkExtent3D{static_cast<uint32_t>(m_context->extent.width), static_cast<uint32_t>(m_context->extent.height), 1},
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
		VkDescriptorBindingFlags descriptor_binding_flags[] = {
		    0,
		    0,
		    0,
		    0,
		    0,
		    0,
		    0,
		    0,
		    0,
		    0,
		    0,
		    0,
		    0,
		    0,
		    0,
		    0,
		    VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
		};
		VkDescriptorSetLayoutBinding bindings[] = {
		    // Global buffer
		    {
		        .binding         = 0,
		        .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		        .descriptorCount = 1,
		        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
		    },
		    // GBufferA
		    {
		        .binding         = 1,
		        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
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
		    // GBufferC
		    {
		        .binding         = 3,
		        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .descriptorCount = 1,
		        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
		    },
		    // Depth Stencil
		    {
		        .binding         = 4,
		        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .descriptorCount = 1,
		        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
		    },
		    // Sobol Sequence
		    {
		        .binding         = 5,
		        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .descriptorCount = 1,
		        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
		    },
		    // Scrambling Ranking Tile
		    {
		        .binding         = 6,
		        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .descriptorCount = 1,
		        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
		    },
		    // Top Levell Acceleration Structure
		    {
		        .binding         = 7,
		        .descriptorType  = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
		        .descriptorCount = 1,
		        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
		    },
		    // Previous Path Tracing Image
		    {
		        .binding         = 8,
		        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		        .descriptorCount = 1,
		        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
		    },
		    // Path Tracing Image
		    {
		        .binding         = 9,
		        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		        .descriptorCount = 1,
		        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
		    },
		    // Vertex Buffer
		    {
		        .binding         = 10,
		        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		        .descriptorCount = 1,
		        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
		    },
		    // Index Buffer
		    {
		        .binding         = 11,
		        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		        .descriptorCount = 1,
		        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
		    },
		    // Material Buffer
		    {
		        .binding         = 12,
		        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		        .descriptorCount = 1,
		        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
		    },
		    // Emitter Buffer
		    {
		        .binding         = 13,
		        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		        .descriptorCount = 1,
		        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
		    },
		    // Scene Buffer
		    {
		        .binding         = 14,
		        .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		        .descriptorCount = 1,
		        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
		    },
		    // Instance Buffer
		    {
		        .binding         = 15,
		        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		        .descriptorCount = 1,
		        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
		    },
		    // Bindless textures
		    {
		        .binding         = 16,
		        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .descriptorCount = 1024,
		        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
		    },
		};
		VkDescriptorSetLayoutBindingFlagsCreateInfo descriptor_set_layout_binding_flag_create_info = {
		    .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
		    .bindingCount  = 17,
		    .pBindingFlags = descriptor_binding_flags,
		};
		VkDescriptorSetLayoutCreateInfo create_info = {
		    .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		    .pNext        = &descriptor_set_layout_binding_flag_create_info,
		    .flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
		    .bindingCount = 17,
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
	        .dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
	        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
	        .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
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
	    {
	        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .srcAccessMask       = 0,
	        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
	        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
	        .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
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
	};
	vkCmdPipelineBarrier(
	    cmd_buffer,
	    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	    0, 0, nullptr, 0, nullptr, 2, image_barriers);
}

void PathTracing::update(const Scene &scene, const BlueNoise &blue_noise, const GBufferPass &gbuffer_pass)
{
	VkDescriptorBufferInfo global_buffer_info = {
	    .buffer = scene.global_buffer.vk_buffer,
	    .offset = 0,
	    .range  = sizeof(GlobalBuffer),
	};

	VkDescriptorBufferInfo vertex_buffer_info = {
	    .buffer = scene.vertex_buffer.vk_buffer,
	    .offset = 0,
	    .range  = sizeof(Vertex) * scene.scene_info.vertices_count,
	};

	VkDescriptorBufferInfo index_buffer_info = {
	    .buffer = scene.index_buffer.vk_buffer,
	    .offset = 0,
	    .range  = sizeof(uint32_t) * scene.scene_info.indices_count,
	};

	VkDescriptorBufferInfo material_buffer_info = {
	    .buffer = scene.material_buffer.vk_buffer,
	    .offset = 0,
	    .range  = sizeof(Material) * scene.scene_info.material_count,
	};

	VkDescriptorBufferInfo emitter_buffer_info = {
	    .buffer = scene.emitter_buffer.vk_buffer,
	    .offset = 0,
	    .range  = VK_WHOLE_SIZE,
	};

	VkDescriptorBufferInfo scene_buffer_info = {
	    .buffer = scene.scene_buffer.vk_buffer,
	    .offset = 0,
	    .range  = sizeof(scene.scene_info),
	};

	VkDescriptorBufferInfo instance_buffer_info = {
	    .buffer = scene.instance_buffer.vk_buffer,
	    .offset = 0,
	    .range  = sizeof(Instance) * scene.scene_info.instance_count,
	};

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

	VkDescriptorImageInfo gbufferA_info[] = {
	    {
	        .sampler     = scene.linear_sampler,
	        .imageView   = gbuffer_pass.gbufferA_view[0],
	        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    },
	    {
	        .sampler     = scene.linear_sampler,
	        .imageView   = gbuffer_pass.gbufferA_view[1],
	        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    },
	};

	VkDescriptorImageInfo gbufferB_info[] = {
	    {
	        .sampler     = scene.linear_sampler,
	        .imageView   = gbuffer_pass.gbufferB_view[0],
	        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    },
	    {
	        .sampler     = scene.linear_sampler,
	        .imageView   = gbuffer_pass.gbufferB_view[1],
	        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    },
	};

	VkDescriptorImageInfo gbufferC_info[] = {
	    {
	        .sampler     = scene.linear_sampler,
	        .imageView   = gbuffer_pass.gbufferC_view[0],
	        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    },
	    {
	        .sampler     = scene.linear_sampler,
	        .imageView   = gbuffer_pass.gbufferC_view[1],
	        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    },
	};

	VkDescriptorImageInfo depth_stencil_info[] = {
	    {
	        .sampler     = scene.linear_sampler,
	        .imageView   = gbuffer_pass.depth_buffer_view[0],
	        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    },
	    {
	        .sampler     = scene.linear_sampler,
	        .imageView   = gbuffer_pass.depth_buffer_view[1],
	        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
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

	std::vector<VkDescriptorImageInfo> texture_infos;
	texture_infos.reserve(scene.textures.size());
	for (auto &view : scene.texture_views)
	{
		texture_infos.push_back(VkDescriptorImageInfo{
		    .sampler     = scene.linear_sampler,
		    .imageView   = view,
		    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		});
	}

	VkWriteDescriptorSetAccelerationStructureKHR as_write = {
	    .sType                      = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
	    .accelerationStructureCount = 1,
	    .pAccelerationStructures    = &scene.tlas.vk_as,
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
			        .descriptorType   = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			        .pImageInfo       = nullptr,
			        .pBufferInfo      = &global_buffer_info,
			        .pTexelBufferView = nullptr,
			    },
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_descriptor_sets[i],
			        .dstBinding       = 1,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .pImageInfo       = &gbufferA_info[i],
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
			        .pImageInfo       = &gbufferB_info[i],
			        .pBufferInfo      = nullptr,
			        .pTexelBufferView = nullptr,
			    },
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_descriptor_sets[i],
			        .dstBinding       = 3,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .pImageInfo       = &gbufferC_info[i],
			        .pBufferInfo      = nullptr,
			        .pTexelBufferView = nullptr,
			    },
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_descriptor_sets[i],
			        .dstBinding       = 4,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .pImageInfo       = &depth_stencil_info[i],
			        .pBufferInfo      = nullptr,
			        .pTexelBufferView = nullptr,
			    },
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_descriptor_sets[i],
			        .dstBinding       = 5,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .pImageInfo       = &sobol_sequence_info,
			        .pBufferInfo      = nullptr,
			        .pTexelBufferView = nullptr,
			    },
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_descriptor_sets[i],
			        .dstBinding       = 6,
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
			        .dstSet           = m_descriptor_sets[i],
			        .dstBinding       = 7,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
			        .pImageInfo       = nullptr,
			        .pBufferInfo      = nullptr,
			        .pTexelBufferView = nullptr,
			    },
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_descriptor_sets[i],
			        .dstBinding       = 8,
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
			        .dstBinding       = 9,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			        .pImageInfo       = &path_tracing_image_info[i],
			        .pBufferInfo      = nullptr,
			        .pTexelBufferView = nullptr,
			    },
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_descriptor_sets[i],
			        .dstBinding       = 10,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			        .pImageInfo       = nullptr,
			        .pBufferInfo      = &vertex_buffer_info,
			        .pTexelBufferView = nullptr,
			    },
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_descriptor_sets[i],
			        .dstBinding       = 11,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			        .pImageInfo       = nullptr,
			        .pBufferInfo      = &index_buffer_info,
			        .pTexelBufferView = nullptr,
			    },
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_descriptor_sets[i],
			        .dstBinding       = 12,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			        .pImageInfo       = nullptr,
			        .pBufferInfo      = &material_buffer_info,
			        .pTexelBufferView = nullptr,
			    },
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_descriptor_sets[i],
			        .dstBinding       = 13,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			        .pImageInfo       = nullptr,
			        .pBufferInfo      = &emitter_buffer_info,
			        .pTexelBufferView = nullptr,
			    },
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_descriptor_sets[i],
			        .dstBinding       = 14,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			        .pImageInfo       = nullptr,
			        .pBufferInfo      = &scene_buffer_info,
			        .pTexelBufferView = nullptr,
			    },
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_descriptor_sets[i],
			        .dstBinding       = 15,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			        .pImageInfo       = nullptr,
			        .pBufferInfo      = &instance_buffer_info,
			        .pTexelBufferView = nullptr,
			    },
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_descriptor_sets[i],
			        .dstBinding       = 16,
			        .dstArrayElement  = 0,
			        .descriptorCount  = static_cast<uint32_t>(texture_infos.size()),
			        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .pImageInfo       = texture_infos.data(),
			        .pBufferInfo      = nullptr,
			        .pTexelBufferView = nullptr,
			    },
			};
			vkUpdateDescriptorSets(m_context->vk_device, 17, writes, 0, nullptr);
		}
	}
}

void PathTracing::draw(VkCommandBuffer cmd_buffer)
{
	{
		{
			VkImageMemoryBarrier image_barriers[] = {
			    {
			        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			        .srcAccessMask       = VK_ACCESS_SHADER_READ_BIT,
			        .dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
			        .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
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
			    {
			        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			        .srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
			        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
			        .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
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
			};
			vkCmdPipelineBarrier(
			    cmd_buffer,
			    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			    0, 0, nullptr, 0, nullptr, 2, image_barriers);
		}

		m_context->begin_marker(cmd_buffer, "Path Tracing");
		{
			vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline_layout, 0, 1, &m_descriptor_sets[m_context->ping_pong], 0, nullptr);
			vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
			vkCmdPushConstants(cmd_buffer, m_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(m_push_constant), &m_push_constant);
			vkCmdDispatch(cmd_buffer, static_cast<uint32_t>(ceil(float(m_context->extent.width) / float(RAY_TRACE_NUM_THREADS_X))), static_cast<uint32_t>(ceil(float(m_context->extent.height) / float(RAY_TRACE_NUM_THREADS_Y))), 1);
		}
		m_context->end_marker(cmd_buffer);
	}
	m_push_constant.frame_count++;
}

bool PathTracing::draw_ui()
{
	bool update = false;
	if (ImGui::TreeNode("Path Tracing"))
	{
		update |= ImGui::SliderInt("Max Depth", reinterpret_cast<int32_t *>(&m_push_constant.max_depth), 1, 100);
		update |= ImGui::DragFloat("Emitter Scale", &m_push_constant.emitter_scale, 1.f, 1.f, 1000.f);
		update |= ImGui::DragFloat("Bias", &m_push_constant.bias, 0.0000000001f, -1.f, 1.f, "%.10f");
	}
	return update;
}

void PathTracing::reset_frames()
{
	m_push_constant.frame_count = 0;
}
