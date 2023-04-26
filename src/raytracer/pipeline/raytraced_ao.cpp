#include "render/pipeline/raytraced_ao.hpp"

#include <imgui.h>
#include <spdlog/fmt/fmt.h>

static const int RAY_TRACE_NUM_THREADS_X = 8;
static const int RAY_TRACE_NUM_THREADS_Y = 8;

static const uint32_t TEMPORAL_ACCUMULATION_NUM_THREADS_X = 8;
static const uint32_t TEMPORAL_ACCUMULATION_NUM_THREADS_Y = 8;

static unsigned char g_ao_raytraced_comp_spv_data[] = {
#include "ao_raytraced.comp.spv.h"
};

static unsigned char g_ao_temporal_accumulation_comp_spv_data[] = {
#include "ao_temporal_accumulation.comp.spv.h"
};

RayTracedAO::RayTracedAO(const Context &context, RayTracedScale scale) :
    m_context(&context)
{
	float scale_divisor = powf(2.0f, float(scale));

	m_width  = static_cast<uint32_t>(static_cast<float>(context.extent.width) / scale_divisor);
	m_height = static_cast<uint32_t>(static_cast<float>(context.extent.height) / scale_divisor);

	m_gbuffer_mip = static_cast<uint32_t>(scale);

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
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) raytraced_image.vk_image, "RayTraced Image");
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) raytraced_image_view, "RayTraced Image View");
	}

	// Create AO image & history length image
	for (uint32_t i = 0; i < 2; i++)
	{
		VkImageCreateInfo image_create_info = {
		    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		    .imageType     = VK_IMAGE_TYPE_2D,
		    .format        = VK_FORMAT_R32_SFLOAT,
		    .extent        = VkExtent3D{m_width, m_height, 1},
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
		vmaCreateImage(context.vma_allocator, &image_create_info, &allocation_create_info, &ao_image[i].vk_image, &ao_image[i].vma_allocation, nullptr);
		vmaCreateImage(context.vma_allocator, &image_create_info, &allocation_create_info, &history_length_image[i].vk_image, &history_length_image[i].vma_allocation, nullptr);
		VkImageViewCreateInfo view_create_info = {
		    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image            = ao_image[i].vk_image,
		    .viewType         = VK_IMAGE_VIEW_TYPE_2D,
		    .format           = VK_FORMAT_R32_SFLOAT,
		    .components       = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
		    .subresourceRange = {
		        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		        .baseMipLevel   = 0,
		        .levelCount     = 1,
		        .baseArrayLayer = 0,
		        .layerCount     = 1,
		    },
		};
		vkCreateImageView(context.vk_device, &view_create_info, nullptr, &ao_image_view[i]);
		view_create_info.image = history_length_image[i].vk_image;
		vkCreateImageView(context.vk_device, &view_create_info, nullptr, &history_length_image_view[i]);
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) ao_image[i].vk_image, fmt::format("AO Image - {}", i).c_str());
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) ao_image_view[i], fmt::format("AO Image View - {}", i).c_str());
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) history_length_image[i].vk_image, fmt::format("History Length Image - {}", i).c_str());
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) history_length_image_view[i], fmt::format("History Length Image View - {}", i).c_str());
	}

	// Create upsampling ao image
	{
		VkImageCreateInfo image_create_info = {
		    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		    .imageType     = VK_IMAGE_TYPE_2D,
		    .format        = VK_FORMAT_R32_SFLOAT,
		    .extent        = VkExtent3D{m_width, m_height, 1},
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
		vmaCreateImage(context.vma_allocator, &image_create_info, &allocation_create_info, &upsampled_ao_image.vk_image, &upsampled_ao_image.vma_allocation, nullptr);
		VkImageViewCreateInfo view_create_info = {
		    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image            = upsampled_ao_image.vk_image,
		    .viewType         = VK_IMAGE_VIEW_TYPE_2D,
		    .format           = VK_FORMAT_R32_SFLOAT,
		    .components       = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
		    .subresourceRange = {
		        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		        .baseMipLevel   = 0,
		        .levelCount     = 1,
		        .baseArrayLayer = 0,
		        .layerCount     = 1,
		    },
		};
		vkCreateImageView(context.vk_device, &view_create_info, nullptr, &upsampled_ao_image_view);
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) upsampled_ao_image.vk_image, "Upsampled AO Image");
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) upsampled_ao_image_view, "Upsampled AO Image View");
	}

	// Create denoise tile buffer
	{
		VkBufferCreateInfo buffer_create_info = {
		    .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		    .size        = sizeof(glm::ivec2) * static_cast<uint32_t>(ceil(float(m_width) / float(TEMPORAL_ACCUMULATION_NUM_THREADS_X))) * static_cast<uint32_t>(ceil(float(m_height) / float(TEMPORAL_ACCUMULATION_NUM_THREADS_Y))),
		    .usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};
		VmaAllocationCreateInfo allocation_create_info = {
		    .usage = VMA_MEMORY_USAGE_GPU_ONLY,
		};
		VmaAllocationInfo allocation_info = {};
		vmaCreateBuffer(m_context->vma_allocator, &buffer_create_info, &allocation_create_info, &denoise_tile_buffer.vk_buffer, &denoise_tile_buffer.vma_allocation, &allocation_info);
	}

	// Create denoise tile dispatch argument buffer
	{
		VkBufferCreateInfo buffer_create_info = {
		    .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		    .size        = sizeof(VkDispatchIndirectCommand),
		    .usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
		    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};
		VmaAllocationCreateInfo allocation_create_info = {
		    .usage = VMA_MEMORY_USAGE_GPU_ONLY,
		};
		VmaAllocationInfo allocation_info = {};
		vmaCreateBuffer(m_context->vma_allocator, &buffer_create_info, &allocation_create_info, &denoise_tile_dispatch_args_buffer.vk_buffer, &denoise_tile_dispatch_args_buffer.vma_allocation, &allocation_info);
	}

	// Ray Traced Pass
	{
		// Create shader module
		VkShaderModule shader = VK_NULL_HANDLE;
		{
			VkShaderModuleCreateInfo create_info = {
			    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			    .codeSize = sizeof(g_ao_raytraced_comp_spv_data),
			    .pCode    = reinterpret_cast<uint32_t *>(g_ao_raytraced_comp_spv_data),
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
			VkDescriptorSetLayout descriptor_set_layouts[] = {m_raytraced.descriptor_set_layout, m_raytraced.descriptor_set_layout};

			VkDescriptorSetAllocateInfo allocate_info = {
			    .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			    .pNext              = nullptr,
			    .descriptorPool     = m_context->vk_descriptor_pool,
			    .descriptorSetCount = 2,
			    .pSetLayouts        = descriptor_set_layouts,
			};
			vkAllocateDescriptorSets(m_context->vk_device, &allocate_info, m_raytraced.descriptor_sets);
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
			vkDestroyShaderModule(m_context->vk_device, shader, nullptr);
		}
	}

	// Create temporal accumulation pass
	{
		// Create shader module
		VkShaderModule shader = VK_NULL_HANDLE;
		{
			VkShaderModuleCreateInfo create_info = {
			    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			    .codeSize = sizeof(g_ao_temporal_accumulation_comp_spv_data),
			    .pCode    = reinterpret_cast<uint32_t *>(g_ao_temporal_accumulation_comp_spv_data),
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
			    // GBufferB
			    {
			        .binding         = 1,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // GBufferC
			    {
			        .binding         = 2,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Depth Buffer
			    {
			        .binding         = 3,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Prev GBufferB
			    {
			        .binding         = 4,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Prev GBufferC
			    {
			        .binding         = 5,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Prev Depth Buffer
			    {
			        .binding         = 6,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Ray Traced Image
			    {
			        .binding         = 7,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // AO Image
			    {
			        .binding         = 8,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // History Length Image
			    {
			        .binding         = 9,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Prev AO Image
			    {
			        .binding         = 10,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Prev History Length Image
			    {
			        .binding         = 11,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Denoise Tile Data
			    {
			        .binding         = 12,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Denoise Tile Dispatch Argument
			    {
			        .binding         = 13,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			};
			VkDescriptorSetLayoutCreateInfo create_info = {
			    .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			    .flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
			    .bindingCount = 14,
			    .pBindings    = bindings,
			};
			vkCreateDescriptorSetLayout(m_context->vk_device, &create_info, nullptr, &m_denoise.temporal_accumulation.descriptor_set_layout);
		}

		// Allocate descriptor set
		{
			VkDescriptorSetLayout descriptor_set_layouts[] = {m_denoise.temporal_accumulation.descriptor_set_layout, m_denoise.temporal_accumulation.descriptor_set_layout};

			VkDescriptorSetAllocateInfo allocate_info = {
			    .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			    .pNext              = nullptr,
			    .descriptorPool     = m_context->vk_descriptor_pool,
			    .descriptorSetCount = 2,
			    .pSetLayouts        = descriptor_set_layouts,
			};
			vkAllocateDescriptorSets(m_context->vk_device, &allocate_info, m_denoise.temporal_accumulation.descriptor_sets);
		}

		// Create pipeline layout
		{
			VkPushConstantRange range = {
			    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			    .offset     = 0,
			    .size       = sizeof(m_denoise.temporal_accumulation.push_constant),
			};
			VkPipelineLayoutCreateInfo create_info = {
			    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			    .setLayoutCount         = 1,
			    .pSetLayouts            = &m_denoise.temporal_accumulation.descriptor_set_layout,
			    .pushConstantRangeCount = 1,
			    .pPushConstantRanges    = &range,
			};
			vkCreatePipelineLayout(m_context->vk_device, &create_info, nullptr, &m_denoise.temporal_accumulation.pipeline_layout);
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
			    .layout             = m_denoise.temporal_accumulation.pipeline_layout,
			    .basePipelineHandle = VK_NULL_HANDLE,
			    .basePipelineIndex  = -1,
			};
			vkCreateComputePipelines(m_context->vk_device, m_context->vk_pipeline_cache, 1, &create_info, nullptr, &m_denoise.temporal_accumulation.pipeline);
			vkDestroyShaderModule(m_context->vk_device, shader, nullptr);
		}
	}
}

RayTracedAO::~RayTracedAO()
{
	vkDestroyPipelineLayout(m_context->vk_device, m_raytraced.pipeline_layout, nullptr);
	vkDestroyPipelineLayout(m_context->vk_device, m_denoise.temporal_accumulation.pipeline_layout, nullptr);
	vkDestroyPipeline(m_context->vk_device, m_raytraced.pipeline, nullptr);
	vkDestroyPipeline(m_context->vk_device, m_denoise.temporal_accumulation.pipeline, nullptr);
	vkDestroyDescriptorSetLayout(m_context->vk_device, m_raytraced.descriptor_set_layout, nullptr);
	vkDestroyDescriptorSetLayout(m_context->vk_device, m_denoise.temporal_accumulation.descriptor_set_layout, nullptr);
	vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 2, m_raytraced.descriptor_sets);
	vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 2, m_denoise.temporal_accumulation.descriptor_sets);
	vmaDestroyBuffer(m_context->vma_allocator, denoise_tile_buffer.vk_buffer, denoise_tile_buffer.vma_allocation);
	vmaDestroyBuffer(m_context->vma_allocator, denoise_tile_dispatch_args_buffer.vk_buffer, denoise_tile_dispatch_args_buffer.vma_allocation);
	vkDestroyImageView(m_context->vk_device, raytraced_image_view, nullptr);
	vkDestroyImageView(m_context->vk_device, upsampled_ao_image_view, nullptr);
	vmaDestroyImage(m_context->vma_allocator, raytraced_image.vk_image, raytraced_image.vma_allocation);
	vmaDestroyImage(m_context->vma_allocator, upsampled_ao_image.vk_image, upsampled_ao_image.vma_allocation);
	for (uint32_t i = 0; i < 2; i++)
	{
		vkDestroyImageView(m_context->vk_device, ao_image_view[i], nullptr);
		vkDestroyImageView(m_context->vk_device, history_length_image_view[i], nullptr);
		vmaDestroyImage(m_context->vma_allocator, ao_image[i].vk_image, ao_image[i].vma_allocation);
		vmaDestroyImage(m_context->vma_allocator, history_length_image[i].vk_image, history_length_image[i].vma_allocation);
	}
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
	    {
	        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .srcAccessMask       = 0,
	        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
	        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
	        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image               = ao_image[0].vk_image,
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
	        .dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
	        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
	        .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image               = ao_image[1].vk_image,
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
	        .image               = history_length_image[0].vk_image,
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
	        .dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
	        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
	        .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image               = history_length_image[1].vk_image,
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
	        .dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
	        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
	        .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image               = upsampled_ao_image.vk_image,
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
	    0, 0, nullptr, 0, nullptr, 6, image_barriers);
}

void RayTracedAO::update(const Scene &scene, const BlueNoise &blue_noise, const GBufferPass &gbuffer_pass)
{
	VkDescriptorBufferInfo global_buffer_info = {
	    .buffer = scene.global_buffer.vk_buffer,
	    .offset = 0,
	    .range  = sizeof(GlobalBuffer),
	};

	VkDescriptorBufferInfo denoise_tile_buffer_info = {
	    .buffer = denoise_tile_buffer.vk_buffer,
	    .offset = 0,
	    .range  = sizeof(glm::ivec2) * static_cast<uint32_t>(ceil(float(m_width) / float(TEMPORAL_ACCUMULATION_NUM_THREADS_X))) * static_cast<uint32_t>(ceil(float(m_height) / float(TEMPORAL_ACCUMULATION_NUM_THREADS_Y))),
	};

	VkDescriptorBufferInfo denoise_tile_dispatch_args_buffer_info = {
	    .buffer = denoise_tile_dispatch_args_buffer.vk_buffer,
	    .offset = 0,
	    .range  = sizeof(VkDispatchIndirectCommand),
	};

	VkDescriptorImageInfo raytraced_image_info[] = {
	    {
	        .sampler     = VK_NULL_HANDLE,
	        .imageView   = raytraced_image_view,
	        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	    },
	    {
	        .sampler     = scene.nearest_sampler,
	        .imageView   = raytraced_image_view,
	        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    },
	};

	VkDescriptorImageInfo ao_image_info[2][2] = {
	    {
	        {
	            .sampler     = VK_NULL_HANDLE,
	            .imageView   = ao_image_view[0],
	            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	        },
	        {
	            .sampler     = scene.linear_sampler,
	            .imageView   = ao_image_view[1],
	            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        },
	    },
	    {
	        {
	            .sampler     = VK_NULL_HANDLE,
	            .imageView   = ao_image_view[1],
	            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	        },
	        {
	            .sampler     = scene.linear_sampler,
	            .imageView   = ao_image_view[0],
	            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        },
	    },
	};

	VkDescriptorImageInfo history_length_image_info[2][2] = {
	    {
	        {
	            .sampler     = VK_NULL_HANDLE,
	            .imageView   = history_length_image_view[0],
	            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	        },
	        {
	            .sampler     = scene.linear_sampler,
	            .imageView   = history_length_image_view[1],
	            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        },
	    },
	    {
	        {
	            .sampler     = VK_NULL_HANDLE,
	            .imageView   = history_length_image_view[1],
	            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	        },
	        {
	            .sampler     = scene.linear_sampler,
	            .imageView   = history_length_image_view[0],
	            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        },
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

	VkWriteDescriptorSetAccelerationStructureKHR as_write = {
	    .sType                      = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
	    .accelerationStructureCount = 1,
	    .pAccelerationStructures    = &scene.tlas.vk_as,
	};

	// Raytraced
	{
		for (uint32_t i = 0; i < 2; i++)
		{
			VkWriteDescriptorSet writes[] = {
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_raytraced.descriptor_sets[i],
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
			        .dstSet           = m_raytraced.descriptor_sets[i],
			        .dstBinding       = 1,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			        .pImageInfo       = &raytraced_image_info[0],
			        .pBufferInfo      = nullptr,
			        .pTexelBufferView = nullptr,
			    },
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_raytraced.descriptor_sets[i],
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
			        .dstSet           = m_raytraced.descriptor_sets[i],
			        .dstBinding       = 3,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .pImageInfo       = &depth_stencil_info[i],
			        .pBufferInfo      = nullptr,
			        .pTexelBufferView = nullptr,
			    },
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_raytraced.descriptor_sets[i],
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
			        .dstSet           = m_raytraced.descriptor_sets[i],
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
			        .dstSet           = m_raytraced.descriptor_sets[i],
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

	// Temporal accumulation
	{
		for (uint32_t i = 0; i < 2; i++)
		{
			VkWriteDescriptorSet writes[] = {
			    // Binding = 0: Global Buffer
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_denoise.temporal_accumulation.descriptor_sets[i],
			        .dstBinding       = 0,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			        .pImageInfo       = nullptr,
			        .pBufferInfo      = &global_buffer_info,
			        .pTexelBufferView = nullptr,
			    },
			    // Binding = 1: GBufferB
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_denoise.temporal_accumulation.descriptor_sets[i],
			        .dstBinding       = 1,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .pImageInfo       = &gbufferB_info[i],
			        .pBufferInfo      = nullptr,
			        .pTexelBufferView = nullptr,
			    },
			    // Binding = 2: GBufferC
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_denoise.temporal_accumulation.descriptor_sets[i],
			        .dstBinding       = 2,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .pImageInfo       = &gbufferC_info[i],
			        .pBufferInfo      = nullptr,
			        .pTexelBufferView = nullptr,
			    },
			    // Binding = 3: Depth Buffer
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_denoise.temporal_accumulation.descriptor_sets[i],
			        .dstBinding       = 3,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .pImageInfo       = &depth_stencil_info[i],
			        .pBufferInfo      = nullptr,
			        .pTexelBufferView = nullptr,
			    },
			    // Binding = 4: Prev GBufferB
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_denoise.temporal_accumulation.descriptor_sets[i],
			        .dstBinding       = 4,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .pImageInfo       = &gbufferB_info[!i],
			        .pBufferInfo      = nullptr,
			        .pTexelBufferView = nullptr,
			    },
			    // Binding = 5: Prev GBufferC
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_denoise.temporal_accumulation.descriptor_sets[i],
			        .dstBinding       = 5,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .pImageInfo       = &gbufferC_info[!i],
			        .pBufferInfo      = nullptr,
			        .pTexelBufferView = nullptr,
			    },
			    // Binding = 6: Prev Depth Buffer
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_denoise.temporal_accumulation.descriptor_sets[i],
			        .dstBinding       = 6,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .pImageInfo       = &depth_stencil_info[!i],
			        .pBufferInfo      = nullptr,
			        .pTexelBufferView = nullptr,
			    },
			    // Binding = 7: Ray Traced Image
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_denoise.temporal_accumulation.descriptor_sets[i],
			        .dstBinding       = 7,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .pImageInfo       = &raytraced_image_info[1],
			        .pBufferInfo      = nullptr,
			        .pTexelBufferView = nullptr,
			    },
			    // Binding = 8: AO Image
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_denoise.temporal_accumulation.descriptor_sets[i],
			        .dstBinding       = 8,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			        .pImageInfo       = &ao_image_info[i][0],
			        .pBufferInfo      = nullptr,
			        .pTexelBufferView = nullptr,
			    },
			    // Binding = 9: History Length Image
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_denoise.temporal_accumulation.descriptor_sets[i],
			        .dstBinding       = 9,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			        .pImageInfo       = &history_length_image_info[i][0],
			        .pBufferInfo      = nullptr,
			        .pTexelBufferView = nullptr,
			    },
			    // Binding = 10: Prev AO Image
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_denoise.temporal_accumulation.descriptor_sets[i],
			        .dstBinding       = 10,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .pImageInfo       = &ao_image_info[i][1],
			        .pBufferInfo      = nullptr,
			        .pTexelBufferView = nullptr,
			    },
			    // Binding = 11: Prev History Length Image
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_denoise.temporal_accumulation.descriptor_sets[i],
			        .dstBinding       = 11,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .pImageInfo       = &history_length_image_info[i][1],
			        .pBufferInfo      = nullptr,
			        .pTexelBufferView = nullptr,
			    },
			    // Binding = 12: Denoise tile buffer
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_denoise.temporal_accumulation.descriptor_sets[i],
			        .dstBinding       = 12,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			        .pImageInfo       = nullptr,
			        .pBufferInfo      = &denoise_tile_buffer_info,
			        .pTexelBufferView = nullptr,
			    },
			    // Binding = 13: Denoise tile dispatch argument buffer
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_denoise.temporal_accumulation.descriptor_sets[i],
			        .dstBinding       = 13,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			        .pImageInfo       = nullptr,
			        .pBufferInfo      = &denoise_tile_dispatch_args_buffer_info,
			        .pTexelBufferView = nullptr,
			    },
			};
			vkUpdateDescriptorSets(m_context->vk_device, 14, writes, 0, nullptr);
		}
	}
}

