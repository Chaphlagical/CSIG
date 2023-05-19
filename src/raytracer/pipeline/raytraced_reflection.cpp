#include "render/pipeline/raytraced_reflection.hpp"

#define NUM_THREADS_X 8
#define NUM_THREADS_Y 8

static unsigned char g_reflection_raytraced_comp_spv_data[] = {
#include "reflection_raytrace.comp.spv.h"
};

static unsigned char g_reflection_reprojection_comp_spv_data[] = {
#include "reflection_reprojection.comp.spv.h"
};

static unsigned char g_reflection_copy_tiles_comp_spv_data[] = {
#include "reflection_copy_tiles.comp.spv.h"
};

static unsigned char g_reflection_atrous_comp_spv_data[] = {
#include "reflection_atrous.comp.spv.h"
};

static unsigned char g_reflection_upsampling_comp_spv_data[] = {
#include "reflection_upsampling.comp.spv.h"
};

RayTracedReflection::RayTracedReflection(const Context &context, const Scene &scene, const GBufferPass &gbuffer_pass, const BlueNoise &blue_noise, const LUT &lut, const RayTracedGI &raytraced_gi, RayTracedScale scale) :
    m_context(&context)
{
	float scale_divisor = powf(2.0f, float(scale));

	m_width  = static_cast<uint32_t>(static_cast<float>(context.renderExtent.width) / scale_divisor);
	m_height = static_cast<uint32_t>(static_cast<float>(context.renderExtent.height) / scale_divisor);

	m_gbuffer_mip = static_cast<uint32_t>(scale);

	// Create raytrace image
	{
		VkImageCreateInfo image_create_info = {
		    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		    .imageType     = VK_IMAGE_TYPE_2D,
		    .format        = VK_FORMAT_R16G16B16A16_SFLOAT,
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
		vmaCreateImage(context.vma_allocator, &image_create_info, &allocation_create_info, &raytraced_image.vk_image, &raytraced_image.vma_allocation, nullptr);
		VkImageViewCreateInfo view_create_info = {
		    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image            = raytraced_image.vk_image,
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
		vkCreateImageView(context.vk_device, &view_create_info, nullptr, &raytraced_view);
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) raytraced_image.vk_image, "Reflection RayTraced Image");
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) raytraced_view, "Reflection RayTraced Image View");
	}

	// Create reprojection output image
	for (uint32_t i = 0; i < 2; i++)
	{
		VkImageCreateInfo image_create_info = {
		    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		    .imageType     = VK_IMAGE_TYPE_2D,
		    .format        = VK_FORMAT_R16G16B16A16_SFLOAT,
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
		vmaCreateImage(context.vma_allocator, &image_create_info, &allocation_create_info, &reprojection_output_image[i].vk_image, &reprojection_output_image[i].vma_allocation, nullptr);
		VkImageViewCreateInfo view_create_info = {
		    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image            = reprojection_output_image[i].vk_image,
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
		vkCreateImageView(context.vk_device, &view_create_info, nullptr, &reprojection_output_view[i]);
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) reprojection_output_image[i].vk_image, (std::string("Reflection Reprojection Output Image - ") + std::to_string(i)).c_str());
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) reprojection_output_view[i], (std::string("Reflection Reprojection Output Image View - ") + std::to_string(i)).c_str());
	}

	// Create reprojection moment image
	for (uint32_t i = 0; i < 2; i++)
	{
		VkImageCreateInfo image_create_info = {
		    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		    .imageType     = VK_IMAGE_TYPE_2D,
		    .format        = VK_FORMAT_R16G16B16A16_SFLOAT,
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
		vmaCreateImage(context.vma_allocator, &image_create_info, &allocation_create_info, &reprojection_moment_image[i].vk_image, &reprojection_moment_image[i].vma_allocation, nullptr);
		VkImageViewCreateInfo view_create_info = {
		    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image            = reprojection_moment_image[i].vk_image,
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
		vkCreateImageView(context.vk_device, &view_create_info, nullptr, &reprojection_moment_view[i]);
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) reprojection_moment_image[i].vk_image, (std::string("Reflection Reprojection Moment Image - ") + std::to_string(i)).c_str());
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) reprojection_moment_view[i], (std::string("Reflection Reprojection Moment Image View - ") + std::to_string(i)).c_str());
	}

	// Create A-trous image
	for (uint32_t i = 0; i < 2; i++)
	{
		VkImageCreateInfo image_create_info = {
		    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		    .imageType     = VK_IMAGE_TYPE_2D,
		    .format        = VK_FORMAT_R16G16B16A16_SFLOAT,
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
		vmaCreateImage(context.vma_allocator, &image_create_info, &allocation_create_info, &a_trous_image[i].vk_image, &a_trous_image[i].vma_allocation, nullptr);
		VkImageViewCreateInfo view_create_info = {
		    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image            = a_trous_image[i].vk_image,
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
		vkCreateImageView(context.vk_device, &view_create_info, nullptr, &a_trous_view[i]);
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) a_trous_image[i].vk_image, (std::string("Reflection A-Trous Image - ") + std::to_string(i)).c_str());
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) a_trous_view[i], (std::string("Reflection A-Trous View - ") + std::to_string(i)).c_str());
	}

	// Create upsampling output image
	{
		VkImageCreateInfo image_create_info = {
		    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		    .imageType     = VK_IMAGE_TYPE_2D,
		    .format        = VK_FORMAT_R16G16B16A16_SFLOAT,
		    .extent        = VkExtent3D{m_context->renderExtent.width, m_context->renderExtent.height, 1},
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
		vmaCreateImage(context.vma_allocator, &image_create_info, &allocation_create_info, &upsampling_image.vk_image, &upsampling_image.vma_allocation, nullptr);
		VkImageViewCreateInfo view_create_info = {
		    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image            = upsampling_image.vk_image,
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
		vkCreateImageView(context.vk_device, &view_create_info, nullptr, &upsampling_view);
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) upsampling_image.vk_image, "Reflection Upsampling Output Image");
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) upsampling_view, "Reflection Upsampling Output View");
	}

	// Create tile data buffer
	{
		VkBufferCreateInfo buffer_create_info = {
		    .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		    .size        = sizeof(glm::ivec2) * static_cast<uint32_t>(ceil(float(m_width) / float(NUM_THREADS_X))) * static_cast<uint32_t>(ceil(float(m_height) / float(NUM_THREADS_Y))),
		    .usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};
		VmaAllocationCreateInfo allocation_create_info = {
		    .usage = VMA_MEMORY_USAGE_GPU_ONLY,
		};
		VmaAllocationInfo allocation_info = {};
		vmaCreateBuffer(context.vma_allocator, &buffer_create_info, &allocation_create_info, &denoise_tile_data_buffer.vk_buffer, &denoise_tile_data_buffer.vma_allocation, &allocation_info);
		vmaCreateBuffer(context.vma_allocator, &buffer_create_info, &allocation_create_info, &copy_tile_data_buffer.vk_buffer, &copy_tile_data_buffer.vma_allocation, &allocation_info);
		VkBufferDeviceAddressInfoKHR buffer_device_address_info = {
		    .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
		    .buffer = denoise_tile_data_buffer.vk_buffer,
		};
		denoise_tile_data_buffer.device_address = vkGetBufferDeviceAddress(context.vk_device, &buffer_device_address_info);
		buffer_device_address_info.buffer       = copy_tile_data_buffer.vk_buffer;
		copy_tile_data_buffer.device_address    = vkGetBufferDeviceAddress(context.vk_device, &buffer_device_address_info);
		m_context->set_object_name(VK_OBJECT_TYPE_BUFFER, (uint64_t) denoise_tile_data_buffer.vk_buffer, "Denoise Tile Data Buffer");
		m_context->set_object_name(VK_OBJECT_TYPE_BUFFER, (uint64_t) copy_tile_data_buffer.vk_buffer, "Copy Tile Data Buffer");
	}

	// Create tile dispatch args buffer
	{
		VkBufferCreateInfo buffer_create_info = {
		    .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		    .size        = sizeof(int32_t) * 3,
		    .usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};
		VmaAllocationCreateInfo allocation_create_info = {
		    .usage = VMA_MEMORY_USAGE_GPU_ONLY,
		};
		VmaAllocationInfo allocation_info = {};
		vmaCreateBuffer(context.vma_allocator, &buffer_create_info, &allocation_create_info, &denoise_tile_dispatch_args_buffer.vk_buffer, &denoise_tile_dispatch_args_buffer.vma_allocation, &allocation_info);
		vmaCreateBuffer(context.vma_allocator, &buffer_create_info, &allocation_create_info, &copy_tile_dispatch_args_buffer.vk_buffer, &copy_tile_dispatch_args_buffer.vma_allocation, &allocation_info);
		VkBufferDeviceAddressInfoKHR buffer_device_address_info = {
		    .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
		    .buffer = denoise_tile_dispatch_args_buffer.vk_buffer,
		};
		denoise_tile_dispatch_args_buffer.device_address = vkGetBufferDeviceAddress(context.vk_device, &buffer_device_address_info);
		buffer_device_address_info.buffer                = copy_tile_dispatch_args_buffer.vk_buffer;
		copy_tile_dispatch_args_buffer.device_address    = vkGetBufferDeviceAddress(context.vk_device, &buffer_device_address_info);
		m_context->set_object_name(VK_OBJECT_TYPE_BUFFER, (uint64_t) denoise_tile_dispatch_args_buffer.vk_buffer, "Denoise Tile Dispatch Args Buffer");
		m_context->set_object_name(VK_OBJECT_TYPE_BUFFER, (uint64_t) copy_tile_dispatch_args_buffer.vk_buffer, "Copy Tile Dispatch Args Buffer");
	}

	// Create ray traced pass
	{
		// Create shader module
		VkShaderModule shader = VK_NULL_HANDLE;
		{
			VkShaderModuleCreateInfo create_info = {
			    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			    .codeSize = sizeof(g_reflection_raytraced_comp_spv_data),
			    .pCode    = reinterpret_cast<uint32_t *>(g_reflection_raytraced_comp_spv_data),
			};
			vkCreateShaderModule(context.vk_device, &create_info, nullptr, &shader);
		}

		// Create descriptor set layout
		{
			VkDescriptorSetLayoutBinding bindings[] = {
			    // RayTrace image
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
			vkCreateDescriptorSetLayout(m_context->vk_device, &create_info, nullptr, &m_raytrace.descriptor_set_layout);
		}

		// Allocate descriptor set
		{
			VkDescriptorSetLayout descriptor_set_layouts[] = {
			    m_raytrace.descriptor_set_layout,
			};

			VkDescriptorSetAllocateInfo allocate_info = {
			    .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			    .pNext              = nullptr,
			    .descriptorPool     = m_context->vk_descriptor_pool,
			    .descriptorSetCount = 1,
			    .pSetLayouts        = descriptor_set_layouts,
			};
			vkAllocateDescriptorSets(m_context->vk_device, &allocate_info, &m_raytrace.descriptor_set);
		}

		// Create pipeline layout
		{
			VkDescriptorSetLayout descriptor_set_layouts[] = {
			    scene.descriptor.layout,
			    gbuffer_pass.descriptor.layout,
			    blue_noise.descriptor.layout,
			    lut.descriptor.layout,
				raytraced_gi.descriptor.layout,
			    m_raytrace.descriptor_set_layout,
			};
			VkPushConstantRange range = {
			    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			    .offset     = 0,
			    .size       = sizeof(m_raytrace.push_constants),
			};
			VkPipelineLayoutCreateInfo create_info = {
			    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			    .setLayoutCount         = 6,
			    .pSetLayouts            = descriptor_set_layouts,
			    .pushConstantRangeCount = 1,
			    .pPushConstantRanges    = &range,
			};
			vkCreatePipelineLayout(m_context->vk_device, &create_info, nullptr, &m_raytrace.pipeline_layout);
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
			    .layout             = m_raytrace.pipeline_layout,
			    .basePipelineHandle = VK_NULL_HANDLE,
			    .basePipelineIndex  = -1,
			};
			vkCreateComputePipelines(m_context->vk_device, m_context->vk_pipeline_cache, 1, &create_info, nullptr, &m_raytrace.pipeline);
			vkDestroyShaderModule(m_context->vk_device, shader, nullptr);
		}
	}

	// Create reprojection pass
	{
		// Create shader module
		VkShaderModule shader = VK_NULL_HANDLE;
		{
			VkShaderModuleCreateInfo create_info = {
			    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			    .codeSize = sizeof(g_reflection_reprojection_comp_spv_data),
			    .pCode    = reinterpret_cast<uint32_t *>(g_reflection_reprojection_comp_spv_data),
			};
			vkCreateShaderModule(context.vk_device, &create_info, nullptr, &shader);
		}

		// Create descriptor set layout
		{
			VkDescriptorSetLayoutBinding bindings[] = {
			    // Output image
			    {
			        .binding         = 0,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Moments image
			    {
			        .binding         = 1,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Input image
			    {
			        .binding         = 2,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // History output image
			    {
			        .binding         = 3,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // History moments image
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
			vkCreateDescriptorSetLayout(m_context->vk_device, &create_info, nullptr, &m_reprojection.descriptor_set_layout);
		}

		// Allocate descriptor set
		{
			VkDescriptorSetLayout descriptor_set_layouts[] = {
			    m_reprojection.descriptor_set_layout,
			    m_reprojection.descriptor_set_layout,
			};

			VkDescriptorSetAllocateInfo allocate_info = {
			    .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			    .pNext              = nullptr,
			    .descriptorPool     = m_context->vk_descriptor_pool,
			    .descriptorSetCount = 2,
			    .pSetLayouts        = descriptor_set_layouts,
			};
			vkAllocateDescriptorSets(m_context->vk_device, &allocate_info, m_reprojection.descriptor_sets);
		}

		// Create pipeline layout
		{
			VkDescriptorSetLayout descriptor_set_layouts[] = {
			    scene.descriptor.layout,
			    gbuffer_pass.descriptor.layout,
			    m_reprojection.descriptor_set_layout,
			};
			VkPushConstantRange range = {
			    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			    .offset     = 0,
			    .size       = sizeof(m_reprojection.push_constants),
			};
			VkPipelineLayoutCreateInfo create_info = {
			    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			    .setLayoutCount         = 3,
			    .pSetLayouts            = descriptor_set_layouts,
			    .pushConstantRangeCount = 1,
			    .pPushConstantRanges    = &range,
			};
			vkCreatePipelineLayout(m_context->vk_device, &create_info, nullptr, &m_reprojection.pipeline_layout);
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
			    .layout             = m_reprojection.pipeline_layout,
			    .basePipelineHandle = VK_NULL_HANDLE,
			    .basePipelineIndex  = -1,
			};
			vkCreateComputePipelines(m_context->vk_device, m_context->vk_pipeline_cache, 1, &create_info, nullptr, &m_reprojection.pipeline);
			vkDestroyShaderModule(m_context->vk_device, shader, nullptr);
		}
	}

	// Create tile copy pass
	{
		// Create shader module
		VkShaderModule shader = VK_NULL_HANDLE;
		{
			VkShaderModuleCreateInfo create_info = {
			    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			    .codeSize = sizeof(g_reflection_copy_tiles_comp_spv_data),
			    .pCode    = reinterpret_cast<uint32_t *>(g_reflection_copy_tiles_comp_spv_data),
			};
			vkCreateShaderModule(context.vk_device, &create_info, nullptr, &shader);
		}

		// Create descriptor set layout
		{
			VkDescriptorSetLayoutBinding bindings[] = {
			    // Output image
			    {
			        .binding         = 0,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Input image
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
			vkCreateDescriptorSetLayout(m_context->vk_device, &create_info, nullptr, &m_denoise.copy_tiles.descriptor_set_layout);
		}

		// Allocate descriptor set
		{
			VkDescriptorSetLayout descriptor_set_layouts[] = {
			    m_denoise.copy_tiles.descriptor_set_layout,
			    m_denoise.copy_tiles.descriptor_set_layout,
			};

			VkDescriptorSetAllocateInfo allocate_info = {
			    .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			    .pNext              = nullptr,
			    .descriptorPool     = m_context->vk_descriptor_pool,
			    .descriptorSetCount = 2,
			    .pSetLayouts        = descriptor_set_layouts,
			};
			vkAllocateDescriptorSets(m_context->vk_device, &allocate_info, m_denoise.copy_tiles.copy_reprojection_sets);
			vkAllocateDescriptorSets(m_context->vk_device, &allocate_info, m_denoise.copy_tiles.copy_atrous_sets);
		}

		// Create pipeline layout
		{
			VkDescriptorSetLayout descriptor_set_layouts[] = {
			    m_denoise.copy_tiles.descriptor_set_layout,
			};
			VkPushConstantRange range = {
			    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			    .offset     = 0,
			    .size       = sizeof(m_denoise.copy_tiles.push_constants),
			};
			VkPipelineLayoutCreateInfo create_info = {
			    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			    .setLayoutCount         = 1,
			    .pSetLayouts            = descriptor_set_layouts,
			    .pushConstantRangeCount = 1,
			    .pPushConstantRanges    = &range,
			};
			vkCreatePipelineLayout(m_context->vk_device, &create_info, nullptr, &m_denoise.copy_tiles.pipeline_layout);
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
			    .layout             = m_denoise.copy_tiles.pipeline_layout,
			    .basePipelineHandle = VK_NULL_HANDLE,
			    .basePipelineIndex  = -1,
			};
			vkCreateComputePipelines(m_context->vk_device, m_context->vk_pipeline_cache, 1, &create_info, nullptr, &m_denoise.copy_tiles.pipeline);
			vkDestroyShaderModule(m_context->vk_device, shader, nullptr);
		}
	}

	// Create atrous pass
	{
		// Create shader module
		VkShaderModule shader = VK_NULL_HANDLE;
		{
			VkShaderModuleCreateInfo create_info = {
			    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			    .codeSize = sizeof(g_reflection_atrous_comp_spv_data),
			    .pCode    = reinterpret_cast<uint32_t *>(g_reflection_atrous_comp_spv_data),
			};
			vkCreateShaderModule(context.vk_device, &create_info, nullptr, &shader);
		}

		// Create descriptor set layout
		{
			VkDescriptorSetLayoutBinding bindings[] = {
			    // Output image
			    {
			        .binding         = 0,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Input image
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
			vkCreateDescriptorSetLayout(m_context->vk_device, &create_info, nullptr, &m_denoise.a_trous.descriptor_set_layout);
		}

		// Allocate descriptor set
		{
			VkDescriptorSetLayout descriptor_set_layouts[] = {
			    m_denoise.a_trous.descriptor_set_layout,
			    m_denoise.a_trous.descriptor_set_layout,
			};

			VkDescriptorSetAllocateInfo allocate_info = {
			    .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			    .pNext              = nullptr,
			    .descriptorPool     = m_context->vk_descriptor_pool,
			    .descriptorSetCount = 2,
			    .pSetLayouts        = descriptor_set_layouts,
			};
			vkAllocateDescriptorSets(m_context->vk_device, &allocate_info, m_denoise.a_trous.filter_reprojection_sets);
			vkAllocateDescriptorSets(m_context->vk_device, &allocate_info, m_denoise.a_trous.filter_atrous_sets);
		}

		// Create pipeline layout
		{
			VkDescriptorSetLayout descriptor_set_layouts[] = {
			    scene.descriptor.layout,
			    gbuffer_pass.descriptor.layout,
			    m_denoise.a_trous.descriptor_set_layout,
			};
			VkPushConstantRange range = {
			    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			    .offset     = 0,
			    .size       = sizeof(m_denoise.a_trous.push_constants),
			};
			VkPipelineLayoutCreateInfo create_info = {
			    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			    .setLayoutCount         = 3,
			    .pSetLayouts            = descriptor_set_layouts,
			    .pushConstantRangeCount = 1,
			    .pPushConstantRanges    = &range,
			};
			vkCreatePipelineLayout(m_context->vk_device, &create_info, nullptr, &m_denoise.a_trous.pipeline_layout);
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
			    .layout             = m_denoise.a_trous.pipeline_layout,
			    .basePipelineHandle = VK_NULL_HANDLE,
			    .basePipelineIndex  = -1,
			};
			vkCreateComputePipelines(m_context->vk_device, m_context->vk_pipeline_cache, 1, &create_info, nullptr, &m_denoise.a_trous.pipeline);
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
			    .codeSize = sizeof(g_reflection_upsampling_comp_spv_data),
			    .pCode    = reinterpret_cast<uint32_t *>(g_reflection_upsampling_comp_spv_data),
			};
			vkCreateShaderModule(context.vk_device, &create_info, nullptr, &shader);
		}

		// Create descriptor set layout
		{
			VkDescriptorSetLayoutBinding bindings[] = {
			    // Output image
			    {
			        .binding         = 0,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Input image
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
			};

			VkDescriptorSetAllocateInfo allocate_info = {
			    .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			    .pNext              = nullptr,
			    .descriptorPool     = m_context->vk_descriptor_pool,
			    .descriptorSetCount = 1,
			    .pSetLayouts        = descriptor_set_layouts,
			};
			vkAllocateDescriptorSets(m_context->vk_device, &allocate_info, &m_upsampling.descriptor_set);
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
			    .size       = sizeof(m_upsampling.push_constants),
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

RayTracedReflection::~RayTracedReflection()
{
	vkDestroyImageView(m_context->vk_device, raytraced_view, nullptr);
	vkDestroyImageView(m_context->vk_device, reprojection_output_view[0], nullptr);
	vkDestroyImageView(m_context->vk_device, reprojection_output_view[1], nullptr);
	vkDestroyImageView(m_context->vk_device, reprojection_moment_view[0], nullptr);
	vkDestroyImageView(m_context->vk_device, reprojection_moment_view[1], nullptr);
	vkDestroyImageView(m_context->vk_device, a_trous_view[0], nullptr);
	vkDestroyImageView(m_context->vk_device, a_trous_view[1], nullptr);
	vkDestroyImageView(m_context->vk_device, upsampling_view, nullptr);

	vmaDestroyImage(m_context->vma_allocator, raytraced_image.vk_image, raytraced_image.vma_allocation);
	vmaDestroyImage(m_context->vma_allocator, reprojection_output_image[0].vk_image, reprojection_output_image[0].vma_allocation);
	vmaDestroyImage(m_context->vma_allocator, reprojection_output_image[1].vk_image, reprojection_output_image[1].vma_allocation);
	vmaDestroyImage(m_context->vma_allocator, reprojection_moment_image[0].vk_image, reprojection_moment_image[0].vma_allocation);
	vmaDestroyImage(m_context->vma_allocator, reprojection_moment_image[1].vk_image, reprojection_moment_image[1].vma_allocation);
	vmaDestroyImage(m_context->vma_allocator, a_trous_image[0].vk_image, a_trous_image[0].vma_allocation);
	vmaDestroyImage(m_context->vma_allocator, a_trous_image[1].vk_image, a_trous_image[1].vma_allocation);
	vmaDestroyImage(m_context->vma_allocator, upsampling_image.vk_image, upsampling_image.vma_allocation);

	vmaDestroyBuffer(m_context->vma_allocator, denoise_tile_data_buffer.vk_buffer, denoise_tile_data_buffer.vma_allocation);
	vmaDestroyBuffer(m_context->vma_allocator, denoise_tile_dispatch_args_buffer.vk_buffer, denoise_tile_dispatch_args_buffer.vma_allocation);
	vmaDestroyBuffer(m_context->vma_allocator, copy_tile_data_buffer.vk_buffer, copy_tile_data_buffer.vma_allocation);
	vmaDestroyBuffer(m_context->vma_allocator, copy_tile_dispatch_args_buffer.vk_buffer, copy_tile_dispatch_args_buffer.vma_allocation);

	vkDestroyDescriptorSetLayout(m_context->vk_device, m_raytrace.descriptor_set_layout, nullptr);
	vkDestroyDescriptorSetLayout(m_context->vk_device, m_reprojection.descriptor_set_layout, nullptr);
	vkDestroyDescriptorSetLayout(m_context->vk_device, m_denoise.copy_tiles.descriptor_set_layout, nullptr);
	vkDestroyDescriptorSetLayout(m_context->vk_device, m_denoise.a_trous.descriptor_set_layout, nullptr);
	vkDestroyDescriptorSetLayout(m_context->vk_device, m_upsampling.descriptor_set_layout, nullptr);

	vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 1, &m_raytrace.descriptor_set);
	vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 2, m_reprojection.descriptor_sets);
	vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 2, m_denoise.copy_tiles.copy_reprojection_sets);
	vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 2, m_denoise.copy_tiles.copy_atrous_sets);
	vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 2, m_denoise.a_trous.filter_reprojection_sets);
	vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 2, m_denoise.a_trous.filter_atrous_sets);
	vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 1, &m_upsampling.descriptor_set);

	vkDestroyPipelineLayout(m_context->vk_device, m_raytrace.pipeline_layout, nullptr);
	vkDestroyPipelineLayout(m_context->vk_device, m_reprojection.pipeline_layout, nullptr);
	vkDestroyPipelineLayout(m_context->vk_device, m_denoise.copy_tiles.pipeline_layout, nullptr);
	vkDestroyPipelineLayout(m_context->vk_device, m_denoise.a_trous.pipeline_layout, nullptr);
	vkDestroyPipelineLayout(m_context->vk_device, m_upsampling.pipeline_layout, nullptr);

	vkDestroyPipeline(m_context->vk_device, m_raytrace.pipeline, nullptr);
	vkDestroyPipeline(m_context->vk_device, m_reprojection.pipeline, nullptr);
	vkDestroyPipeline(m_context->vk_device, m_denoise.copy_tiles.pipeline, nullptr);
	vkDestroyPipeline(m_context->vk_device, m_denoise.a_trous.pipeline, nullptr);
	vkDestroyPipeline(m_context->vk_device, m_upsampling.pipeline, nullptr);
}

void RayTracedReflection::init(VkCommandBuffer cmd_buffer)
{
	{
		VkImageMemoryBarrier image_barriers[] = {
		    {
		        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask       = 0,
		        .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
		        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
		        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image               = reprojection_output_image[m_context->ping_pong].vk_image,
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
		        .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
		        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
		        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image               = reprojection_output_image[!m_context->ping_pong].vk_image,
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
		        .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
		        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
		        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image               = reprojection_moment_image[m_context->ping_pong].vk_image,
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
		        .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
		        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
		        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image               = reprojection_moment_image[!m_context->ping_pong].vk_image,
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
		        .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
		        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
		        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image               = a_trous_image[1].vk_image,
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
		        .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
		        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
		        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image               = a_trous_image[0].vk_image,
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

	VkClearColorValue clear_value = {
	    .float32 = {0.f, 0.f, 0.f, 0.f},
	};

	VkImageSubresourceRange range = {
	    .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	    .baseMipLevel   = 0,
	    .levelCount     = 1,
	    .baseArrayLayer = 0,
	    .layerCount     = 1,
	};

	vkCmdClearColorImage(cmd_buffer, reprojection_output_image[0].vk_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_value, 1, &range);
	vkCmdClearColorImage(cmd_buffer, reprojection_output_image[1].vk_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_value, 1, &range);
	vkCmdClearColorImage(cmd_buffer, reprojection_moment_image[0].vk_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_value, 1, &range);
	vkCmdClearColorImage(cmd_buffer, reprojection_moment_image[1].vk_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_value, 1, &range);
	vkCmdClearColorImage(cmd_buffer, a_trous_image[0].vk_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_value, 1, &range);
	vkCmdClearColorImage(cmd_buffer, a_trous_image[1].vk_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_value, 1, &range);

	{
		VkBufferMemoryBarrier buffer_barriers[] = {
		    {
		        .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		        .pNext               = nullptr,
		        .srcAccessMask       = 0,
		        .dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .buffer              = denoise_tile_data_buffer.vk_buffer,
		        .offset              = 0,
		        .size                = VK_WHOLE_SIZE,
		    },
		    {
		        .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		        .pNext               = nullptr,
		        .srcAccessMask       = 0,
		        .dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .buffer              = denoise_tile_dispatch_args_buffer.vk_buffer,
		        .offset              = 0,
		        .size                = VK_WHOLE_SIZE,
		    },
		    {
		        .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		        .pNext               = nullptr,
		        .srcAccessMask       = 0,
		        .dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .buffer              = copy_tile_data_buffer.vk_buffer,
		        .offset              = 0,
		        .size                = VK_WHOLE_SIZE,
		    },
		    {
		        .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		        .pNext               = nullptr,
		        .srcAccessMask       = 0,
		        .dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .buffer              = copy_tile_dispatch_args_buffer.vk_buffer,
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
		        .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
		        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
		        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image               = reprojection_output_image[m_context->ping_pong].vk_image,
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
		        .image               = reprojection_output_image[!m_context->ping_pong].vk_image,
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
		        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
		        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image               = reprojection_moment_image[m_context->ping_pong].vk_image,
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
		        .image               = reprojection_moment_image[!m_context->ping_pong].vk_image,
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
		        .image               = a_trous_image[0].vk_image,
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
		        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
		        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image               = a_trous_image[1].vk_image,
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
		        .image               = upsampling_image.vk_image,
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
		    0, 0, nullptr, 4, buffer_barriers, 8, image_barriers);
	}
}

void RayTracedReflection::update(const Scene &scene, const GBufferPass &gbuffer_pass, const BlueNoise &blue_noise, const LUT &lut)
{
	VkDescriptorImageInfo raytrace_image_info[2] = {
	    {
	        .sampler     = VK_NULL_HANDLE,
	        .imageView   = raytraced_view,
	        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	    },
	    {
	        .sampler     = scene.linear_sampler,
	        .imageView   = raytraced_view,
	        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    },
	};

	VkDescriptorImageInfo reprojection_image_info[2][2] = {
	    {
	        {
	            .sampler     = VK_NULL_HANDLE,
	            .imageView   = reprojection_output_view[0],
	            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	        },
	        {
	            .sampler     = VK_NULL_HANDLE,
	            .imageView   = reprojection_output_view[1],
	            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	        },
	    },
	    {
	        {
	            .sampler     = scene.linear_sampler,
	            .imageView   = reprojection_output_view[0],
	            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        },
	        {
	            .sampler     = scene.linear_sampler,
	            .imageView   = reprojection_output_view[1],
	            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        },
	    },
	};

	VkDescriptorImageInfo reprojection_moments_info[2][2] = {
	    {
	        {
	            .sampler     = VK_NULL_HANDLE,
	            .imageView   = reprojection_moment_view[0],
	            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	        },
	        {
	            .sampler     = VK_NULL_HANDLE,
	            .imageView   = reprojection_moment_view[1],
	            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	        },
	    },
	    {
	        {
	            .sampler     = scene.linear_sampler,
	            .imageView   = reprojection_moment_view[0],
	            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        },
	        {
	            .sampler     = scene.linear_sampler,
	            .imageView   = reprojection_moment_view[1],
	            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        },
	    },
	};

	VkDescriptorImageInfo atrous_image_info[2][2] = {
	    {
	        {
	            .sampler     = VK_NULL_HANDLE,
	            .imageView   = a_trous_view[0],
	            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	        },
	        {
	            .sampler     = VK_NULL_HANDLE,
	            .imageView   = a_trous_view[1],
	            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	        },
	    },
	    {
	        {
	            .sampler     = scene.linear_sampler,
	            .imageView   = a_trous_view[0],
	            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        },
	        {
	            .sampler     = scene.linear_sampler,
	            .imageView   = a_trous_view[1],
	            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        },
	    },
	};

	VkDescriptorImageInfo upsampling_image_info[2] = {
	    {
	        .sampler     = VK_NULL_HANDLE,
	        .imageView   = upsampling_view,
	        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	    },
	    {
	        .sampler     = scene.linear_sampler,
	        .imageView   = upsampling_view,
	        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    },
	};

	// Update raytrace pass descriptor
	{
		VkWriteDescriptorSet writes[] = {
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_raytrace.descriptor_set,
		        .dstBinding       = 0,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		        .pImageInfo       = &raytrace_image_info[0],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		};
		vkUpdateDescriptorSets(m_context->vk_device, 1, writes, 0, nullptr);
	}

	// Update reprojection pass descriptor
	for (uint32_t i = 0; i < 2; i++)
	{
		VkWriteDescriptorSet writes[] = {
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_reprojection.descriptor_sets[i],
		        .dstBinding       = 0,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		        .pImageInfo       = &reprojection_image_info[0][i],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_reprojection.descriptor_sets[i],
		        .dstBinding       = 1,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		        .pImageInfo       = &reprojection_moments_info[0][i],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_reprojection.descriptor_sets[i],
		        .dstBinding       = 2,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .pImageInfo       = &raytrace_image_info[1],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_reprojection.descriptor_sets[i],
		        .dstBinding       = 3,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .pImageInfo       = &reprojection_image_info[1][!i],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_reprojection.descriptor_sets[i],
		        .dstBinding       = 4,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .pImageInfo       = &reprojection_moments_info[1][!i],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		};
		vkUpdateDescriptorSets(m_context->vk_device, 5, writes, 0, nullptr);
	}

	// Update copy tile pass descriptor
	for (uint32_t i = 0; i < 2; i++)
	{
		VkWriteDescriptorSet writes[] = {
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_denoise.copy_tiles.copy_reprojection_sets[i],
		        .dstBinding       = 0,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		        .pImageInfo       = &atrous_image_info[0][0],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_denoise.copy_tiles.copy_reprojection_sets[i],
		        .dstBinding       = 1,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .pImageInfo       = &reprojection_image_info[1][i],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		};
		vkUpdateDescriptorSets(m_context->vk_device, 2, writes, 0, nullptr);
	}

	for (uint32_t i = 0; i < 2; i++)
	{
		VkWriteDescriptorSet writes[] = {
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_denoise.copy_tiles.copy_atrous_sets[i],
		        .dstBinding       = 0,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		        .pImageInfo       = &atrous_image_info[0][i],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_denoise.copy_tiles.copy_atrous_sets[i],
		        .dstBinding       = 1,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .pImageInfo       = &atrous_image_info[1][!i],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		};
		vkUpdateDescriptorSets(m_context->vk_device, 2, writes, 0, nullptr);
	}

	// Update atrous pass descriptor
	for (uint32_t i = 0; i < 2; i++)
	{
		VkWriteDescriptorSet writes[] = {
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_denoise.a_trous.filter_reprojection_sets[i],
		        .dstBinding       = 0,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		        .pImageInfo       = &atrous_image_info[0][0],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_denoise.a_trous.filter_reprojection_sets[i],
		        .dstBinding       = 1,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .pImageInfo       = &reprojection_image_info[1][i],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		};
		vkUpdateDescriptorSets(m_context->vk_device, 2, writes, 0, nullptr);
	}

	for (uint32_t i = 0; i < 2; i++)
	{
		VkWriteDescriptorSet writes[] = {
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_denoise.a_trous.filter_atrous_sets[i],
		        .dstBinding       = 0,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		        .pImageInfo       = &atrous_image_info[0][i],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_denoise.a_trous.filter_atrous_sets[i],
		        .dstBinding       = 1,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .pImageInfo       = &atrous_image_info[1][!i],
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
		        .dstSet           = m_upsampling.descriptor_set,
		        .dstBinding       = 0,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		        .pImageInfo       = &upsampling_image_info[0],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_upsampling.descriptor_set,
		        .dstBinding       = 1,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .pImageInfo       = &atrous_image_info[1][0],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		};
		vkUpdateDescriptorSets(m_context->vk_device, 2, writes, 0, nullptr);
	}
}

void RayTracedReflection::draw(VkCommandBuffer cmd_buffer, const Scene &scene, const GBufferPass &gbuffer_pass, const BlueNoise &blue_noise, const LUT &lut, const RayTracedGI &raytraced_gi)
{
	m_context->begin_marker(cmd_buffer, "Raytraced Reflection");
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

		m_context->begin_marker(cmd_buffer, "Reflection - Ray Traced");
		{
			VkDescriptorSet descriptors[] = {
			    scene.descriptor.set,
			    gbuffer_pass.descriptor.sets[m_context->ping_pong],
			    blue_noise.descriptor.set,
			    lut.descriptor.set,
			    raytraced_gi.descriptor.sets[m_context->ping_pong],
			    m_raytrace.descriptor_set,
			};
			m_raytrace.push_constants.gbuffer_mip = m_gbuffer_mip;
			vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_raytrace.pipeline_layout, 0, 6, descriptors, 0, nullptr);
			vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_raytrace.pipeline);
			vkCmdPushConstants(cmd_buffer, m_raytrace.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(m_raytrace.push_constants), &m_raytrace.push_constants);
			vkCmdDispatch(cmd_buffer, static_cast<uint32_t>(ceil(float(m_width) / float(NUM_THREADS_X))), static_cast<uint32_t>(ceil(float(m_height) / float(NUM_THREADS_Y))), 1);
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
			        .image               = reprojection_output_image[m_context->ping_pong].vk_image,
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
			        .image               = reprojection_output_image[!m_context->ping_pong].vk_image,
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
			        .image               = reprojection_moment_image[m_context->ping_pong].vk_image,
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
			        .image               = reprojection_moment_image[!m_context->ping_pong].vk_image,
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
			        .image               = upsampling_image.vk_image,
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
			    0, 0, nullptr, 0, nullptr, 6, image_barriers);
		}

		m_context->begin_marker(cmd_buffer, "Reflection - Reprojection");
		{
			VkDescriptorSet descriptors[] = {
			    scene.descriptor.set,
			    gbuffer_pass.descriptor.sets[m_context->ping_pong],
			    m_reprojection.descriptor_sets[m_context->ping_pong],
			};
			m_reprojection.push_constants.gbuffer_mip                     = m_gbuffer_mip;
			m_reprojection.push_constants.denoise_tile_data_addr          = denoise_tile_data_buffer.device_address;
			m_reprojection.push_constants.denoise_tile_dispatch_args_addr = denoise_tile_dispatch_args_buffer.device_address;
			m_reprojection.push_constants.copy_tile_data_addr             = copy_tile_data_buffer.device_address;
			m_reprojection.push_constants.copy_tile_dispatch_args_addr    = copy_tile_dispatch_args_buffer.device_address;
			vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_reprojection.pipeline_layout, 0, 3, descriptors, 0, nullptr);
			vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_reprojection.pipeline);
			vkCmdPushConstants(cmd_buffer, m_reprojection.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(m_reprojection.push_constants), &m_reprojection.push_constants);
			vkCmdDispatch(cmd_buffer, static_cast<uint32_t>(ceil(float(m_width) / float(NUM_THREADS_X))), static_cast<uint32_t>(ceil(float(m_height) / float(NUM_THREADS_Y))), 1);
		}
		m_context->end_marker(cmd_buffer);

		{
			VkBufferMemoryBarrier buffer_barriers[] = {
			    {
			        .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			        .pNext               = nullptr,
			        .srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
			        .dstAccessMask       = VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
			        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .buffer              = copy_tile_dispatch_args_buffer.vk_buffer,
			        .offset              = 0,
			        .size                = VK_WHOLE_SIZE,
			    },
			    {
			        .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			        .pNext               = nullptr,
			        .srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
			        .dstAccessMask       = VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
			        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .buffer              = denoise_tile_dispatch_args_buffer.vk_buffer,
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
			        .buffer              = copy_tile_data_buffer.vk_buffer,
			        .offset              = 0,
			        .size                = VK_WHOLE_SIZE,
			    },
			    {
			        .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			        .pNext               = nullptr,
			        .srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
			        .dstAccessMask       = VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
			        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .buffer              = denoise_tile_data_buffer.vk_buffer,
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
			        .image               = reprojection_output_image[m_context->ping_pong].vk_image,
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
			        .image               = reprojection_output_image[!m_context->ping_pong].vk_image,
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
			        .image               = reprojection_moment_image[m_context->ping_pong].vk_image,
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
			        .image               = reprojection_moment_image[!m_context->ping_pong].vk_image,
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
			    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
			    0, 0, nullptr, 4, buffer_barriers, 4, image_barriers);
		}

		m_context->begin_marker(cmd_buffer, "Reflection - Denoise");
		{
			bool ping_pong = true;
			for (uint32_t i = 0; i < 3; i++)
			{
				m_context->begin_marker(cmd_buffer, (std::string("Iteration - ") + std::to_string(i)).c_str());
				{
					m_context->begin_marker(cmd_buffer, "Copy Tile Data");
					{
						VkDescriptorSet descriptors[] = {
						    i == 0 ?
						        m_denoise.copy_tiles.copy_reprojection_sets[m_context->ping_pong] :
						        m_denoise.copy_tiles.copy_atrous_sets[!ping_pong],
						};
						m_denoise.copy_tiles.push_constants.copy_tile_data_addr = copy_tile_data_buffer.device_address;
						vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_denoise.copy_tiles.pipeline_layout, 0, 1, descriptors, 0, nullptr);
						vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_denoise.copy_tiles.pipeline);
						vkCmdPushConstants(cmd_buffer, m_denoise.copy_tiles.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(m_denoise.copy_tiles.push_constants), &m_denoise.copy_tiles.push_constants);
						vkCmdDispatchIndirect(cmd_buffer, copy_tile_dispatch_args_buffer.vk_buffer, 0);
					}
					m_context->end_marker(cmd_buffer);
					m_context->begin_marker(cmd_buffer, "Atrous Filter");
					{
						VkDescriptorSet descriptors[] = {
						    scene.descriptor.set,
						    gbuffer_pass.descriptor.sets[ping_pong],
						    i == 0 ?
						        m_denoise.a_trous.filter_reprojection_sets[m_context->ping_pong] :
						        m_denoise.a_trous.filter_atrous_sets[!ping_pong],
						};
						m_denoise.a_trous.push_constants.denoise_tile_data_addr = denoise_tile_data_buffer.device_address;
						m_denoise.a_trous.push_constants.gbuffer_mip            = m_gbuffer_mip;
						m_denoise.a_trous.push_constants.step_size              = 1 << i;
						vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_denoise.a_trous.pipeline_layout, 0, 3, descriptors, 0, nullptr);
						vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_denoise.a_trous.pipeline);
						vkCmdPushConstants(cmd_buffer, m_denoise.a_trous.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(m_denoise.a_trous.push_constants), &m_denoise.a_trous.push_constants);
						vkCmdDispatchIndirect(cmd_buffer, denoise_tile_dispatch_args_buffer.vk_buffer, 0);
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
						        .image               = a_trous_image[ping_pong].vk_image,
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
						        .image               = a_trous_image[!ping_pong].vk_image,
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

				ping_pong = !ping_pong;

				m_context->end_marker(cmd_buffer);
			}
		}
		m_context->end_marker(cmd_buffer);

		m_context->begin_marker(cmd_buffer, "Reflection - Upsampling");
		{
			VkDescriptorSet descriptors[] = {
			    scene.descriptor.set,
			    gbuffer_pass.descriptor.sets[m_context->ping_pong],
			    m_upsampling.descriptor_set,
			};
			m_upsampling.push_constants.gbuffer_mip = m_gbuffer_mip;

			vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_upsampling.pipeline_layout, 0, 3, descriptors, 0, nullptr);
			vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_upsampling.pipeline);
			vkCmdPushConstants(cmd_buffer, m_upsampling.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(m_upsampling.push_constants), &m_upsampling.push_constants);
			vkCmdDispatch(cmd_buffer, static_cast<uint32_t>(ceil(float(m_context->extent.width) / float(NUM_THREADS_X))), static_cast<uint32_t>(ceil(float(m_context->extent.height) / float(NUM_THREADS_Y))), 1);
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
			        .image               = a_trous_image[0].vk_image,
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
			        .image               = a_trous_image[1].vk_image,
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

		{
			VkBufferMemoryBarrier buffer_barriers[] = {
			    {
			        .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			        .pNext               = nullptr,
			        .srcAccessMask       = VK_ACCESS_SHADER_READ_BIT,
			        .dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
			        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .buffer              = denoise_tile_data_buffer.vk_buffer,
			        .offset              = 0,
			        .size                = VK_WHOLE_SIZE,
			    },
			    {
			        .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			        .pNext               = nullptr,
			        .srcAccessMask       = VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
			        .dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
			        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .buffer              = denoise_tile_dispatch_args_buffer.vk_buffer,
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
			        .buffer              = copy_tile_data_buffer.vk_buffer,
			        .offset              = 0,
			        .size                = VK_WHOLE_SIZE,
			    },
			    {
			        .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			        .pNext               = nullptr,
			        .srcAccessMask       = VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
			        .dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
			        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .buffer              = copy_tile_dispatch_args_buffer.vk_buffer,
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
			        .image               = reprojection_output_image[m_context->ping_pong].vk_image,
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
			        .image               = reprojection_output_image[!m_context->ping_pong].vk_image,
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
			        .image               = reprojection_moment_image[m_context->ping_pong].vk_image,
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
			        .image               = reprojection_moment_image[!m_context->ping_pong].vk_image,
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
			        .image               = upsampling_image.vk_image,
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
			    0, 0, nullptr, 4, buffer_barriers, 5, image_barriers);
		}
	}
	m_context->end_marker(cmd_buffer);
}

bool RayTracedReflection::draw_ui()
{
	return false;
}
