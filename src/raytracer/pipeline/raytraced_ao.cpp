#include "render/pipeline/raytraced_ao.hpp"

#include <imgui.h>
#include <spdlog/fmt/fmt.h>

static const uint32_t RAY_TRACE_NUM_THREADS_X = 8;
static const uint32_t RAY_TRACE_NUM_THREADS_Y = 4;

static const uint32_t TEMPORAL_ACCUMULATION_NUM_THREADS_X = 8;
static const uint32_t TEMPORAL_ACCUMULATION_NUM_THREADS_Y = 8;

static const uint32_t NUM_THREADS_X = 8;
static const uint32_t NUM_THREADS_Y = 8;

static unsigned char g_ao_raytraced_comp_spv_data[] = {
#include "ao_raytraced.comp.spv.h"
};

static unsigned char g_ao_temporal_accumulation_comp_spv_data[] = {
#include "ao_temporal_accumulation.comp.spv.h"
};

static unsigned char g_ao_bilateral_blur_comp_spv_data[] = {
#include "ao_bilateral_blur.comp.spv.h"
};

static unsigned char g_ao_upsampling_comp_spv_data[] = {
#include "ao_upsampling.comp.spv.h"
};

RayTracedAO::RayTracedAO(const Context &context, const Scene &scene, const GBufferPass &gbuffer_pass, RayTracedScale scale) :
    m_context(&context)
{
	float scale_divisor = powf(2.0f, float(scale));

	m_width  = static_cast<uint32_t>(static_cast<float>(context.renderExtent.width) / scale_divisor);
	m_height = static_cast<uint32_t>(static_cast<float>(context.renderExtent.height) / scale_divisor);

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

	// Create AO image & history length image & bilateral blur image
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
		    .usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		    .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
		    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};
		VmaAllocationCreateInfo allocation_create_info = {
		    .usage = VMA_MEMORY_USAGE_GPU_ONLY,
		};
		vmaCreateImage(context.vma_allocator, &image_create_info, &allocation_create_info, &ao_image[i].vk_image, &ao_image[i].vma_allocation, nullptr);
		vmaCreateImage(context.vma_allocator, &image_create_info, &allocation_create_info, &history_length_image[i].vk_image, &history_length_image[i].vma_allocation, nullptr);
		vmaCreateImage(context.vma_allocator, &image_create_info, &allocation_create_info, &bilateral_blur_image[i].vk_image, &bilateral_blur_image[i].vma_allocation, nullptr);
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
		view_create_info.image = bilateral_blur_image[i].vk_image;
		vkCreateImageView(context.vk_device, &view_create_info, nullptr, &bilateral_blur_image_view[i]);
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) ao_image[i].vk_image, fmt::format("AO Image - {}", i).c_str());
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) ao_image_view[i], fmt::format("AO Image View - {}", i).c_str());
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) history_length_image[i].vk_image, fmt::format("History Length Image - {}", i).c_str());
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) history_length_image_view[i], fmt::format("History Length Image View - {}", i).c_str());
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) bilateral_blur_image[i].vk_image, fmt::format("Bilateral Blur Image - {}", i).c_str());
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) bilateral_blur_image_view[i], fmt::format("Bilateral Blur Image View - {}", i).c_str());
	}

	// Create upsampling ao image
	{
		VkImageCreateInfo image_create_info = {
		    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		    .imageType     = VK_IMAGE_TYPE_2D,
		    .format        = VK_FORMAT_R32_SFLOAT,
		    .extent        = VkExtent3D{m_context->renderExtent.width, m_context->renderExtent.height, 1},
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
			    // Raytraced image
			    {
			        .binding         = 0,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			};
			VkDescriptorSetLayoutCreateInfo create_info = {
			    .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			    .flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
			    .bindingCount = 1,
			    .pBindings    = bindings,
			};
			vkCreateDescriptorSetLayout(m_context->vk_device, &create_info, nullptr, &m_raytraced.descriptor_set_layout);
		}

		// Allocate descriptor set
		{
			VkDescriptorSetLayout descriptor_set_layouts[] = {
			    m_raytraced.descriptor_set_layout,
			};

			VkDescriptorSetAllocateInfo allocate_info = {
			    .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			    .pNext              = nullptr,
			    .descriptorPool     = m_context->vk_descriptor_pool,
			    .descriptorSetCount = 1,
			    .pSetLayouts        = descriptor_set_layouts,
			};
			vkAllocateDescriptorSets(m_context->vk_device, &allocate_info, &m_raytraced.descriptor_set);
		}

		// Create pipeline layout
		{
			VkDescriptorSetLayout descriptor_set_layouts[] = {
			    scene.descriptor.layout,
			    gbuffer_pass.descriptor.layout,
			    m_raytraced.descriptor_set_layout,
			};

			VkPushConstantRange range = {
			    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			    .offset     = 0,
			    .size       = sizeof(m_raytraced.push_constant),
			};
			VkPipelineLayoutCreateInfo create_info = {
			    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			    .setLayoutCount         = 3,
			    .pSetLayouts            = descriptor_set_layouts,
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
			    // Ray Traced Image
			    {
			        .binding         = 0,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // AO Image
			    {
			        .binding         = 1,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // History Length Image
			    {
			        .binding         = 2,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Prev AO Image
			    {
			        .binding         = 3,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Prev History Length Image
			    {
			        .binding         = 4,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Denoise Tile Data
			    {
			        .binding         = 5,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Denoise Tile Dispatch Argument
			    {
			        .binding         = 6,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			};
			VkDescriptorSetLayoutCreateInfo create_info = {
			    .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			    .bindingCount = 7,
			    .pBindings    = bindings,
			};
			vkCreateDescriptorSetLayout(m_context->vk_device, &create_info, nullptr, &m_denoise.temporal_accumulation.descriptor_set_layout);
		}

		// Allocate descriptor set
		{
			VkDescriptorSetLayout descriptor_set_layouts[] = {
			    m_denoise.temporal_accumulation.descriptor_set_layout,
			    m_denoise.temporal_accumulation.descriptor_set_layout,
			};

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
			VkDescriptorSetLayout descriptor_set_layouts[] = {
			    scene.descriptor.layout,
			    gbuffer_pass.descriptor.layout,
			    m_denoise.temporal_accumulation.descriptor_set_layout,
			};
			VkPushConstantRange range = {
			    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			    .offset     = 0,
			    .size       = sizeof(m_denoise.temporal_accumulation.push_constant),
			};
			VkPipelineLayoutCreateInfo create_info = {
			    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			    .setLayoutCount         = 3,
			    .pSetLayouts            = descriptor_set_layouts,
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

	// Create bilateral blur pass
	{
		// Create shader module
		VkShaderModule shader = VK_NULL_HANDLE;
		{
			VkShaderModuleCreateInfo create_info = {
			    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			    .codeSize = sizeof(g_ao_bilateral_blur_comp_spv_data),
			    .pCode    = reinterpret_cast<uint32_t *>(g_ao_bilateral_blur_comp_spv_data),
			};
			vkCreateShaderModule(context.vk_device, &create_info, nullptr, &shader);
		}

		// Create descriptor set layout
		{
			VkDescriptorSetLayoutBinding bindings[] = {
			    // AO Image
			    {
			        .binding         = 0,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Bilateral Image
			    {
			        .binding         = 1,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // History Length
			    {
			        .binding         = 2,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Denoise Tile Data
			    {
			        .binding         = 3,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			};
			VkDescriptorSetLayoutCreateInfo create_info = {
			    .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			    .flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
			    .bindingCount = 4,
			    .pBindings    = bindings,
			};
			vkCreateDescriptorSetLayout(m_context->vk_device, &create_info, nullptr, &m_denoise.bilateral_blur.descriptor_set_layout);
		}

		// Allocate descriptor set
		{
			VkDescriptorSetLayout descriptor_set_layouts[] = {
			    m_denoise.bilateral_blur.descriptor_set_layout,
			    m_denoise.bilateral_blur.descriptor_set_layout,
			};

			VkDescriptorSetAllocateInfo allocate_info = {
			    .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			    .pNext              = nullptr,
			    .descriptorPool     = m_context->vk_descriptor_pool,
			    .descriptorSetCount = 2,
			    .pSetLayouts        = descriptor_set_layouts,
			};
			vkAllocateDescriptorSets(m_context->vk_device, &allocate_info, m_denoise.bilateral_blur.descriptor_sets[0]);
			vkAllocateDescriptorSets(m_context->vk_device, &allocate_info, m_denoise.bilateral_blur.descriptor_sets[1]);
		}

		// Create pipeline layout
		{
			VkDescriptorSetLayout descriptor_set_layouts[] = {
			    scene.descriptor.layout,
			    gbuffer_pass.descriptor.layout,
			    m_denoise.bilateral_blur.descriptor_set_layout,
			};
			VkPushConstantRange range = {
			    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			    .offset     = 0,
			    .size       = sizeof(m_denoise.bilateral_blur.push_constant),
			};
			VkPipelineLayoutCreateInfo create_info = {
			    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			    .setLayoutCount         = 3,
			    .pSetLayouts            = descriptor_set_layouts,
			    .pushConstantRangeCount = 1,
			    .pPushConstantRanges    = &range,
			};
			vkCreatePipelineLayout(m_context->vk_device, &create_info, nullptr, &m_denoise.bilateral_blur.pipeline_layout);
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
			    .layout             = m_denoise.bilateral_blur.pipeline_layout,
			    .basePipelineHandle = VK_NULL_HANDLE,
			    .basePipelineIndex  = -1,
			};
			vkCreateComputePipelines(m_context->vk_device, m_context->vk_pipeline_cache, 1, &create_info, nullptr, &m_denoise.bilateral_blur.pipeline);
			vkDestroyShaderModule(m_context->vk_device, shader, nullptr);
		}
	}

	// Create upsampling pass
	{
		// Create shader module
		VkShaderModule shader = VK_NULL_HANDLE;
		{
			VkShaderModuleCreateInfo create_info = {
			    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			    .codeSize = sizeof(g_ao_upsampling_comp_spv_data),
			    .pCode    = reinterpret_cast<uint32_t *>(g_ao_upsampling_comp_spv_data),
			};
			vkCreateShaderModule(context.vk_device, &create_info, nullptr, &shader);
		}

		// Create descriptor set layout
		{
			VkDescriptorSetLayoutBinding bindings[] = {
			    // Upsample Image
			    {
			        .binding         = 0,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Bilateral Image
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
			vkCreateDescriptorSetLayout(m_context->vk_device, &create_info, nullptr, &m_upsampling.descriptor_set_layout);
		}

		// Allocate descriptor set
		{
			VkDescriptorSetLayout descriptor_set_layouts[] = {
			    m_upsampling.descriptor_set_layout,
			    m_upsampling.descriptor_set_layout,
			};
			VkDescriptorSetAllocateInfo allocate_info = {
			    .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			    .pNext              = nullptr,
			    .descriptorPool     = m_context->vk_descriptor_pool,
			    .descriptorSetCount = 2,
			    .pSetLayouts        = descriptor_set_layouts,
			};
			vkAllocateDescriptorSets(m_context->vk_device, &allocate_info, m_upsampling.descriptor_sets);
		}

		// Create pipeline layout
		{
			VkDescriptorSetLayout descriptor_set_layouts[] = {
			    scene.descriptor.layout,
			    gbuffer_pass.descriptor.layout,
			    m_upsampling.descriptor_set_layout,
			};
			VkPushConstantRange range = {
			    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			    .offset     = 0,
			    .size       = sizeof(m_upsampling.push_constant),
			};
			VkPipelineLayoutCreateInfo create_info = {
			    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			    .setLayoutCount         = 3,
			    .pSetLayouts            = descriptor_set_layouts,
			    .pushConstantRangeCount = 1,
			    .pPushConstantRanges    = &range,
			};
			vkCreatePipelineLayout(m_context->vk_device, &create_info, nullptr, &m_upsampling.pipeline_layout);
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
			    .layout             = m_upsampling.pipeline_layout,
			    .basePipelineHandle = VK_NULL_HANDLE,
			    .basePipelineIndex  = -1,
			};
			vkCreateComputePipelines(m_context->vk_device, m_context->vk_pipeline_cache, 1, &create_info, nullptr, &m_upsampling.pipeline);
			vkDestroyShaderModule(m_context->vk_device, shader, nullptr);
		}
	}
}

RayTracedAO::~RayTracedAO()
{
	vkDestroyPipelineLayout(m_context->vk_device, m_raytraced.pipeline_layout, nullptr);
	vkDestroyPipelineLayout(m_context->vk_device, m_denoise.temporal_accumulation.pipeline_layout, nullptr);
	vkDestroyPipelineLayout(m_context->vk_device, m_denoise.bilateral_blur.pipeline_layout, nullptr);
	vkDestroyPipelineLayout(m_context->vk_device, m_upsampling.pipeline_layout, nullptr);

	vkDestroyPipeline(m_context->vk_device, m_raytraced.pipeline, nullptr);
	vkDestroyPipeline(m_context->vk_device, m_denoise.temporal_accumulation.pipeline, nullptr);
	vkDestroyPipeline(m_context->vk_device, m_denoise.bilateral_blur.pipeline, nullptr);
	vkDestroyPipeline(m_context->vk_device, m_upsampling.pipeline, nullptr);

	vkDestroyDescriptorSetLayout(m_context->vk_device, m_raytraced.descriptor_set_layout, nullptr);
	vkDestroyDescriptorSetLayout(m_context->vk_device, m_denoise.temporal_accumulation.descriptor_set_layout, nullptr);
	vkDestroyDescriptorSetLayout(m_context->vk_device, m_denoise.bilateral_blur.descriptor_set_layout, nullptr);
	vkDestroyDescriptorSetLayout(m_context->vk_device, m_upsampling.descriptor_set_layout, nullptr);

	vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 1, &m_raytraced.descriptor_set);
	vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 2, m_denoise.temporal_accumulation.descriptor_sets);
	vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 2, m_denoise.bilateral_blur.descriptor_sets[0]);
	vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 2, m_denoise.bilateral_blur.descriptor_sets[1]);
	vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 2, m_upsampling.descriptor_sets);

	vkDestroyImageView(m_context->vk_device, raytraced_image_view, nullptr);
	vkDestroyImageView(m_context->vk_device, upsampled_ao_image_view, nullptr);

	vmaDestroyImage(m_context->vma_allocator, raytraced_image.vk_image, raytraced_image.vma_allocation);
	vmaDestroyImage(m_context->vma_allocator, upsampled_ao_image.vk_image, upsampled_ao_image.vma_allocation);

	vmaDestroyBuffer(m_context->vma_allocator, denoise_tile_buffer.vk_buffer, denoise_tile_buffer.vma_allocation);
	vmaDestroyBuffer(m_context->vma_allocator, denoise_tile_dispatch_args_buffer.vk_buffer, denoise_tile_dispatch_args_buffer.vma_allocation);

	for (uint32_t i = 0; i < 2; i++)
	{
		vkDestroyImageView(m_context->vk_device, ao_image_view[i], nullptr);
		vkDestroyImageView(m_context->vk_device, history_length_image_view[i], nullptr);
		vkDestroyImageView(m_context->vk_device, bilateral_blur_image_view[i], nullptr);

		vmaDestroyImage(m_context->vma_allocator, ao_image[i].vk_image, ao_image[i].vma_allocation);
		vmaDestroyImage(m_context->vma_allocator, history_length_image[i].vk_image, history_length_image[i].vma_allocation);
		vmaDestroyImage(m_context->vma_allocator, bilateral_blur_image[i].vk_image, bilateral_blur_image[i].vma_allocation);
	}
}

void RayTracedAO::init(VkCommandBuffer cmd_buffer)
{
	VkBufferMemoryBarrier buffer_barrier = {
	    .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
	    .pNext               = nullptr,
	    .srcAccessMask       = 0,
	    .dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
	    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .buffer              = denoise_tile_dispatch_args_buffer.vk_buffer,
	    .offset              = 0,
	    .size                = sizeof(VkDispatchIndirectCommand),
	};
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
	        .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
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
	        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
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
	        .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
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
	        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
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
	        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
	        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
	        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
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
	    {
	        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .srcAccessMask       = 0,
	        .dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
	        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
	        .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image               = bilateral_blur_image[0].vk_image,
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
	        .image               = bilateral_blur_image[1].vk_image,
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
	    0, 0, nullptr, 1, &buffer_barrier, 8, image_barriers);
}

void RayTracedAO::update(const Scene &scene, const GBufferPass &gbuffer_pass)
{
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
	            .sampler     = scene.nearest_sampler,
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
	            .sampler     = scene.nearest_sampler,
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
	            .sampler     = scene.nearest_sampler,
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
	            .sampler     = scene.nearest_sampler,
	            .imageView   = history_length_image_view[0],
	            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        },
	    },
	};

	VkDescriptorImageInfo bilateral_blur_image_info[2][2] = {
	    {
	        VkDescriptorImageInfo{
	            .sampler     = VK_NULL_HANDLE,
	            .imageView   = bilateral_blur_image_view[0],
	            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	        },
	        VkDescriptorImageInfo{
	            .sampler     = VK_NULL_HANDLE,
	            .imageView   = bilateral_blur_image_view[1],
	            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	        },
	    },
	    VkDescriptorImageInfo{
	        .sampler     = scene.nearest_sampler,
	        .imageView   = bilateral_blur_image_view[0],
	        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    },
	    VkDescriptorImageInfo{
	        .sampler     = scene.nearest_sampler,
	        .imageView   = bilateral_blur_image_view[1],
	        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    },
	};

	VkDescriptorImageInfo upsampling_ao_image_info = {
	    .sampler     = VK_NULL_HANDLE,
	    .imageView   = upsampled_ao_image_view,
	    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	};

	// Raytraced
	{
		VkWriteDescriptorSet writes[] = {
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_raytraced.descriptor_set,
		        .dstBinding       = 0,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		        .pImageInfo       = &raytraced_image_info[0],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		};
		vkUpdateDescriptorSets(m_context->vk_device, 1, writes, 0, nullptr);
	}

	// Temporal accumulation
	{
		for (uint32_t i = 0; i < 2; i++)
		{
			VkWriteDescriptorSet writes[] = {
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_denoise.temporal_accumulation.descriptor_sets[i],
			        .dstBinding       = 0,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .pImageInfo       = &raytraced_image_info[1],
			        .pBufferInfo      = nullptr,
			        .pTexelBufferView = nullptr,
			    },
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_denoise.temporal_accumulation.descriptor_sets[i],
			        .dstBinding       = 1,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			        .pImageInfo       = &ao_image_info[i][0],
			        .pBufferInfo      = nullptr,
			        .pTexelBufferView = nullptr,
			    },
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_denoise.temporal_accumulation.descriptor_sets[i],
			        .dstBinding       = 2,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			        .pImageInfo       = &history_length_image_info[i][0],
			        .pBufferInfo      = nullptr,
			        .pTexelBufferView = nullptr,
			    },
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_denoise.temporal_accumulation.descriptor_sets[i],
			        .dstBinding       = 3,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .pImageInfo       = &ao_image_info[i][1],
			        .pBufferInfo      = nullptr,
			        .pTexelBufferView = nullptr,
			    },
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_denoise.temporal_accumulation.descriptor_sets[i],
			        .dstBinding       = 4,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .pImageInfo       = &history_length_image_info[i][1],
			        .pBufferInfo      = nullptr,
			        .pTexelBufferView = nullptr,
			    },
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_denoise.temporal_accumulation.descriptor_sets[i],
			        .dstBinding       = 5,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			        .pImageInfo       = nullptr,
			        .pBufferInfo      = &denoise_tile_buffer_info,
			        .pTexelBufferView = nullptr,
			    },
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_denoise.temporal_accumulation.descriptor_sets[i],
			        .dstBinding       = 6,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			        .pImageInfo       = nullptr,
			        .pBufferInfo      = &denoise_tile_dispatch_args_buffer_info,
			        .pTexelBufferView = nullptr,
			    },
			};
			vkUpdateDescriptorSets(m_context->vk_device, 7, writes, 0, nullptr);
		}
	}

	// Bilateral Blur
	{
		for (uint32_t i = 0; i < 2; i++)
		{
			for (uint32_t j = 0; j < 2; j++)
			{
				VkWriteDescriptorSet writes[] = {
				    {
				        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				        .dstSet           = m_denoise.bilateral_blur.descriptor_sets[i][j],
				        .dstBinding       = 0,
				        .dstArrayElement  = 0,
				        .descriptorCount  = 1,
				        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
				        .pImageInfo       = &bilateral_blur_image_info[0][j],
				        .pBufferInfo      = nullptr,
				        .pTexelBufferView = nullptr,
				    },
				    {
				        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				        .dstSet           = m_denoise.bilateral_blur.descriptor_sets[i][j],
				        .dstBinding       = 1,
				        .dstArrayElement  = 0,
				        .descriptorCount  = 1,
				        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				        .pImageInfo       = j == 0 ? &ao_image_info[!i][1] : &bilateral_blur_image_info[1][0],
				        .pBufferInfo      = nullptr,
				        .pTexelBufferView = nullptr,
				    },
				    {
				        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				        .dstSet           = m_denoise.bilateral_blur.descriptor_sets[i][j],
				        .dstBinding       = 2,
				        .dstArrayElement  = 0,
				        .descriptorCount  = 1,
				        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				        .pImageInfo       = &history_length_image_info[!i][1],
				        .pBufferInfo      = nullptr,
				        .pTexelBufferView = nullptr,
				    },
				    {
				        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				        .dstSet           = m_denoise.bilateral_blur.descriptor_sets[i][j],
				        .dstBinding       = 3,
				        .dstArrayElement  = 0,
				        .descriptorCount  = 1,
				        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				        .pImageInfo       = nullptr,
				        .pBufferInfo      = &denoise_tile_buffer_info,
				        .pTexelBufferView = nullptr,
				    },
				};
				vkUpdateDescriptorSets(m_context->vk_device, 4, writes, 0, nullptr);
			}
		}
	}

	// Upsampling
	{
		for (uint32_t i = 0; i < 2; i++)
		{
			VkWriteDescriptorSet writes[] = {
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_upsampling.descriptor_sets[i],
			        .dstBinding       = 0,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			        .pImageInfo       = &upsampling_ao_image_info,
			        .pBufferInfo      = nullptr,
			        .pTexelBufferView = nullptr,
			    },
			    {
			        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet           = m_upsampling.descriptor_sets[i],
			        .dstBinding       = 1,
			        .dstArrayElement  = 0,
			        .descriptorCount  = 1,
			        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .pImageInfo       = &bilateral_blur_image_info[1][1],
			        .pBufferInfo      = nullptr,
			        .pTexelBufferView = nullptr,
			    },
			};
			vkUpdateDescriptorSets(m_context->vk_device, 2, writes, 0, nullptr);
		}
	}
}

