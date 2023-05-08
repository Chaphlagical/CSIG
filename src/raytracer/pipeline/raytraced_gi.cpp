#include "render/pipeline/raytraced_gi.hpp"

static const uint32_t NUM_THREADS_X = 8;
static const uint32_t NUM_THREADS_Y = 8;

static unsigned char g_gi_raytraced_comp_spv_data[] = {
#include "gi_raytraced.comp.spv.h"
};

static unsigned char g_gi_probe_update_irradiance_comp_spv_data[] = {
#include "gi_probe_update.comp.spv.h"
};

static unsigned char g_gi_probe_update_depth_comp_spv_data[] = {
#include "gi_probe_update_depth.comp.spv.h"
};

RayTracedGI::RayTracedGI(const Context &context, RayTracedScale scale) :
    m_context(&context)
{
	float scale_divisor = std::powf(2.0f, float(scale));

	m_width  = m_context->extent.width / static_cast<uint32_t>(scale_divisor);
	m_height = m_context->extent.height / static_cast<uint32_t>(scale_divisor);

	m_gbuffer_mip = static_cast<uint32_t>(scale);

	// Create ray trace pass
	{
		// Create shader module
		VkShaderModule shader = VK_NULL_HANDLE;
		{
			VkShaderModuleCreateInfo create_info = {
			    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			    .codeSize = sizeof(g_gi_raytraced_comp_spv_data),
			    .pCode    = reinterpret_cast<uint32_t *>(g_gi_raytraced_comp_spv_data),
			};
			vkCreateShaderModule(m_context->vk_device, &create_info, nullptr, &shader);
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
			    VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
			    0,
			    0,
			    0,
			    0,
			    0,
			};
			VkDescriptorSetLayoutBinding bindings[] = {
			    // Global buffer
			    {
			        .binding         = 0,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Vertex buffer
			    {
			        .binding         = 1,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Index buffer
			    {
			        .binding         = 2,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Material buffer
			    {
			        .binding         = 3,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Emitter buffer
			    {
			        .binding         = 4,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Instance buffer
			    {
			        .binding         = 5,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Scene buffer
			    {
			        .binding         = 6,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // DDGI uniform buffer
			    {
			        .binding         = 7,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // TLAS
			    {
			        .binding         = 8,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Textures
			    {
			        .binding         = 9,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1024,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Skybox
			    {
			        .binding         = 10,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Radiance
			    {
			        .binding         = 11,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Direction Depth
			    {
			        .binding         = 12,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Probe Irradiance
			    {
			        .binding         = 13,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Probe Depth
			    {
			        .binding         = 14,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			};
			VkDescriptorSetLayoutBindingFlagsCreateInfo descriptor_set_layout_binding_flag_create_info = {
			    .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
			    .bindingCount  = 15,
			    .pBindingFlags = descriptor_binding_flags,
			};
			VkDescriptorSetLayoutCreateInfo create_info = {
			    .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			    .pNext        = &descriptor_set_layout_binding_flag_create_info,
			    .flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
			    .bindingCount = 15,
			    .pBindings    = bindings,
			};
			vkCreateDescriptorSetLayout(m_context->vk_device, &create_info, nullptr, &m_raytraced.descriptor_set_layout);
		}

		// Allocate descriptor set
		{
			VkDescriptorSetLayout layouts[] = {
			    m_raytraced.descriptor_set_layout,
			    m_raytraced.descriptor_set_layout,
			};
			VkDescriptorSetAllocateInfo allocate_info = {
			    .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			    .pNext              = nullptr,
			    .descriptorPool     = m_context->vk_descriptor_pool,
			    .descriptorSetCount = 2,
			    .pSetLayouts        = layouts,
			};
			vkAllocateDescriptorSets(m_context->vk_device, &allocate_info, m_raytraced.descriptor_sets);
		}

		// Create pipeline layout
		{
			VkPushConstantRange range = {
			    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			    .offset     = 0,
			    .size       = sizeof(m_raytraced.push_constants),
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

	// Create probe update pass
	{
		// Create shader module
		VkShaderModule update_irradiance_shader = VK_NULL_HANDLE;
		{
			VkShaderModuleCreateInfo create_info = {
			    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			    .codeSize = sizeof(g_gi_probe_update_irradiance_comp_spv_data),
			    .pCode    = reinterpret_cast<uint32_t *>(g_gi_probe_update_irradiance_comp_spv_data),
			};
			vkCreateShaderModule(m_context->vk_device, &create_info, nullptr, &update_irradiance_shader);
		}
		VkShaderModule update_depth_shader = VK_NULL_HANDLE;
		{
			VkShaderModuleCreateInfo create_info = {
			    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			    .codeSize = sizeof(g_gi_probe_update_depth_comp_spv_data),
			    .pCode    = reinterpret_cast<uint32_t *>(g_gi_probe_update_depth_comp_spv_data),
			};
			vkCreateShaderModule(m_context->vk_device, &create_info, nullptr, &update_depth_shader);
		}

		// Create descriptor set layout
		{
			VkDescriptorSetLayoutBinding bindings[] = {
			    // Output irradiance
			    {
			        .binding         = 0,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Output depth
			    {
			        .binding         = 1,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Input irradiance
			    {
			        .binding         = 2,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Input depth
			    {
			        .binding         = 3,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Input radiance
			    {
			        .binding         = 4,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Input direction depth
			    {
			        .binding         = 5,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // DDGI uniform buffer
			    {
			        .binding         = 6,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			};
			VkDescriptorSetLayoutCreateInfo create_info = {
			    .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			    .bindingCount = 7,
			    .pBindings    = bindings,
			};
			vkCreateDescriptorSetLayout(m_context->vk_device, &create_info, nullptr, &m_probe_update.update_probe.descriptor_set_layout);
		}

		// Allocate descriptor set
		{
			VkDescriptorSetLayout layouts[] = {
			    m_probe_update.update_probe.descriptor_set_layout,
			    m_probe_update.update_probe.descriptor_set_layout,
			};
			VkDescriptorSetAllocateInfo allocate_info = {
			    .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			    .pNext              = nullptr,
			    .descriptorPool     = m_context->vk_descriptor_pool,
			    .descriptorSetCount = 2,
			    .pSetLayouts        = layouts,
			};
			vkAllocateDescriptorSets(m_context->vk_device, &allocate_info, m_probe_update.update_probe.descriptor_sets);
		}

		// Create pipeline layout
		{
			VkPushConstantRange range = {
			    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			    .offset     = 0,
			    .size       = sizeof(m_probe_update.update_probe.push_constants),
			};
			VkPipelineLayoutCreateInfo create_info = {
			    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			    .setLayoutCount         = 1,
			    .pSetLayouts            = &m_probe_update.update_probe.descriptor_set_layout,
			    .pushConstantRangeCount = 1,
			    .pPushConstantRanges    = &range,
			};
			vkCreatePipelineLayout(m_context->vk_device, &create_info, nullptr, &m_probe_update.update_probe.pipeline_layout);
		}

		// Create pipeline
		{
			VkComputePipelineCreateInfo create_info = {
			    .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			    .stage = {
			        .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			        .stage               = VK_SHADER_STAGE_COMPUTE_BIT,
			        .module              = update_irradiance_shader,
			        .pName               = "main",
			        .pSpecializationInfo = nullptr,
			    },
			    .layout             = m_probe_update.update_probe.pipeline_layout,
			    .basePipelineHandle = VK_NULL_HANDLE,
			    .basePipelineIndex  = -1,
			};
			vkCreateComputePipelines(m_context->vk_device, m_context->vk_pipeline_cache, 1, &create_info, nullptr, &m_probe_update.update_probe.irradiance_pipeline);
			vkDestroyShaderModule(m_context->vk_device, update_irradiance_shader, nullptr);
		}
		{
			VkComputePipelineCreateInfo create_info = {
			    .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			    .stage = {
			        .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			        .stage               = VK_SHADER_STAGE_COMPUTE_BIT,
			        .module              = update_depth_shader,
			        .pName               = "main",
			        .pSpecializationInfo = nullptr,
			    },
			    .layout             = m_probe_update.update_probe.pipeline_layout,
			    .basePipelineHandle = VK_NULL_HANDLE,
			    .basePipelineIndex  = -1,
			};
			vkCreateComputePipelines(m_context->vk_device, m_context->vk_pipeline_cache, 1, &create_info, nullptr, &m_probe_update.update_probe.depth_pipeline);
			vkDestroyShaderModule(m_context->vk_device, update_depth_shader, nullptr);
		}
	}
}

RayTracedGI::~RayTracedGI()
{
	destroy_resource();

	vkDestroyPipelineLayout(m_context->vk_device, m_raytraced.pipeline_layout, nullptr);
	vkDestroyPipelineLayout(m_context->vk_device, m_probe_update.update_probe.pipeline_layout, nullptr);

	vkDestroyPipeline(m_context->vk_device, m_raytraced.pipeline, nullptr);
	vkDestroyPipeline(m_context->vk_device, m_probe_update.update_probe.irradiance_pipeline, nullptr);
	vkDestroyPipeline(m_context->vk_device, m_probe_update.update_probe.depth_pipeline, nullptr);

	vkDestroyDescriptorSetLayout(m_context->vk_device, m_raytraced.descriptor_set_layout, nullptr);
	vkDestroyDescriptorSetLayout(m_context->vk_device, m_probe_update.update_probe.descriptor_set_layout, nullptr);

	vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 2, m_raytraced.descriptor_sets);
	vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 2, m_probe_update.update_probe.descriptor_sets);
}

void RayTracedGI::init(VkCommandBuffer cmd_buffer)
{
	if (!m_init)
	{
		return;
	}

	VkBufferMemoryBarrier buffer_barriers[] = {
	    {
	        .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
	        .pNext               = nullptr,
	        .srcAccessMask       = 0,
	        .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .buffer              = uniform_buffer.vk_buffer,
	        .offset              = 0,
	        .size                = sizeof(UBO),
	    },
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
	        .image               = radiance_image.vk_image,
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
	        .image               = direction_depth_image.vk_image,
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
	        .image               = probe_grid_irradiance_image[0].vk_image,
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
	        .image               = probe_grid_depth_image[0].vk_image,
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
	        .image               = probe_grid_irradiance_image[1].vk_image,
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
	        .image               = probe_grid_depth_image[1].vk_image,
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
	    0, 0, nullptr, 1, buffer_barriers, 6, image_barriers);
}

void RayTracedGI::update(const Scene &scene, const BlueNoise &blue_noise, const GBufferPass &gbuffer_pass)
{
	glm::vec3 min_extent = scene.scene_info.min_extent;
	glm::vec3 max_extent = scene.scene_info.max_extent;

	if (m_scene_min_extent != min_extent ||
	    m_scene_max_extent != max_extent)
	{
		m_init = true;

		m_scene_min_extent = min_extent;
		m_scene_max_extent = max_extent;

		glm::vec3 scene_length = max_extent - min_extent;

		m_probe_update.params.probe_count  = glm::ivec3(scene_length / m_probe_update.params.probe_distance) + glm::ivec3(2);
		m_probe_update.params.grid_start   = min_extent;
		m_probe_update.params.max_distance = m_probe_update.params.probe_distance * 1.5f;

		create_resource();

		VkCommandBuffer          cmd_buffer = m_context->create_command_buffer();
		VkCommandBufferBeginInfo begin_info = {
		    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		vkBeginCommandBuffer(cmd_buffer, &begin_info);
		init(cmd_buffer);
		vkEndCommandBuffer(cmd_buffer);
		m_context->flush_command_buffer(cmd_buffer);
	}

	VkDescriptorBufferInfo global_buffer_info = {
	    .buffer = scene.global_buffer.vk_buffer,
	    .offset = 0,
	    .range  = sizeof(GlobalBuffer),
	};

	VkDescriptorBufferInfo ddgi_buffer_info = {
	    .buffer = uniform_buffer.vk_buffer,
	    .offset = 0,
	    .range  = sizeof(UBO),
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

	VkWriteDescriptorSetAccelerationStructureKHR as_write = {
	    .sType                      = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
	    .accelerationStructureCount = 1,
	    .pAccelerationStructures    = &scene.tlas.vk_as,
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

	VkDescriptorImageInfo skybox_info = {
	    .sampler     = scene.linear_sampler,
	    .imageView   = scene.envmap.texture_view,
	    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};

	VkDescriptorImageInfo radiance_infos[] = {
	    {
	        .sampler     = VK_NULL_HANDLE,
	        .imageView   = radiance_view,
	        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	    },
	    {
	        .sampler     = scene.linear_sampler,
	        .imageView   = radiance_view,
	        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    },
	};

	VkDescriptorImageInfo direction_depth_infos[] = {
	    {
	        .sampler     = VK_NULL_HANDLE,
	        .imageView   = direction_depth_view,
	        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	    },
	    {
	        .sampler     = scene.linear_sampler,
	        .imageView   = direction_depth_view,
	        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    },
	};

	VkDescriptorImageInfo probe_grid_irradiance_infos[2][2] = {
	    {
	        {
	            .sampler     = VK_NULL_HANDLE,
	            .imageView   = probe_grid_irradiance_view[0],
	            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	        },
	        {
	            .sampler     = VK_NULL_HANDLE,
	            .imageView   = probe_grid_irradiance_view[1],
	            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	        },
	    },
	    {
	        {
	            .sampler     = scene.linear_sampler,
	            .imageView   = probe_grid_irradiance_view[0],
	            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        },
	        {
	            .sampler     = scene.linear_sampler,
	            .imageView   = probe_grid_irradiance_view[1],
	            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        },
	    },
	};

	VkDescriptorImageInfo probe_grid_depth_infos[2][2] = {
	    {
	        {
	            .sampler     = VK_NULL_HANDLE,
	            .imageView   = probe_grid_depth_view[0],
	            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	        },
	        {
	            .sampler     = VK_NULL_HANDLE,
	            .imageView   = probe_grid_depth_view[1],
	            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	        },
	    },
	    {
	        {
	            .sampler     = scene.linear_sampler,
	            .imageView   = probe_grid_depth_view[0],
	            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        },
	        {
	            .sampler     = scene.linear_sampler,
	            .imageView   = probe_grid_depth_view[1],
	            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        },
	    },
	};

	// Update ray tracing pass
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
		        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		        .pImageInfo       = nullptr,
		        .pBufferInfo      = &vertex_buffer_info,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_raytraced.descriptor_sets[i],
		        .dstBinding       = 2,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		        .pImageInfo       = nullptr,
		        .pBufferInfo      = &index_buffer_info,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_raytraced.descriptor_sets[i],
		        .dstBinding       = 3,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		        .pImageInfo       = nullptr,
		        .pBufferInfo      = &material_buffer_info,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_raytraced.descriptor_sets[i],
		        .dstBinding       = 4,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		        .pImageInfo       = nullptr,
		        .pBufferInfo      = &emitter_buffer_info,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_raytraced.descriptor_sets[i],
		        .dstBinding       = 5,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		        .pImageInfo       = nullptr,
		        .pBufferInfo      = &instance_buffer_info,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_raytraced.descriptor_sets[i],
		        .dstBinding       = 6,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		        .pImageInfo       = nullptr,
		        .pBufferInfo      = &scene_buffer_info,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_raytraced.descriptor_sets[i],
		        .dstBinding       = 7,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		        .pImageInfo       = nullptr,
		        .pBufferInfo      = &ddgi_buffer_info,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .pNext            = &as_write,
		        .dstSet           = m_raytraced.descriptor_sets[i],
		        .dstBinding       = 8,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
		        .pImageInfo       = nullptr,
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_raytraced.descriptor_sets[i],
		        .dstBinding       = 9,
		        .dstArrayElement  = 0,
		        .descriptorCount  = static_cast<uint32_t>(texture_infos.size()),
		        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .pImageInfo       = texture_infos.data(),
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_raytraced.descriptor_sets[i],
		        .dstBinding       = 10,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .pImageInfo       = &skybox_info,
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_raytraced.descriptor_sets[i],
		        .dstBinding       = 11,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		        .pImageInfo       = &radiance_infos[0],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_raytraced.descriptor_sets[i],
		        .dstBinding       = 12,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		        .pImageInfo       = &direction_depth_infos[0],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_raytraced.descriptor_sets[i],
		        .dstBinding       = 13,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		        .pImageInfo       = &probe_grid_irradiance_infos[1][i],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_raytraced.descriptor_sets[i],
		        .dstBinding       = 14,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		        .pImageInfo       = &probe_grid_depth_infos[1][i],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		};
		vkUpdateDescriptorSets(m_context->vk_device, 13, writes, 0, nullptr);
	}
}

void RayTracedGI::draw(VkCommandBuffer cmd_buffer)
{
	m_context->begin_marker(cmd_buffer, "Ray Traced GI");
	{
		m_context->begin_marker(cmd_buffer, "Uniform Buffer Update");
		{
			UBO ubo = {
			    .grid_start                   = m_probe_update.params.grid_start,
			    .max_distance                 = m_probe_update.params.max_distance,
			    .grid_step                    = glm::vec3(m_probe_update.params.probe_distance),
			    .depth_sharpness              = m_probe_update.params.depth_sharpness,
			    .probe_count                  = m_probe_update.params.probe_count,
			    .hysteresis                   = m_probe_update.params.hysteresis,
			    .normal_bias                  = m_probe_update.params.normal_bias,
			    .energy_preservation          = m_probe_update.params.recursive_energy_preservation,
			    .rays_per_probe               = m_raytraced.params.rays_per_probe,
			    .visibility_test              = true,
			    .irradiance_probe_side_length = m_probe_update.params.irradiance_oct_size,
			    .irradiance_texture_width     = m_probe_update.params.irradiance_width,
			    .irradiance_texture_height    = m_probe_update.params.irradiance_height,
			    .depth_probe_side_length      = m_probe_update.params.depth_oct_size,
			    .depth_texture_width          = m_probe_update.params.depth_width,
			    .depth_texture_height         = m_probe_update.params.depth_height,
			};
			vkCmdUpdateBuffer(cmd_buffer, uniform_buffer.vk_buffer, 0, sizeof(UBO), &ubo);
		}
		m_context->end_marker(cmd_buffer);

		{
			VkBufferMemoryBarrier buffer_barriers[] = {
			    {
			        .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			        .pNext               = nullptr,
			        .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
			        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
			        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .buffer              = uniform_buffer.vk_buffer,
			        .offset              = 0,
			        .size                = sizeof(UBO),
			    },
			};
			vkCmdPipelineBarrier(
			    cmd_buffer,
			    VK_PIPELINE_STAGE_TRANSFER_BIT,
			    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			    0, 0, nullptr, 1, buffer_barriers, 0, nullptr);
		}

		m_context->begin_marker(cmd_buffer, "Ray Traced");
		{
			uint32_t total_probes                       = m_probe_update.params.probe_count.x * m_probe_update.params.probe_count.y * m_probe_update.params.probe_count.z;
			m_raytraced.push_constants.num_frames       = m_frame_count;
			m_raytraced.push_constants.infinite_bounces = m_raytraced.params.infinite_bounces && m_frame_count == 0 ? 1u : 0u;
			m_raytraced.push_constants.gi_intensity     = m_raytraced.params.infinite_bounce_intensity;

			vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_raytraced.pipeline_layout, 0, 1, &m_raytraced.descriptor_sets[m_context->ping_pong], 0, nullptr);
			vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_raytraced.pipeline);
			vkCmdPushConstants(cmd_buffer, m_raytraced.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(m_raytraced.push_constants), &m_raytraced.push_constants);
			vkCmdDispatch(cmd_buffer, static_cast<uint32_t>(ceil(float(m_raytraced.params.rays_per_probe) / float(NUM_THREADS_X))), static_cast<uint32_t>(ceil(float(total_probes) / float(NUM_THREADS_Y))), 1);
		}
		m_context->end_marker(cmd_buffer);

		m_context->begin_marker(cmd_buffer, "Probe Update");
		{
			m_probe_update.update_probe.push_constants.frame_count = m_frame_count;
			m_context->begin_marker(cmd_buffer, "Update Irradiance");
			{
				vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_probe_update.update_probe.pipeline_layout, 0, 1, &m_probe_update.update_probe.descriptor_sets[m_context->ping_pong], 0, nullptr);
				vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_probe_update.update_probe.irradiance_pipeline);
				vkCmdPushConstants(cmd_buffer, m_raytraced.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(m_probe_update.update_probe.push_constants), &m_probe_update.update_probe.push_constants);
				vkCmdDispatch(cmd_buffer, m_probe_update.params.probe_count.x * m_probe_update.params.probe_count.y, m_probe_update.params.probe_count.z, 1);
			}
			m_context->end_marker(cmd_buffer);

			m_context->begin_marker(cmd_buffer, "Update Depth");
			{
				vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_probe_update.update_probe.pipeline_layout, 0, 1, &m_probe_update.update_probe.descriptor_sets[m_context->ping_pong], 0, nullptr);
				vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_probe_update.update_probe.depth_pipeline);
				vkCmdPushConstants(cmd_buffer, m_raytraced.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(m_probe_update.update_probe.push_constants), &m_probe_update.update_probe.push_constants);
				vkCmdDispatch(cmd_buffer, m_probe_update.params.probe_count.x * m_probe_update.params.probe_count.y, m_probe_update.params.probe_count.z, 1);
			}
			m_context->end_marker(cmd_buffer);

			m_context->begin_marker(cmd_buffer, "Update Border");
			{
				m_context->begin_marker(cmd_buffer, "Update Irradiance");
				{
				}
				m_context->end_marker(cmd_buffer);

				m_context->begin_marker(cmd_buffer, "Update Depth");
				{
				}
				m_context->end_marker(cmd_buffer);
			}
			m_context->end_marker(cmd_buffer);
		}
		m_context->end_marker(cmd_buffer);

		m_context->begin_marker(cmd_buffer, "Sample Probe Grid");
		{
		}
		m_context->end_marker(cmd_buffer);
	}
	m_context->end_marker(cmd_buffer);

	{
		VkBufferMemoryBarrier buffer_barriers[] = {
		    {
		        .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		        .pNext               = nullptr,
		        .srcAccessMask       = VK_ACCESS_SHADER_READ_BIT,
		        .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .buffer              = uniform_buffer.vk_buffer,
		        .offset              = 0,
		        .size                = sizeof(UBO),
		    },
		};
		vkCmdPipelineBarrier(
		    cmd_buffer,
		    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		    VK_PIPELINE_STAGE_TRANSFER_BIT,
		    0, 0, nullptr, 1, buffer_barriers, 0, nullptr);
	}

	m_frame_count++;
}

bool RayTracedGI::draw_ui()
{
	return false;
}

void RayTracedGI::create_resource()
{
	vkDeviceWaitIdle(m_context->vk_device);

	m_frame_count = 0;

	destroy_resource();

	uint32_t total_probes = m_probe_update.params.probe_count.x * m_probe_update.params.probe_count.y * m_probe_update.params.probe_count.z;

	// Radiance image
	{
		VkImageCreateInfo image_create_info = {
		    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		    .imageType     = VK_IMAGE_TYPE_2D,
		    .format        = VK_FORMAT_R16G16B16A16_SFLOAT,
		    .extent        = VkExtent3D{m_raytraced.params.rays_per_probe, total_probes, 1},
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
		vmaCreateImage(m_context->vma_allocator, &image_create_info, &allocation_create_info, &radiance_image.vk_image, &radiance_image.vma_allocation, nullptr);
		VkImageViewCreateInfo view_create_info = {
		    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image            = radiance_image.vk_image,
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
		vkCreateImageView(m_context->vk_device, &view_create_info, nullptr, &radiance_view);
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) radiance_image.vk_image, "DDGI Radiance Image");
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) radiance_view, "DDGI Radiance View");
	}

	// Direction depth image
	{
		VkImageCreateInfo image_create_info = {
		    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		    .imageType     = VK_IMAGE_TYPE_2D,
		    .format        = VK_FORMAT_R16G16B16A16_SFLOAT,
		    .extent        = VkExtent3D{m_raytraced.params.rays_per_probe, total_probes, 1},
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
		vmaCreateImage(m_context->vma_allocator, &image_create_info, &allocation_create_info, &direction_depth_image.vk_image, &direction_depth_image.vma_allocation, nullptr);
		VkImageViewCreateInfo view_create_info = {
		    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image            = direction_depth_image.vk_image,
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
		vkCreateImageView(m_context->vk_device, &view_create_info, nullptr, &direction_depth_view);
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) direction_depth_image.vk_image, "DDGI Direction Depth Image");
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) direction_depth_view, "DDGI Direction Depth View");
	}

	// Probe grid irradiance image
	{
		m_probe_update.params.irradiance_width  = (m_probe_update.params.irradiance_oct_size + 2) * m_probe_update.params.probe_count.x * m_probe_update.params.probe_count.y + 2;
		m_probe_update.params.irradiance_height = (m_probe_update.params.irradiance_oct_size + 2) * m_probe_update.params.probe_count.z + 2;

		for (uint32_t i = 0; i < 2; i++)
		{
			VkImageCreateInfo image_create_info = {
			    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			    .imageType     = VK_IMAGE_TYPE_2D,
			    .format        = VK_FORMAT_R16G16B16A16_SFLOAT,
			    .extent        = VkExtent3D{m_probe_update.params.irradiance_width, m_probe_update.params.irradiance_height, 1},
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
			vmaCreateImage(m_context->vma_allocator, &image_create_info, &allocation_create_info, &probe_grid_irradiance_image[i].vk_image, &probe_grid_irradiance_image[i].vma_allocation, nullptr);
			VkImageViewCreateInfo view_create_info = {
			    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			    .image            = probe_grid_irradiance_image[i].vk_image,
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
			vkCreateImageView(m_context->vk_device, &view_create_info, nullptr, &probe_grid_irradiance_view[i]);
			m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) probe_grid_irradiance_image[i].vk_image, (std::string("DDGI Probe Grid Irradiance Image - ") + std::to_string(i)).c_str());
			m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) probe_grid_irradiance_view[i], (std::string("DDGI Probe Grid Irradiance View - ") + std::to_string(i)).c_str());
		}
	}

	// Probe grid depth image
	{
		m_probe_update.params.depth_width  = (m_probe_update.params.depth_oct_size + 2) * m_probe_update.params.probe_count.x * m_probe_update.params.probe_count.y + 2;
		m_probe_update.params.depth_height = (m_probe_update.params.depth_oct_size + 2) * m_probe_update.params.probe_count.z + 2;

		for (uint32_t i = 0; i < 2; i++)
		{
			VkImageCreateInfo image_create_info = {
			    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			    .imageType     = VK_IMAGE_TYPE_2D,
			    .format        = VK_FORMAT_R16G16B16A16_SFLOAT,
			    .extent        = VkExtent3D{m_probe_update.params.depth_width, m_probe_update.params.depth_height, 1},
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
			vmaCreateImage(m_context->vma_allocator, &image_create_info, &allocation_create_info, &probe_grid_depth_image[i].vk_image, &probe_grid_depth_image[i].vma_allocation, nullptr);
			VkImageViewCreateInfo view_create_info = {
			    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			    .image            = probe_grid_depth_image[i].vk_image,
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
			vkCreateImageView(m_context->vk_device, &view_create_info, nullptr, &probe_grid_depth_view[i]);
			m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) probe_grid_depth_image[i].vk_image, (std::string("DDGI Probe Grid Depth Image - ") + std::to_string(i)).c_str());
			m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) probe_grid_depth_view[i], (std::string("DDGI Probe Grid Depth View - ") + std::to_string(i)).c_str());
		}
	}

	// Sample probe grid
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
		vmaCreateImage(m_context->vma_allocator, &image_create_info, &allocation_create_info, &sample_probe_grid_image.vk_image, &sample_probe_grid_image.vma_allocation, nullptr);
		VkImageViewCreateInfo view_create_info = {
		    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image            = sample_probe_grid_image.vk_image,
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
		vkCreateImageView(m_context->vk_device, &view_create_info, nullptr, &sample_probe_grid_view);
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) sample_probe_grid_image.vk_image, "DDGI Sample Probe Grid Image");
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) sample_probe_grid_view, "DDGI Sample Probe Grid View");
	}

	// Uniform buffer
	{
		VkBufferCreateInfo buffer_create_info = {
		    .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		    .size        = sizeof(UBO),
		    .usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};
		VmaAllocationCreateInfo allocation_create_info = {
		    .usage = VMA_MEMORY_USAGE_CPU_TO_GPU};
		VmaAllocationInfo allocation_info = {};
		vmaCreateBuffer(m_context->vma_allocator, &buffer_create_info, &allocation_create_info, &uniform_buffer.vk_buffer, &uniform_buffer.vma_allocation, &allocation_info);
	}
}

void RayTracedGI::destroy_resource()
{
	if (radiance_image.vk_image &&
	    radiance_image.vma_allocation &&
	    radiance_view)
	{
		vkDestroyImageView(m_context->vk_device, radiance_view, nullptr);
		vmaDestroyImage(m_context->vma_allocator, radiance_image.vk_image, radiance_image.vma_allocation);
		radiance_view                 = VK_NULL_HANDLE;
		radiance_image.vk_image       = VK_NULL_HANDLE;
		radiance_image.vma_allocation = VK_NULL_HANDLE;
	}

	if (direction_depth_image.vk_image &&
	    direction_depth_image.vma_allocation &&
	    direction_depth_view)
	{
		vkDestroyImageView(m_context->vk_device, direction_depth_view, nullptr);
		vmaDestroyImage(m_context->vma_allocator, direction_depth_image.vk_image, direction_depth_image.vma_allocation);
		direction_depth_view                 = VK_NULL_HANDLE;
		direction_depth_image.vk_image       = VK_NULL_HANDLE;
		direction_depth_image.vma_allocation = VK_NULL_HANDLE;
	}

	for (uint32_t i = 0; i < 2; i++)
	{
		if (probe_grid_irradiance_image[i].vk_image &&
		    probe_grid_irradiance_image[i].vma_allocation &&
		    probe_grid_irradiance_view[i])
		{
			vkDestroyImageView(m_context->vk_device, probe_grid_irradiance_view[i], nullptr);
			vmaDestroyImage(m_context->vma_allocator, probe_grid_irradiance_image[i].vk_image, probe_grid_irradiance_image[i].vma_allocation);
			probe_grid_irradiance_view[i]                 = VK_NULL_HANDLE;
			probe_grid_irradiance_image[i].vk_image       = VK_NULL_HANDLE;
			probe_grid_irradiance_image[i].vma_allocation = VK_NULL_HANDLE;
		}

		if (probe_grid_depth_image[i].vk_image &&
		    probe_grid_depth_image[i].vma_allocation &&
		    probe_grid_depth_view[i])
		{
			vkDestroyImageView(m_context->vk_device, probe_grid_depth_view[i], nullptr);
			vmaDestroyImage(m_context->vma_allocator, probe_grid_depth_image[i].vk_image, probe_grid_depth_image[i].vma_allocation);
			probe_grid_depth_view[i]                 = VK_NULL_HANDLE;
			probe_grid_depth_image[i].vk_image       = VK_NULL_HANDLE;
			probe_grid_depth_image[i].vma_allocation = VK_NULL_HANDLE;
		}
	}

	if (sample_probe_grid_image.vk_image &&
	    sample_probe_grid_image.vma_allocation &&
	    sample_probe_grid_view)
	{
		vkDestroyImageView(m_context->vk_device, sample_probe_grid_view, nullptr);
		vmaDestroyImage(m_context->vma_allocator, sample_probe_grid_image.vk_image, sample_probe_grid_image.vma_allocation);
		sample_probe_grid_view                 = VK_NULL_HANDLE;
		sample_probe_grid_image.vk_image       = VK_NULL_HANDLE;
		sample_probe_grid_image.vma_allocation = VK_NULL_HANDLE;
	}

	if (uniform_buffer.vk_buffer && uniform_buffer.vma_allocation)
	{
		vmaDestroyBuffer(m_context->vma_allocator, uniform_buffer.vk_buffer, uniform_buffer.vma_allocation);
		uniform_buffer.vk_buffer      = VK_NULL_HANDLE;
		uniform_buffer.vma_allocation = VK_NULL_HANDLE;
		uniform_buffer.device_address = 0;
	}
}
