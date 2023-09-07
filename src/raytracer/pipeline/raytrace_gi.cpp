#include "pipeline/raytrace_gi.hpp"

#include <glm/gtc/quaternion.hpp>

#include <imgui.h>

static const uint32_t NUM_THREADS_X = 8;
static const uint32_t NUM_THREADS_Y = 8;

static unsigned char g_gi_raytraced_comp_spv_data[] = {
#include "gi_raytraced.comp.spv.h"
};

static unsigned char g_gi_probe_update_irradiance_comp_spv_data[] = {
#include "gi_probe_update_irradiance.comp.spv.h"
};

static unsigned char g_gi_probe_update_depth_comp_spv_data[] = {
#include "gi_probe_update_depth.comp.spv.h"
};

static unsigned char g_gi_border_update_irradiance_comp_spv_data[] = {
#include "gi_border_update_irradiance.comp.spv.h"
};

static unsigned char g_gi_border_update_depth_comp_spv_data[] = {
#include "gi_border_update_depth.comp.spv.h"
};

static unsigned char g_gi_probe_sample_comp_spv_data[] = {
#include "gi_probe_sample.comp.spv.h"
};

RayTracedGI::RayTracedGI(const Context &context, const Scene &scene, const GBufferPass &gbuffer_pass, RayTracedScale scale) :
    m_context(&context)
{
	float scale_divisor = std::powf(2.0f, float(scale));

	m_width  = m_context->render_extent.width / static_cast<uint32_t>(scale_divisor);
	m_height = m_context->render_extent.height / static_cast<uint32_t>(scale_divisor);

	m_gbuffer_mip = static_cast<uint32_t>(scale);

	std::random_device random_device;
	m_random_generator = std::mt19937(random_device());
	m_random_distrib   = std::uniform_real_distribution<float>(0.0f, 1.0f);

	m_raytraced.descriptor_set_layout = m_context->create_descriptor_layout()
	                                        // DDGI uniform buffer
	                                        .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
	                                        // Radiance
	                                        .add_descriptor_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                                        // Direction Depth
	                                        .add_descriptor_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                                        // Probe Irradiance
	                                        .add_descriptor_binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT)
	                                        // Probe Depth
	                                        .add_descriptor_binding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT)
	                                        // Probe Data
	                                        .add_descriptor_binding(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT)
	                                        .create();
	m_raytraced.descriptor_sets = m_context->allocate_descriptor_sets<2>(m_raytraced.descriptor_set_layout);
	m_raytraced.pipeline_layout = m_context->create_pipeline_layout({
	                                                                    scene.glsl_descriptor.layout,
	                                                                    gbuffer_pass.glsl_descriptor.layout,
	                                                                    m_raytraced.descriptor_set_layout,
	                                                                },
	                                                                sizeof(m_raytraced.push_constants), VK_SHADER_STAGE_COMPUTE_BIT);
	m_raytraced.pipeline        = m_context->create_compute_pipeline((uint32_t *) g_gi_raytraced_comp_spv_data, sizeof(g_gi_raytraced_comp_spv_data), m_raytraced.pipeline_layout);

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
			    // Probe data buffer
			    {
			        .binding         = 7,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			};
			VkDescriptorSetLayoutCreateInfo create_info = {
			    .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			    .bindingCount = 8,
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

	// Create probe border update pass
	{
		// Create shader module
		VkShaderModule update_irradiance_shader = VK_NULL_HANDLE;
		{
			VkShaderModuleCreateInfo create_info = {
			    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			    .codeSize = sizeof(g_gi_border_update_irradiance_comp_spv_data),
			    .pCode    = reinterpret_cast<uint32_t *>(g_gi_border_update_irradiance_comp_spv_data),
			};
			vkCreateShaderModule(m_context->vk_device, &create_info, nullptr, &update_irradiance_shader);
		}
		VkShaderModule update_depth_shader = VK_NULL_HANDLE;
		{
			VkShaderModuleCreateInfo create_info = {
			    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			    .codeSize = sizeof(g_gi_border_update_depth_comp_spv_data),
			    .pCode    = reinterpret_cast<uint32_t *>(g_gi_border_update_depth_comp_spv_data),
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
			};
			VkDescriptorSetLayoutCreateInfo create_info = {
			    .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			    .bindingCount = 2,
			    .pBindings    = bindings,
			};
			vkCreateDescriptorSetLayout(m_context->vk_device, &create_info, nullptr, &m_probe_update.update_border.descriptor_set_layout);
		}

		// Allocate descriptor set
		{
			VkDescriptorSetLayout layouts[] = {
			    m_probe_update.update_border.descriptor_set_layout,
			    m_probe_update.update_border.descriptor_set_layout,
			};
			VkDescriptorSetAllocateInfo allocate_info = {
			    .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			    .pNext              = nullptr,
			    .descriptorPool     = m_context->vk_descriptor_pool,
			    .descriptorSetCount = 2,
			    .pSetLayouts        = layouts,
			};
			vkAllocateDescriptorSets(m_context->vk_device, &allocate_info, m_probe_update.update_border.descriptor_sets);
		}

		// Create pipeline layout
		{
			VkPipelineLayoutCreateInfo create_info = {
			    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			    .setLayoutCount         = 1,
			    .pSetLayouts            = &m_probe_update.update_border.descriptor_set_layout,
			    .pushConstantRangeCount = 0,
			    .pPushConstantRanges    = nullptr,
			};
			vkCreatePipelineLayout(m_context->vk_device, &create_info, nullptr, &m_probe_update.update_border.pipeline_layout);
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
			    .layout             = m_probe_update.update_border.pipeline_layout,
			    .basePipelineHandle = VK_NULL_HANDLE,
			    .basePipelineIndex  = -1,
			};
			vkCreateComputePipelines(m_context->vk_device, m_context->vk_pipeline_cache, 1, &create_info, nullptr, &m_probe_update.update_border.irradiance_pipeline);
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
			    .layout             = m_probe_update.update_border.pipeline_layout,
			    .basePipelineHandle = VK_NULL_HANDLE,
			    .basePipelineIndex  = -1,
			};
			vkCreateComputePipelines(m_context->vk_device, m_context->vk_pipeline_cache, 1, &create_info, nullptr, &m_probe_update.update_border.depth_pipeline);
			vkDestroyShaderModule(m_context->vk_device, update_depth_shader, nullptr);
		}
	}

	// Create probe sample pass
	{
		// Create shader module
		VkShaderModule shader = VK_NULL_HANDLE;
		{
			VkShaderModuleCreateInfo create_info = {
			    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			    .codeSize = sizeof(g_gi_probe_sample_comp_spv_data),
			    .pCode    = reinterpret_cast<uint32_t *>(g_gi_probe_sample_comp_spv_data),
			};
			vkCreateShaderModule(m_context->vk_device, &create_info, nullptr, &shader);
		}

		// Create descriptor set layout
		{
			VkDescriptorSetLayoutBinding bindings[] = {
			    // DDGI buffer
			    {
			        .binding         = 0,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Probe irradiance
			    {
			        .binding         = 1,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Probe depth
			    {
			        .binding         = 2,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Output GI
			    {
			        .binding         = 3,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Probe data
			    {
			        .binding         = 4,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			};
			VkDescriptorSetLayoutCreateInfo create_info = {
			    .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			    .bindingCount = 5,
			    .pBindings    = bindings,
			};
			vkCreateDescriptorSetLayout(m_context->vk_device, &create_info, nullptr, &m_probe_sample.descriptor_set_layout);
		}

		// Allocate descriptor set
		{
			VkDescriptorSetLayout layouts[] = {
			    m_probe_sample.descriptor_set_layout,
			    m_probe_sample.descriptor_set_layout,
			};
			VkDescriptorSetAllocateInfo allocate_info = {
			    .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			    .pNext              = nullptr,
			    .descriptorPool     = m_context->vk_descriptor_pool,
			    .descriptorSetCount = 2,
			    .pSetLayouts        = layouts,
			};
			vkAllocateDescriptorSets(m_context->vk_device, &allocate_info, m_probe_sample.descriptor_sets);
		}

		// Create pipeline layout
		{
			VkDescriptorSetLayout layouts[] = {
			    scene.glsl_descriptor.layout,
			    gbuffer_pass.glsl_descriptor.layout,
			    m_probe_sample.descriptor_set_layout,
			};
			VkPushConstantRange range = {
			    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			    .offset     = 0,
			    .size       = sizeof(m_probe_sample.push_constants),
			};
			VkPipelineLayoutCreateInfo create_info = {
			    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			    .setLayoutCount         = 3,
			    .pSetLayouts            = layouts,
			    .pushConstantRangeCount = 1,
			    .pPushConstantRanges    = &range,
			};
			vkCreatePipelineLayout(m_context->vk_device, &create_info, nullptr, &m_probe_sample.pipeline_layout);
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
			    .layout             = m_probe_sample.pipeline_layout,
			    .basePipelineHandle = VK_NULL_HANDLE,
			    .basePipelineIndex  = -1,
			};
			vkCreateComputePipelines(m_context->vk_device, m_context->vk_pipeline_cache, 1, &create_info, nullptr, &m_probe_sample.pipeline);
			vkDestroyShaderModule(m_context->vk_device, shader, nullptr);
		}
	}

	// Create probe visualize pass
	//{
	//	// Create shader module
	//	VkShaderModule vert_shader = VK_NULL_HANDLE;
	//	{
	//		VkShaderModuleCreateInfo create_info = {
	//		    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	//		    .codeSize = sizeof(g_gi_probe_visualize_vert_spv_data),
	//		    .pCode    = reinterpret_cast<uint32_t *>(g_gi_probe_visualize_vert_spv_data),
	//		};
	//		vkCreateShaderModule(context.vk_device, &create_info, nullptr, &vert_shader);
	//	}

	//	VkShaderModule frag_shader = VK_NULL_HANDLE;
	//	{
	//		VkShaderModuleCreateInfo create_info = {
	//		    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	//		    .codeSize = sizeof(g_gi_probe_visualize_frag_spv_data),
	//		    .pCode    = reinterpret_cast<uint32_t *>(g_gi_probe_visualize_frag_spv_data),
	//		};
	//		vkCreateShaderModule(context.vk_device, &create_info, nullptr, &frag_shader);
	//	}

	//	// Create descriptor set layout
	//	{
	//		VkDescriptorSetLayoutBinding bindings[] = {
	//		    // DDGI shader
	//		    {
	//		        .binding         = 0,
	//		        .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
	//		        .descriptorCount = 1,
	//		        .stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
	//		    },
	//		    // Probe irradiance
	//		    {
	//		        .binding         = 1,
	//		        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	//		        .descriptorCount = 1,
	//		        .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
	//		    },
	//		    // Probe depth
	//		    {
	//		        .binding         = 2,
	//		        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	//		        .descriptorCount = 1,
	//		        .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
	//		    },
	//		};
	//		VkDescriptorSetLayoutCreateInfo create_info = {
	//		    .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
	//		    .bindingCount = 3,
	//		    .pBindings    = bindings,
	//		};
	//		vkCreateDescriptorSetLayout(m_context->vk_device, &create_info, nullptr, &m_probe_visualize.descriptor_set_layout);
	//	}

	//	// Allocate descriptor set
	//	{
	//		VkDescriptorSetLayout layouts[] = {
	//		    m_probe_visualize.descriptor_set_layout,
	//		    m_probe_visualize.descriptor_set_layout,
	//		};
	//		VkDescriptorSetAllocateInfo allocate_info = {
	//		    .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
	//		    .pNext              = nullptr,
	//		    .descriptorPool     = m_context->vk_descriptor_pool,
	//		    .descriptorSetCount = 2,
	//		    .pSetLayouts        = layouts,
	//		};
	//		vkAllocateDescriptorSets(m_context->vk_device, &allocate_info, m_probe_visualize.descriptor_sets);
	//	}

	//	// Create pipeline layout
	//	{
	//		VkDescriptorSetLayout layouts[] = {
	//		    scene.glsl_descriptor.layout,
	//		    gbuffer_pass.glsl_descriptor.layout,
	//		    m_probe_visualize.descriptor_set_layout,
	//		};
	//		VkPushConstantRange range = {
	//		    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
	//		    .offset     = 0,
	//		    .size       = sizeof(m_probe_visualize.push_constants),
	//		};
	//		VkPipelineLayoutCreateInfo create_info = {
	//		    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	//		    .setLayoutCount         = 3,
	//		    .pSetLayouts            = layouts,
	//		    .pushConstantRangeCount = 1,
	//		    .pPushConstantRanges    = &range,
	//		};
	//		vkCreatePipelineLayout(m_context->vk_device, &create_info, nullptr, &m_probe_visualize.pipeline_layout);
	//	}

	//	// Create pipeline
	//	{
	//		VkFormat color_attachment_formats[] =
	//		    {
	//		        VK_FORMAT_R16G16B16A16_SFLOAT,
	//		    };
	//		VkFormat depth_attachment_format = VK_FORMAT_D32_SFLOAT;

	//		VkPipelineRenderingCreateInfo pipeline_rendering_create_info = {
	//		    .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
	//		    .colorAttachmentCount    = 1,
	//		    .pColorAttachmentFormats = color_attachment_formats,
	//		    .depthAttachmentFormat   = depth_attachment_format,
	//		};

	//		VkPipelineInputAssemblyStateCreateInfo input_assembly_state_create_info = {
	//		    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
	//		    .flags                  = 0,
	//		    .topology               = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	//		    .primitiveRestartEnable = VK_FALSE,
	//		};

	//		VkPipelineRasterizationStateCreateInfo rasterization_state_create_info = {
	//		    .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
	//		    .depthClampEnable        = VK_FALSE,
	//		    .rasterizerDiscardEnable = VK_FALSE,
	//		    .polygonMode             = VK_POLYGON_MODE_FILL,
	//		    .cullMode                = VK_CULL_MODE_NONE,
	//		    .frontFace               = VK_FRONT_FACE_CLOCKWISE,
	//		    .depthBiasEnable         = VK_FALSE,
	//		    .depthBiasConstantFactor = 0.f,
	//		    .depthBiasClamp          = 0.f,
	//		    .depthBiasSlopeFactor    = 0.f,
	//		    .lineWidth               = 1.f,
	//		};

	//		VkPipelineColorBlendAttachmentState color_blend_attachment_states[] = {
	//		    {
	//		        .blendEnable    = VK_FALSE,
	//		        .colorWriteMask = 0xf,
	//		    },
	//		};

	//		VkPipelineColorBlendStateCreateInfo color_blend_state_create_info = {
	//		    .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
	//		    .logicOpEnable   = VK_FALSE,
	//		    .attachmentCount = 1,
	//		    .pAttachments    = color_blend_attachment_states,
	//		};

	//		VkPipelineDepthStencilStateCreateInfo depth_stencil_state_create_info = {
	//		    .sType             = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
	//		    .depthTestEnable   = VK_TRUE,
	//		    .depthWriteEnable  = VK_TRUE,
	//		    .depthCompareOp    = VK_COMPARE_OP_GREATER_OR_EQUAL,
	//		    .stencilTestEnable = VK_FALSE,
	//		    .front             = {
	//		                    .failOp    = VK_STENCIL_OP_KEEP,
	//		                    .passOp    = VK_STENCIL_OP_KEEP,
	//		                    .compareOp = VK_COMPARE_OP_ALWAYS,
	//               },
	//		    .back = {
	//		        .failOp    = VK_STENCIL_OP_KEEP,
	//		        .passOp    = VK_STENCIL_OP_KEEP,
	//		        .compareOp = VK_COMPARE_OP_ALWAYS,
	//		    },
	//		};

	//		VkViewport viewport = {
	//		    .x        = 0,
	//		    .y        = 0,
	//		    .width    = static_cast<float>(m_context->extent.width),
	//		    .height   = static_cast<float>(m_context->extent.height),
	//		    .minDepth = 0.f,
	//		    .maxDepth = 1.f,
	//		};

	//		VkRect2D rect = {
	//		    .offset = VkOffset2D{
	//		        .x = 0,
	//		        .y = 0,
	//		    },
	//		    .extent = VkExtent2D{
	//		        .width  = m_context->extent.width,
	//		        .height = m_context->extent.height,
	//		    },
	//		};

	//		VkPipelineViewportStateCreateInfo viewport_state_create_info = {
	//		    .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
	//		    .viewportCount = 1,
	//		    .pViewports    = &viewport,
	//		    .scissorCount  = 1,
	//		    .pScissors     = &rect,
	//		};

	//		VkPipelineMultisampleStateCreateInfo multisample_state_create_info = {
	//		    .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
	//		    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	//		    .sampleShadingEnable  = VK_FALSE,
	//		};

	//		VkVertexInputAttributeDescription attribute_descriptions[] = {
	//		    {
	//		        .location = 0,
	//		        .binding  = 0,
	//		        .format   = VK_FORMAT_R32G32B32A32_SFLOAT,
	//		        .offset   = offsetof(Vertex, position),
	//		    },
	//		    {
	//		        .location = 1,
	//		        .binding  = 0,
	//		        .format   = VK_FORMAT_R32G32B32A32_SFLOAT,
	//		        .offset   = offsetof(Vertex, normal),
	//		    },
	//		};

	//		VkVertexInputBindingDescription binding_description = {
	//		    .binding   = 0,
	//		    .stride    = sizeof(Vertex),
	//		    .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
	//		};

	//		VkPipelineVertexInputStateCreateInfo vertex_input_state_create_info = {
	//		    .sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
	//		    .vertexBindingDescriptionCount   = 1,
	//		    .pVertexBindingDescriptions      = &binding_description,
	//		    .vertexAttributeDescriptionCount = 2,
	//		    .pVertexAttributeDescriptions    = attribute_descriptions,
	//		};

	//		VkPipelineShaderStageCreateInfo pipeline_shader_stage_create_infos[] = {
	//		    {
	//		        .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	//		        .stage               = VK_SHADER_STAGE_VERTEX_BIT,
	//		        .module              = vert_shader,
	//		        .pName               = "main",
	//		        .pSpecializationInfo = nullptr,
	//		    },
	//		    {
	//		        .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	//		        .stage               = VK_SHADER_STAGE_FRAGMENT_BIT,
	//		        .module              = frag_shader,
	//		        .pName               = "main",
	//		        .pSpecializationInfo = nullptr,
	//		    },
	//		};

	//		VkGraphicsPipelineCreateInfo create_info = {
	//		    .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
	//		    .pNext               = &pipeline_rendering_create_info,
	//		    .stageCount          = 2,
	//		    .pStages             = pipeline_shader_stage_create_infos,
	//		    .pVertexInputState   = &vertex_input_state_create_info,
	//		    .pInputAssemblyState = &input_assembly_state_create_info,
	//		    .pTessellationState  = nullptr,
	//		    .pViewportState      = &viewport_state_create_info,
	//		    .pRasterizationState = &rasterization_state_create_info,
	//		    .pMultisampleState   = &multisample_state_create_info,
	//		    .pDepthStencilState  = &depth_stencil_state_create_info,
	//		    .pColorBlendState    = &color_blend_state_create_info,
	//		    .pDynamicState       = nullptr,
	//		    .layout              = m_probe_visualize.pipeline_layout,
	//		    .renderPass          = VK_NULL_HANDLE,
	//		    .subpass             = 0,
	//		    .basePipelineHandle  = VK_NULL_HANDLE,
	//		    .basePipelineIndex   = -1,
	//		};
	//		vkCreateGraphicsPipelines(m_context->vk_device, m_context->vk_pipeline_cache, 1, &create_info, nullptr, &m_probe_visualize.pipeline);
	//		vkDestroyShaderModule(m_context->vk_device, vert_shader, nullptr);
	//		vkDestroyShaderModule(m_context->vk_device, frag_shader, nullptr);
	//	}
	//}

	// Create external descriptor
	{
		// Create descriptor set layout
		{
			VkDescriptorSetLayoutBinding bindings[] = {
			    // DDGI buffer
			    {
			        .binding         = 0,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Probe irradiance
			    {
			        .binding         = 1,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Probe depth
			    {
			        .binding         = 2,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			    // Probe data
			    {
			        .binding         = 3,
			        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .descriptorCount = 1,
			        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
			    },
			};
			VkDescriptorSetLayoutCreateInfo create_info = {
			    .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			    .bindingCount = 4,
			    .pBindings    = bindings,
			};
			vkCreateDescriptorSetLayout(m_context->vk_device, &create_info, nullptr, &descriptor.layout);
		}

		// Allocate descriptor set
		{
			VkDescriptorSetLayout layouts[] = {
			    descriptor.layout,
			    descriptor.layout,
			};
			VkDescriptorSetAllocateInfo allocate_info = {
			    .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			    .pNext              = nullptr,
			    .descriptorPool     = m_context->vk_descriptor_pool,
			    .descriptorSetCount = 2,
			    .pSetLayouts        = layouts,
			};
			vkAllocateDescriptorSets(m_context->vk_device, &allocate_info, descriptor.sets);
		}
	}

	bind_descriptor.layout = m_context->create_descriptor_layout()
	                             .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                             .create();
	bind_descriptor.set = m_context->allocate_descriptor_set(bind_descriptor.layout);
}

RayTracedGI::~RayTracedGI()
{
	destroy_resource();

	m_context->destroy(m_raytraced.pipeline)
	    .destroy(m_raytraced.pipeline_layout)
	    .destroy(m_raytraced.descriptor_set_layout)
	    .destroy(m_raytraced.descriptor_sets);

	vkDestroyPipelineLayout(m_context->vk_device, m_probe_update.update_probe.pipeline_layout, nullptr);
	vkDestroyPipelineLayout(m_context->vk_device, m_probe_update.update_border.pipeline_layout, nullptr);
	vkDestroyPipelineLayout(m_context->vk_device, m_probe_sample.pipeline_layout, nullptr);
	// vkDestroyPipelineLayout(m_context->vk_device, m_probe_visualize.pipeline_layout, nullptr);

	vkDestroyPipeline(m_context->vk_device, m_probe_update.update_probe.irradiance_pipeline, nullptr);
	vkDestroyPipeline(m_context->vk_device, m_probe_update.update_probe.depth_pipeline, nullptr);
	vkDestroyPipeline(m_context->vk_device, m_probe_update.update_border.irradiance_pipeline, nullptr);
	vkDestroyPipeline(m_context->vk_device, m_probe_update.update_border.depth_pipeline, nullptr);
	vkDestroyPipeline(m_context->vk_device, m_probe_sample.pipeline, nullptr);
	// vkDestroyPipeline(m_context->vk_device, m_probe_visualize.pipeline, nullptr);

	vkDestroyDescriptorSetLayout(m_context->vk_device, m_probe_update.update_probe.descriptor_set_layout, nullptr);
	vkDestroyDescriptorSetLayout(m_context->vk_device, m_probe_update.update_border.descriptor_set_layout, nullptr);
	vkDestroyDescriptorSetLayout(m_context->vk_device, m_probe_sample.descriptor_set_layout, nullptr);
	// vkDestroyDescriptorSetLayout(m_context->vk_device, m_probe_visualize.descriptor_set_layout, nullptr);
	vkDestroyDescriptorSetLayout(m_context->vk_device, descriptor.layout, nullptr);
	vkDestroyDescriptorSetLayout(m_context->vk_device, bind_descriptor.layout, nullptr);

	vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 2, m_probe_update.update_probe.descriptor_sets);
	vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 2, m_probe_update.update_border.descriptor_sets);
	vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 2, m_probe_sample.descriptor_sets);
	// vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 2, m_probe_visualize.descriptor_sets);
	vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 2, descriptor.sets);
	vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 1, &bind_descriptor.set);

	// vmaDestroyBuffer(m_context->vma_allocator, m_probe_visualize.vertex_buffer.vk_buffer, m_probe_visualize.vertex_buffer.vma_allocation);
	// vmaDestroyBuffer(m_context->vma_allocator, m_probe_visualize.index_buffer.vk_buffer, m_probe_visualize.index_buffer.vma_allocation);
}

void RayTracedGI::init()
{
	m_context->record_command()
	    .begin()
	    .insert_barrier()
	    .add_buffer_barrier(
	        uniform_buffer.vk_buffer,
	        0, VK_ACCESS_SHADER_READ_BIT)
	    .add_image_barrier(
	        radiance_image.vk_image,
	        0, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        direction_depth_image.vk_image,
	        0, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        probe_grid_irradiance_image[0].vk_image,
	        0, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .add_image_barrier(
	        probe_grid_depth_image[0].vk_image,
	        0, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .add_image_barrier(
	        probe_grid_irradiance_image[1].vk_image,
	        0, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        probe_grid_depth_image[1].vk_image,
	        0, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        sample_probe_grid_image.vk_image,
	        0, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .insert()
	    .end()
	    .flush();
}

void RayTracedGI::update(const Scene &scene)
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

		m_context->update_descriptor()
		    .write_sampled_images(0, {sample_probe_grid_view})
		    .update(bind_descriptor.set);
	}

	VkDescriptorBufferInfo ddgi_buffer_info = {
	    .buffer = uniform_buffer.vk_buffer,
	    .offset = 0,
	    .range  = VK_WHOLE_SIZE,
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

	VkDescriptorImageInfo probe_sample_info = {
	    .sampler     = VK_NULL_HANDLE,
	    .imageView   = sample_probe_grid_view,
	    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
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
		        .pBufferInfo      = &ddgi_buffer_info,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_raytraced.descriptor_sets[i],
		        .dstBinding       = 1,
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
		        .dstBinding       = 2,
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
		        .dstBinding       = 3,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .pImageInfo       = &probe_grid_irradiance_infos[1][i],
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
		        .pImageInfo       = &probe_grid_depth_infos[1][i],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		};
		vkUpdateDescriptorSets(m_context->vk_device, 5, writes, 0, nullptr);
	}

	// Update probe update pass
	for (uint32_t i = 0; i < 2; i++)
	{
		VkWriteDescriptorSet writes[] = {
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_probe_update.update_probe.descriptor_sets[i],
		        .dstBinding       = 0,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		        .pImageInfo       = &probe_grid_irradiance_infos[0][!i],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_probe_update.update_probe.descriptor_sets[i],
		        .dstBinding       = 1,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		        .pImageInfo       = &probe_grid_depth_infos[0][!i],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_probe_update.update_probe.descriptor_sets[i],
		        .dstBinding       = 2,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .pImageInfo       = &probe_grid_irradiance_infos[1][i],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_probe_update.update_probe.descriptor_sets[i],
		        .dstBinding       = 3,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .pImageInfo       = &probe_grid_depth_infos[1][i],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_probe_update.update_probe.descriptor_sets[i],
		        .dstBinding       = 4,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .pImageInfo       = &radiance_infos[1],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_probe_update.update_probe.descriptor_sets[i],
		        .dstBinding       = 5,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .pImageInfo       = &direction_depth_infos[1],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_probe_update.update_probe.descriptor_sets[i],
		        .dstBinding       = 6,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		        .pImageInfo       = nullptr,
		        .pBufferInfo      = &ddgi_buffer_info,
		        .pTexelBufferView = nullptr,
		    },
		};
		vkUpdateDescriptorSets(m_context->vk_device, 7, writes, 0, nullptr);
	}

	// Update border update pass
	for (uint32_t i = 0; i < 2; i++)
	{
		VkWriteDescriptorSet writes[] = {
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_probe_update.update_border.descriptor_sets[i],
		        .dstBinding       = 0,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		        .pImageInfo       = &probe_grid_irradiance_infos[0][!i],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_probe_update.update_border.descriptor_sets[i],
		        .dstBinding       = 1,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		        .pImageInfo       = &probe_grid_depth_infos[0][!i],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		};
		vkUpdateDescriptorSets(m_context->vk_device, 2, writes, 0, nullptr);
	}

	// Update probe sample pass
	for (uint32_t i = 0; i < 2; i++)
	{
		VkWriteDescriptorSet writes[] = {
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_probe_sample.descriptor_sets[i],
		        .dstBinding       = 0,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		        .pImageInfo       = nullptr,
		        .pBufferInfo      = &ddgi_buffer_info,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_probe_sample.descriptor_sets[i],
		        .dstBinding       = 1,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .pImageInfo       = &probe_grid_irradiance_infos[1][!i],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_probe_sample.descriptor_sets[i],
		        .dstBinding       = 2,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .pImageInfo       = &probe_grid_depth_infos[1][!i],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_probe_sample.descriptor_sets[i],
		        .dstBinding       = 3,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		        .pImageInfo       = &probe_sample_info,
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		};
		vkUpdateDescriptorSets(m_context->vk_device, 4, writes, 0, nullptr);
	}

	// Update probe visualize pass
	/*for (uint32_t i = 0; i < 2; i++)
	{
	    VkWriteDescriptorSet writes[] = {

	        {
	            .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	            .dstSet           = m_probe_visualize.descriptor_sets[i],
	            .dstBinding       = 0,
	            .dstArrayElement  = 0,
	            .descriptorCount  = 1,
	            .descriptorType   = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
	            .pImageInfo       = nullptr,
	            .pBufferInfo      = &ddgi_buffer_info,
	            .pTexelBufferView = nullptr,
	        },
	        {
	            .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	            .dstSet           = m_probe_visualize.descriptor_sets[i],
	            .dstBinding       = 1,
	            .dstArrayElement  = 0,
	            .descriptorCount  = 1,
	            .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	            .pImageInfo       = &probe_grid_irradiance_infos[1][!i],
	            .pBufferInfo      = nullptr,
	            .pTexelBufferView = nullptr,
	        },
	        {
	            .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	            .dstSet           = m_probe_visualize.descriptor_sets[i],
	            .dstBinding       = 2,
	            .dstArrayElement  = 0,
	            .descriptorCount  = 1,
	            .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	            .pImageInfo       = &probe_grid_depth_infos[1][!i],
	            .pBufferInfo      = nullptr,
	            .pTexelBufferView = nullptr,
	        },
	    };
	    vkUpdateDescriptorSets(m_context->vk_device, 3, writes, 0, nullptr);
	}*/

	for (uint32_t i = 0; i < 2; i++)
	{
		VkWriteDescriptorSet writes[] = {
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = descriptor.sets[i],
		        .dstBinding       = 0,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		        .pImageInfo       = nullptr,
		        .pBufferInfo      = &ddgi_buffer_info,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = descriptor.sets[i],
		        .dstBinding       = 1,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .pImageInfo       = &probe_grid_irradiance_infos[1][!i],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = descriptor.sets[i],
		        .dstBinding       = 2,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .pImageInfo       = &probe_grid_depth_infos[1][!i],
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		};
		vkUpdateDescriptorSets(m_context->vk_device, 3, writes, 0, nullptr);
	}
}

void RayTracedGI::draw(VkCommandBuffer cmd_buffer, const Scene &scene, const GBufferPass &gbuffer_pass)
{
	{
	    {UBO ubo = {
	         .grid_start                   = m_probe_update.params.grid_start + m_probe_update.params.grid_offset,
	         .max_distance                 = m_probe_update.params.max_distance,
	         .grid_step                    = glm::vec3(m_probe_update.params.probe_distance),
	         .depth_sharpness              = m_probe_update.params.depth_sharpness,
	         .probe_count                  = m_probe_update.params.probe_count,
	         .hysteresis                   = m_probe_update.params.hysteresis,
	         .normal_bias                  = m_probe_update.params.normal_bias,
	         .energy_preservation          = m_probe_update.params.recursive_energy_preservation,
	         .rays_per_probe               = static_cast<uint32_t>(m_raytraced.params.rays_per_probe),
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

{
	uint32_t total_probes = m_probe_update.params.probe_count.x * m_probe_update.params.probe_count.y * m_probe_update.params.probe_count.z;

	VkDescriptorSet descriptors[] = {
	    scene.glsl_descriptor.set,
	    gbuffer_pass.glsl_descriptor.sets[m_context->ping_pong],
	    m_raytraced.descriptor_sets[m_context->ping_pong],
	};

	m_raytraced.push_constants.random_orientation = glm::mat4_cast(glm::angleAxis(m_random_distrib(m_random_generator) * (glm::pi<float>() * 2.0f), glm::normalize(glm::vec3(m_random_distrib(m_random_generator), m_random_distrib(m_random_generator), m_random_distrib(m_random_generator)))));
	m_raytraced.push_constants.num_frames         = m_frame_count;
	m_raytraced.push_constants.infinite_bounces   = m_raytraced.params.infinite_bounces && m_frame_count == 0 ? 0u : 1u;
	m_raytraced.push_constants.gi_intensity       = m_raytraced.params.infinite_bounce_intensity;

	vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_raytraced.pipeline_layout, 0, 3, descriptors, 0, nullptr);
	vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_raytraced.pipeline);
	vkCmdPushConstants(cmd_buffer, m_raytraced.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(m_raytraced.push_constants), &m_raytraced.push_constants);
	vkCmdDispatch(cmd_buffer, static_cast<uint32_t>(ceil(float(m_raytraced.params.rays_per_probe) / float(NUM_THREADS_X))), static_cast<uint32_t>(ceil(float(total_probes) / float(NUM_THREADS_Y))), 1);
}

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
	        .srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
	        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
	        .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
	        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
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
	        .srcAccessMask       = VK_ACCESS_SHADER_READ_BIT,
	        .dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
	        .oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image               = sample_probe_grid_image.vk_image,
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
	    0, 0, nullptr, 0, nullptr, 3, image_barriers);
}

{
	m_probe_update.update_probe.push_constants.frame_count = m_frame_count;
	{
		VkDescriptorSet descriptors[] = {
		    m_probe_update.update_probe.descriptor_sets[m_context->ping_pong],
		};
		vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_probe_update.update_probe.pipeline_layout, 0, 1, descriptors, 0, nullptr);
		vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_probe_update.update_probe.irradiance_pipeline);
		vkCmdPushConstants(cmd_buffer, m_raytraced.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(m_probe_update.update_probe.push_constants), &m_probe_update.update_probe.push_constants);
		vkCmdDispatch(cmd_buffer, m_probe_update.params.probe_count.x * m_probe_update.params.probe_count.y, m_probe_update.params.probe_count.z, 1);
	}

	{
		VkDescriptorSet descriptors[] = {
		    m_probe_update.update_probe.descriptor_sets[m_context->ping_pong],
		};
		vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_probe_update.update_probe.pipeline_layout, 0, 1, descriptors, 0, nullptr);
		vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_probe_update.update_probe.depth_pipeline);
		vkCmdPushConstants(cmd_buffer, m_raytraced.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(m_probe_update.update_probe.push_constants), &m_probe_update.update_probe.push_constants);
		vkCmdDispatch(cmd_buffer, m_probe_update.params.probe_count.x * m_probe_update.params.probe_count.y, m_probe_update.params.probe_count.z, 1);
	}

	{
		VkImageMemoryBarrier image_barriers[] = {
		    {
		        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
		        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
		        .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
		        .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image               = probe_grid_irradiance_image[!m_context->ping_pong].vk_image,
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
		        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
		        .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
		        .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image               = probe_grid_depth_image[!m_context->ping_pong].vk_image,
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
		{
			vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_probe_update.update_border.pipeline_layout, 0, 1, &m_probe_update.update_border.descriptor_sets[m_context->ping_pong], 0, nullptr);
			vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_probe_update.update_border.irradiance_pipeline);
			vkCmdDispatch(cmd_buffer, m_probe_update.params.probe_count.x * m_probe_update.params.probe_count.y, m_probe_update.params.probe_count.z, 1);
		}

		{
			vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_probe_update.update_border.pipeline_layout, 0, 1, &m_probe_update.update_border.descriptor_sets[m_context->ping_pong], 0, nullptr);
			vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_probe_update.update_border.depth_pipeline);
			vkCmdDispatch(cmd_buffer, m_probe_update.params.probe_count.x * m_probe_update.params.probe_count.y, m_probe_update.params.probe_count.z, 1);
		}
	}
}

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
	        .image               = probe_grid_irradiance_image[!m_context->ping_pong].vk_image,
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
	        .image               = probe_grid_depth_image[!m_context->ping_pong].vk_image,
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
	    VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	    0, 0, nullptr, 0, nullptr, 2, image_barriers);
}