void RayTracedAO::draw(VkCommandBuffer cmd_buffer)
{
	// Ray Traced
	m_context->begin_marker(cmd_buffer, "Ray Traced AO");
	{
		m_context->begin_marker(cmd_buffer, "Ray Traced");
		{
			m_raytraced.push_constant.gbuffer_mip = m_gbuffer_mip;

			vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_raytraced.pipeline_layout, 0, 1, &m_raytraced.descriptor_sets[m_context->ping_pong], 0, nullptr);
			vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_raytraced.pipeline);
			vkCmdPushConstants(cmd_buffer, m_raytraced.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(m_raytraced.push_constant), &m_raytraced.push_constant);
			vkCmdDispatch(cmd_buffer, static_cast<uint32_t>(ceil(float(m_width) / float(RAY_TRACE_NUM_THREADS_X))), static_cast<uint32_t>(ceil(float(m_height) / float(RAY_TRACE_NUM_THREADS_Y))), 1);
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
			        .image               = raytraced_image.vk_image,
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
			        .image               = ao_image[m_context->ping_pong].vk_image,
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
			        .image               = ao_image[!m_context->ping_pong].vk_image,
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
			        .image               = history_length_image[m_context->ping_pong].vk_image,
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
			        .image               = history_length_image[!m_context->ping_pong].vk_image,
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
			    0, 0, nullptr, 0, nullptr, 5, image_barriers);
		}

		m_context->begin_marker(cmd_buffer, "Temporal Accumulation");
		{
			m_denoise.temporal_accumulation.push_constant.gbuffer_mip = m_gbuffer_mip;

			vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_denoise.temporal_accumulation.pipeline_layout, 0, 1, &m_denoise.temporal_accumulation.descriptor_sets[m_context->ping_pong], 0, nullptr);
			vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_denoise.temporal_accumulation.pipeline);
			vkCmdPushConstants(cmd_buffer, m_denoise.temporal_accumulation.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(m_denoise.temporal_accumulation.push_constant), &m_denoise.temporal_accumulation.push_constant);
			vkCmdDispatch(cmd_buffer, static_cast<uint32_t>(ceil(float(m_width) / float(RAY_TRACE_NUM_THREADS_X))), static_cast<uint32_t>(ceil(float(m_height) / float(RAY_TRACE_NUM_THREADS_Y))), 1);
		}
		m_context->end_marker(cmd_buffer);

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
			    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			    0, 0, nullptr, 0, nullptr, 1, image_barriers);
		}
	}
	m_context->end_marker(cmd_buffer);
}

bool RayTracedAO::draw_ui()
{
	bool update = false;
	if (ImGui::TreeNode("Ray Traced AO"))
	{
		if (ImGui::TreeNode("Ray Traced"))
		{
			update |= ImGui::SliderFloat("Ray Length", &m_raytraced.push_constant.ray_length, 0.0f, 10.0f);
			update |= ImGui::DragFloat("Ray Traced Bias", &m_raytraced.push_constant.bias, 0.001f, 0.0f, 100.0f, "%.3f");
		}
		ImGui::TreePop();
	}
	return update;
}