void RayTracedAO::draw(VkCommandBuffer cmd_buffer, const Scene &scene, const GBufferPass &gbuffer_pass)
{
	// Ray Traced
	m_context->begin_marker(cmd_buffer, "Ray Traced AO");
	{
		m_context->begin_marker(cmd_buffer, "Ray Traced");
		{
			VkDescriptorSet descriptor_sets[] = {
			    scene.descriptor.set,
			    gbuffer_pass.descriptor.sets[m_context->ping_pong],
			    m_raytraced.descriptor_set,
			};
			m_raytraced.push_constant.gbuffer_mip = m_gbuffer_mip;
			vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_raytraced.pipeline_layout, 0, 3, descriptor_sets, 0, nullptr);
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
			};
			vkCmdPipelineBarrier(
			    cmd_buffer,
			    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			    0, 0, nullptr, 0, nullptr, 1, image_barriers);
		}

		m_context->begin_marker(cmd_buffer, "Temporal Accumulation");
		{
			VkDescriptorSet descriptor_sets[] = {
			    scene.descriptor.set,
			    gbuffer_pass.descriptor.sets[m_context->ping_pong],
			    m_denoise.temporal_accumulation.descriptor_sets[m_context->ping_pong],
			};
			m_denoise.temporal_accumulation.push_constant.gbuffer_mip = m_gbuffer_mip;
			vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_denoise.temporal_accumulation.pipeline_layout, 0, 3, descriptor_sets, 0, nullptr);
			vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_denoise.temporal_accumulation.pipeline);
			vkCmdPushConstants(cmd_buffer, m_denoise.temporal_accumulation.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(m_denoise.temporal_accumulation.push_constant), &m_denoise.temporal_accumulation.push_constant);
			vkCmdDispatch(cmd_buffer, static_cast<uint32_t>(ceil(float(m_width) / float(TEMPORAL_ACCUMULATION_NUM_THREADS_X))), static_cast<uint32_t>(ceil(float(m_height) / float(TEMPORAL_ACCUMULATION_NUM_THREADS_Y))), 1);
		}
		m_context->end_marker(cmd_buffer);

		{
			{
				VkBufferMemoryBarrier buffer_barrier = {
				    .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
				    .pNext               = nullptr,
				    .srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
				    .dstAccessMask       = VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
				    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				    .buffer              = denoise_tile_dispatch_args_buffer.vk_buffer,
				    .offset              = 0,
				    .size                = sizeof(VkDispatchIndirectCommand),
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
				        .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
				        .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
				        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				        .image               = bilateral_blur_image[0].vk_image,
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
				        .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
				        .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
				        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				        .image               = bilateral_blur_image[1].vk_image,
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
				    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
				    0, 0, nullptr, 1, &buffer_barrier, 4, image_barriers);
			}

			{
				VkClearColorValue clear_value = {
				    .float32 = {1.f, 1.f, 1.f, 1.f},
				};
				VkImageSubresourceRange range = {
				    .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
				    .baseMipLevel   = 0,
				    .levelCount     = 1,
				    .baseArrayLayer = 0,
				    .layerCount     = 1,
				};
				vkCmdClearColorImage(cmd_buffer, bilateral_blur_image[0].vk_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_value, 1, &range);
				vkCmdClearColorImage(cmd_buffer, bilateral_blur_image[1].vk_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_value, 1, &range);
			}

			{
				VkImageMemoryBarrier image_barriers[] = {
				    {
				        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				        .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
				        .dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
				        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				        .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
				        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				        .image               = bilateral_blur_image[0].vk_image,
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
				        .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
				        .dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
				        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				        .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
				        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				        .image               = bilateral_blur_image[1].vk_image,
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
				    VK_PIPELINE_STAGE_TRANSFER_BIT,
				    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				    0, 0, nullptr, 0, nullptr, 2, image_barriers);
			}
		}

		m_context->begin_marker(cmd_buffer, "Bilateral Blur");
		{
			vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_denoise.bilateral_blur.pipeline);

			float z_buffer_params_x = -1.f + (CAMERA_NEAR_PLANE / CAMERA_FAR_PLANE);

			m_denoise.bilateral_blur.push_constant.gbuffer_mip     = m_gbuffer_mip;
			m_denoise.bilateral_blur.push_constant.z_buffer_params = glm::vec4(z_buffer_params_x, 1.0f, z_buffer_params_x / CAMERA_NEAR_PLANE, 1.0f / CAMERA_NEAR_PLANE);

			m_context->begin_marker(cmd_buffer, "Vertical Blur");
			{
				VkDescriptorSet descriptor_sets[] = {
				    scene.descriptor.set,
				    gbuffer_pass.descriptor.sets[m_context->ping_pong],
				    m_denoise.bilateral_blur.descriptor_sets[m_context->ping_pong][0],
				};
				m_denoise.bilateral_blur.push_constant.direction = glm::ivec2(1, 0);
				vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_denoise.bilateral_blur.pipeline_layout, 0, 3, descriptor_sets, 0, nullptr);
				vkCmdPushConstants(cmd_buffer, m_denoise.bilateral_blur.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(m_denoise.bilateral_blur.push_constant), &m_denoise.bilateral_blur.push_constant);
				vkCmdDispatchIndirect(cmd_buffer, denoise_tile_dispatch_args_buffer.vk_buffer, 0);
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
				        .image               = bilateral_blur_image[0].vk_image,
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
			m_context->begin_marker(cmd_buffer, "Horizontal Blur");
			{
				VkDescriptorSet descriptor_sets[] = {
				    scene.descriptor.set,
				    gbuffer_pass.descriptor.sets[m_context->ping_pong],
				    m_denoise.bilateral_blur.descriptor_sets[m_context->ping_pong][1],
				};
				m_denoise.bilateral_blur.push_constant.direction = glm::ivec2(0, 1);
				vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_denoise.bilateral_blur.pipeline_layout, 0, 3, descriptor_sets, 0, nullptr);
				vkCmdPushConstants(cmd_buffer, m_denoise.bilateral_blur.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(m_denoise.bilateral_blur.push_constant), &m_denoise.bilateral_blur.push_constant);
				vkCmdDispatchIndirect(cmd_buffer, denoise_tile_dispatch_args_buffer.vk_buffer, 0);
			}
			m_context->end_marker(cmd_buffer);
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
			        .image               = bilateral_blur_image[1].vk_image,
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
			    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			    0, 0, nullptr, 0, nullptr, 2, image_barriers);
		}

		m_context->begin_marker(cmd_buffer, "Upsampling");
		{
			VkDescriptorSet descriptor_sets[] = {
			    scene.descriptor.set,
			    gbuffer_pass.descriptor.sets[m_context->ping_pong],
			    m_upsampling.descriptor_sets[m_context->ping_pong],
			};
			m_upsampling.push_constant.gbuffer_mip = m_gbuffer_mip;
			vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_upsampling.pipeline_layout, 0, 3, descriptor_sets, 0, nullptr);
			vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_upsampling.pipeline);
			vkCmdPushConstants(cmd_buffer, m_upsampling.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(m_upsampling.push_constant), &m_upsampling.push_constant);
			vkCmdDispatch(cmd_buffer, static_cast<uint32_t>(ceil(float(m_context->renderExtent.width) / float(NUM_THREADS_X))), static_cast<uint32_t>(ceil(float(m_context->renderExtent.height) / float(NUM_THREADS_Y))), 1);
		}
		m_context->end_marker(cmd_buffer);
		{
			VkBufferMemoryBarrier buffer_barrier = {
			    .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			    .pNext               = nullptr,
			    .srcAccessMask       = VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
			    .dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
			    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			    .buffer              = denoise_tile_dispatch_args_buffer.vk_buffer,
			    .offset              = 0,
			    .size                = sizeof(VkDispatchIndirectCommand),
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
			        .image               = history_length_image[!m_context->ping_pong].vk_image,
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
			        .image               = bilateral_blur_image[0].vk_image,
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
			        .image               = bilateral_blur_image[1].vk_image,
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
			    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
			    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			    0, 0, nullptr, 1, &buffer_barrier, 6, image_barriers);
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
			update |= ImGui::DragInt("Blur Radius", &m_denoise.bilateral_blur.push_constant.radius, 1, 1, 10);
			update |= ImGui::Checkbox("Debug", reinterpret_cast<bool *>(&m_upsampling.push_constant.debug));
		}
		ImGui::TreePop();
	}
	return update;
}