{
	VkDescriptorSet descriptors[] = {
	    scene.glsl_descriptor.set,
	    gbuffer_pass.glsl_descriptor.sets[m_context->ping_pong],
	    m_probe_sample.descriptor_sets[m_context->ping_pong],
	};

	m_probe_sample.push_constants.gbuffer_mip  = m_gbuffer_mip;
	m_probe_sample.push_constants.gi_intensity = m_probe_sample.params.gi_intensity;

	vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_probe_sample.pipeline_layout, 0, 3, descriptors, 0, nullptr);
	vkCmdPushConstants(cmd_buffer, m_probe_sample.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(m_probe_sample.push_constants), &m_probe_sample.push_constants);
	vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_probe_sample.pipeline);
	vkCmdDispatch(cmd_buffer, static_cast<uint32_t>(ceil(float(m_width) / float(NUM_THREADS_X))), static_cast<uint32_t>(ceil(float(m_height) / float(NUM_THREADS_Y))), 1);
}
}

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
	VkImageMemoryBarrier image_barriers[] = {
	    {
	        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .srcAccessMask       = VK_ACCESS_SHADER_READ_BIT,
	        .dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
	        .oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image               = probe_grid_irradiance_image[m_context->ping_pong].vk_image,
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
	        .image               = probe_grid_depth_image[m_context->ping_pong].vk_image,
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
	        .srcAccessMask       = VK_ACCESS_SHADER_READ_BIT,
	        .dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
	        .oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
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
	        .srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
	        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
	        .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
	        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image               = sample_probe_grid_image.vk_image,
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
	    VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	    0, 0, nullptr, 1, buffer_barriers, 5, image_barriers);
}

