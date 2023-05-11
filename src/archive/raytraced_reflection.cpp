#include "render/pipeline/raytraced_reflection.hpp"

RayTracedReflection::RayTracedReflection(const Context &context, RayTracedScale scale) :
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
		vkCreateImageView(context.vk_device, &view_create_info, nullptr, &raytraced_image_view);
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) raytraced_image.vk_image, "RayTraced Reflection Image");
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) raytraced_image_view, "RayTraced Reflection Image View");
	}

	// Create ray trace pass
	//{
	//	// Create shader module
	//	VkShaderModule shader = VK_NULL_HANDLE;
	//	{
	//		VkShaderModuleCreateInfo create_info = {
	//		    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	//		    .codeSize = sizeof(g_ao_raytraced_comp_spv_data),
	//		    .pCode    = reinterpret_cast<uint32_t *>(g_ao_raytraced_comp_spv_data),
	//		};
	//		vkCreateShaderModule(context.vk_device, &create_info, nullptr, &shader);
	//	}

	//	// Create descriptor set layout
	//	{
	//		VkDescriptorSetLayoutBinding bindings[] = {
	//		    // Global buffer
	//		    {
	//		        .binding         = 0,
	//		        .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
	//		        .descriptorCount = 1,
	//		        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
	//		    },
	//		    // Raytraced image
	//		    {
	//		        .binding         = 1,
	//		        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
	//		        .descriptorCount = 1,
	//		        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
	//		    },
	//		    // GBufferB
	//		    {
	//		        .binding         = 2,
	//		        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	//		        .descriptorCount = 1,
	//		        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
	//		    },
	//		    // Depth Stencil
	//		    {
	//		        .binding         = 3,
	//		        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	//		        .descriptorCount = 1,
	//		        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
	//		    },
	//		    // Sobol Sequence
	//		    {
	//		        .binding         = 4,
	//		        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	//		        .descriptorCount = 1,
	//		        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
	//		    },
	//		    // Scrambling Ranking Tile
	//		    {
	//		        .binding         = 5,
	//		        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	//		        .descriptorCount = 1,
	//		        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
	//		    },
	//		    // Top Levell Acceleration Structure
	//		    {
	//		        .binding         = 6,
	//		        .descriptorType  = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
	//		        .descriptorCount = 1,
	//		        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
	//		    },
	//		};
	//		VkDescriptorSetLayoutCreateInfo create_info = {
	//		    .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
	//		    .flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
	//		    .bindingCount = 7,
	//		    .pBindings    = bindings,
	//		};
	//		vkCreateDescriptorSetLayout(m_context->vk_device, &create_info, nullptr, &m_raytraced.descriptor_set_layout);
	//	}

	//	// Allocate descriptor set
	//	{
	//		VkDescriptorSetLayout descriptor_set_layouts[] = {
	//		    m_raytraced.descriptor_set_layout,
	//		    m_raytraced.descriptor_set_layout,
	//		};

	//		VkDescriptorSetAllocateInfo allocate_info = {
	//		    .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
	//		    .pNext              = nullptr,
	//		    .descriptorPool     = m_context->vk_descriptor_pool,
	//		    .descriptorSetCount = 2,
	//		    .pSetLayouts        = descriptor_set_layouts,
	//		};
	//		vkAllocateDescriptorSets(m_context->vk_device, &allocate_info, m_raytraced.descriptor_sets);
	//	}

	//	// Create pipeline layout
	//	{
	//		VkPushConstantRange range = {
	//		    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
	//		    .offset     = 0,
	//		    .size       = sizeof(m_raytraced.push_constant),
	//		};
	//		VkPipelineLayoutCreateInfo create_info = {
	//		    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	//		    .setLayoutCount         = 1,
	//		    .pSetLayouts            = &m_raytraced.descriptor_set_layout,
	//		    .pushConstantRangeCount = 1,
	//		    .pPushConstantRanges    = &range,
	//		};
	//		vkCreatePipelineLayout(m_context->vk_device, &create_info, nullptr, &m_raytraced.pipeline_layout);
	//	}

	//	// Create pipeline
	//	{
	//		VkComputePipelineCreateInfo create_info = {
	//		    .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
	//		    .stage = {
	//		        .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	//		        .stage               = VK_SHADER_STAGE_COMPUTE_BIT,
	//		        .module              = shader,
	//		        .pName               = "main",
	//		        .pSpecializationInfo = nullptr,
	//		    },
	//		    .layout             = m_raytraced.pipeline_layout,
	//		    .basePipelineHandle = VK_NULL_HANDLE,
	//		    .basePipelineIndex  = -1,
	//		};
	//		vkCreateComputePipelines(m_context->vk_device, m_context->vk_pipeline_cache, 1, &create_info, nullptr, &m_raytraced.pipeline);
	//		vkDestroyShaderModule(m_context->vk_device, shader, nullptr);
	//	}
	//}
}

RayTracedReflection::~RayTracedReflection()
{
}

void RayTracedReflection::init(VkCommandBuffer cmd_buffer)
{
}

void RayTracedReflection::update(const Scene &scene, const BlueNoise &blue_noise, const GBufferPass &gbuffer_pass)
{
}

void RayTracedReflection::draw(VkCommandBuffer cmd_buffer)
{
}

bool RayTracedReflection::draw_ui()
{
	return false;
}