m_frame_count++;
}

bool RayTracedGI::draw_ui()
{
	if (ImGui::TreeNode("Ray Trace GI"))
	{
		ImGui::Text("Probe Grid Size: [%i, %i, %i]",
		            m_probe_update.params.probe_count.x,
		            m_probe_update.params.probe_count.y,
		            m_probe_update.params.probe_count.z);
		ImGui::Checkbox("Visibility Test", &m_probe_update.params.visibility_test);
		ImGui::Checkbox("Infinite Bounce", reinterpret_cast<bool *>(&m_raytraced.params.infinite_bounces));
		ImGui::SliderFloat("Normal Bias", &m_probe_update.params.normal_bias, 0.0f, 1.0f, "%.3f");
		ImGui::DragFloat3("Grid Offset", &m_probe_update.params.grid_offset.x, 0.01f, -10.f, 10.f);
		ImGui::SliderFloat("Infinite Bounce Intensity", &m_raytraced.params.infinite_bounce_intensity, 0.0f, 10.0f);
		ImGui::SliderFloat("GI Intensity", &m_probe_sample.params.gi_intensity, 0.0f, 10.0f);
		ImGui::TreePop();
	}

	return false;
}

void RayTracedGI::create_resource()
{
	vkDeviceWaitIdle(m_context->vk_device);

	m_frame_count = 0;

	destroy_resource();

	uint32_t total_probes = m_probe_update.params.probe_count.x * m_probe_update.params.probe_count.y * m_probe_update.params.probe_count.z;

	radiance_image = m_context->create_texture_2d(
	    "GI Radiance Image",
	    (uint32_t) m_raytraced.params.rays_per_probe, total_probes,
	    VK_FORMAT_R16G16B16A16_SFLOAT,
	    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	radiance_view = m_context->create_texture_view(
	    "GI Radiance View",
	    radiance_image.vk_image,
	    VK_FORMAT_R16G16B16A16_SFLOAT);

	direction_depth_image = m_context->create_texture_2d(
	    "GI Direction Depth Image",
	    (uint32_t) m_raytraced.params.rays_per_probe, total_probes,
	    VK_FORMAT_R16G16B16A16_SFLOAT,
	    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	direction_depth_view = m_context->create_texture_view(
	    "GI Direction Depth View",
	    direction_depth_image.vk_image,
	    VK_FORMAT_R16G16B16A16_SFLOAT);

	{
		m_probe_update.params.irradiance_width  = (m_probe_update.params.irradiance_oct_size + 2) * m_probe_update.params.probe_count.x * m_probe_update.params.probe_count.y + 2;
		m_probe_update.params.irradiance_height = (m_probe_update.params.irradiance_oct_size + 2) * m_probe_update.params.probe_count.z + 2;

		for (uint32_t i = 0; i < 2; i++)
		{
			probe_grid_irradiance_image[i] = m_context->create_texture_2d(
			    "GI Probe Grid Irradiance Image",
			    (uint32_t) m_raytraced.params.rays_per_probe, total_probes,
			    VK_FORMAT_R16G16B16A16_SFLOAT,
			    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
			probe_grid_irradiance_view[i] = m_context->create_texture_view(
			    "GI Probe Grid Irradiance View",
			    probe_grid_irradiance_image[i].vk_image,
			    VK_FORMAT_R16G16B16A16_SFLOAT);
		}
	}

	// Probe grid depth image
	{
		m_probe_update.params.depth_width  = (m_probe_update.params.depth_oct_size + 2) * m_probe_update.params.probe_count.x * m_probe_update.params.probe_count.y + 2;
		m_probe_update.params.depth_height = (m_probe_update.params.depth_oct_size + 2) * m_probe_update.params.probe_count.z + 2;

		for (uint32_t i = 0; i < 2; i++)
		{
			probe_grid_depth_image[i] = m_context->create_texture_2d(
			    "GI Probe Grid Depth Image",
			    m_probe_update.params.depth_width, m_probe_update.params.depth_height,
			    VK_FORMAT_R16G16B16A16_SFLOAT,
			    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
			probe_grid_depth_view[i] = m_context->create_texture_view(
			    "GI Probe Grid Depth View",
			    probe_grid_depth_image[i].vk_image,
			    VK_FORMAT_R16G16B16A16_SFLOAT);
		}
	}

	// Sample probe grid
	{
		sample_probe_grid_image = m_context->create_texture_2d(
		    "GI Sample Probe Grid Image",
		    m_width, m_height,
		    VK_FORMAT_R16G16B16A16_SFLOAT,
		    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
		sample_probe_grid_view = m_context->create_texture_view(
		    "GI Sample Probe Grid View",
		    sample_probe_grid_image.vk_image,
		    VK_FORMAT_R16G16B16A16_SFLOAT);
	}

	// Uniform buffer
	uniform_buffer = m_context->create_buffer("GI Uniform Buffer", sizeof(UBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

	init();
}

void RayTracedGI::destroy_resource()
{
	m_context->destroy(radiance_image)
	    .destroy(radiance_view)
	    .destroy(direction_depth_image)
	    .destroy(direction_depth_view)
	    .destroy(probe_grid_irradiance_image[0])
	    .destroy(probe_grid_irradiance_image[1])
	    .destroy(probe_grid_irradiance_view[0])
	    .destroy(probe_grid_irradiance_view[1])
	    .destroy(probe_grid_depth_image[0])
	    .destroy(probe_grid_depth_image[1])
	    .destroy(probe_grid_depth_view[0])
	    .destroy(probe_grid_depth_view[1])
	    .destroy(sample_probe_grid_image)
	    .destroy(sample_probe_grid_view)
	    .destroy(uniform_buffer);
}
