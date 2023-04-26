#include "render/pipeline/gbuffer.hpp"

#include <spdlog/fmt/fmt.h>

static unsigned char g_gbuffer_vert_spv_data[] = {
#include "gbuffer.vert.spv.h"
};

static unsigned char g_gbuffer_frag_spv_data[] = {
#include "gbuffer.frag.spv.h"
};

GBufferPass::GBufferPass(const Context &context) :
    m_context(&context)
{
	width     = context.extent.width;
	height    = context.extent.height;
	mip_level = static_cast<uint32_t>(std::floor(std::log2(std::max(width, height))) + 1);

	// Create shader module
	VkShaderModule vert_shader = VK_NULL_HANDLE;
	{
		VkShaderModuleCreateInfo create_info = {
		    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		    .codeSize = sizeof(g_gbuffer_vert_spv_data),
		    .pCode    = reinterpret_cast<uint32_t *>(g_gbuffer_vert_spv_data),
		};
		vkCreateShaderModule(context.vk_device, &create_info, nullptr, &vert_shader);
	}

	VkShaderModule frag_shader = VK_NULL_HANDLE;
	{
		VkShaderModuleCreateInfo create_info = {
		    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		    .codeSize = sizeof(g_gbuffer_frag_spv_data),
		    .pCode    = reinterpret_cast<uint32_t *>(g_gbuffer_frag_spv_data),
		};
		vkCreateShaderModule(context.vk_device, &create_info, nullptr, &frag_shader);
	}

	// Create descriptor set layout
	{
		VkDescriptorBindingFlags descriptor_binding_flags[] = {
		    0,
		    VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
		    0,
		    0,
		};

		VkDescriptorSetLayoutBinding bindings[] = {
		    // Global buffer
		    {
		        .binding         = 0,
		        .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		        .descriptorCount = 1,
		        .stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		    },
		    // Bindless textures
		    {
		        .binding         = 1,
		        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .descriptorCount = 1024,
		        .stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		    },
		    // Instance buffer
		    {
		        .binding         = 2,
		        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		        .descriptorCount = 1,
		        .stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		    },
		    // Material buffer
		    {
		        .binding         = 3,
		        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		        .descriptorCount = 1,
		        .stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		    },
		};

		VkDescriptorSetLayoutBindingFlagsCreateInfo descriptor_set_layout_binding_flag_create_info = {
		    .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
		    .bindingCount  = 4,
		    .pBindingFlags = descriptor_binding_flags,
		};

		VkDescriptorSetLayoutCreateInfo create_info = {
		    .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		    .pNext        = &descriptor_set_layout_binding_flag_create_info,
		    .flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
		    .bindingCount = 4,
		    .pBindings    = bindings,
		};
		vkCreateDescriptorSetLayout(m_context->vk_device, &create_info, nullptr, &m_descriptor_set_layout);
	}

	// Allocate descriptor set
	{
		VkDescriptorSetAllocateInfo allocate_info = {
		    .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		    .pNext              = nullptr,
		    .descriptorPool     = m_context->vk_descriptor_pool,
		    .descriptorSetCount = 1,
		    .pSetLayouts        = &m_descriptor_set_layout,
		};
		vkAllocateDescriptorSets(m_context->vk_device, &allocate_info, &m_descriptor_set);
	}

	// Create pipeline layout
	{
		VkPipelineLayoutCreateInfo create_info = {
		    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		    .setLayoutCount         = 1,
		    .pSetLayouts            = &m_descriptor_set_layout,
		    .pushConstantRangeCount = 0,
		    .pPushConstantRanges    = nullptr,
		};
		vkCreatePipelineLayout(m_context->vk_device, &create_info, nullptr, &m_pipeline_layout);
	}

	// Create graphics pipeline
	{
		VkFormat color_attachment_formats[] =
		    {
		        VK_FORMAT_R8G8B8A8_UNORM,
		        VK_FORMAT_R16G16B16A16_SFLOAT,
		        VK_FORMAT_R16G16B16A16_SFLOAT,
		    };
		VkFormat depth_attachment_format = VK_FORMAT_D32_SFLOAT;

		VkPipelineRenderingCreateInfo pipeline_rendering_create_info = {
		    .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
		    .colorAttachmentCount    = 3,
		    .pColorAttachmentFormats = color_attachment_formats,
		    .depthAttachmentFormat   = depth_attachment_format,
		};

		VkPipelineInputAssemblyStateCreateInfo input_assembly_state_create_info = {
		    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		    .flags                  = 0,
		    .topology               = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		    .primitiveRestartEnable = VK_FALSE,
		};

		VkPipelineRasterizationStateCreateInfo rasterization_state_create_info = {
		    .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		    .depthClampEnable        = VK_FALSE,
		    .rasterizerDiscardEnable = VK_FALSE,
		    .polygonMode             = VK_POLYGON_MODE_FILL,
		    .cullMode                = VK_CULL_MODE_NONE,
		    .frontFace               = VK_FRONT_FACE_CLOCKWISE,
		    .depthBiasEnable         = VK_FALSE,
		    .depthBiasConstantFactor = 0.f,
		    .depthBiasClamp          = 0.f,
		    .depthBiasSlopeFactor    = 0.f,
		    .lineWidth               = 1.f,
		};

		VkPipelineColorBlendAttachmentState color_blend_attachment_states[] = {
		    {
		        .blendEnable    = VK_FALSE,
		        .colorWriteMask = 0xf,
		    },
		    {
		        .blendEnable    = VK_FALSE,
		        .colorWriteMask = 0xf,
		    },
		    {
		        .blendEnable    = VK_FALSE,
		        .colorWriteMask = 0xf,
		    },
		};

		VkPipelineColorBlendStateCreateInfo color_blend_state_create_info = {
		    .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		    .logicOpEnable   = VK_FALSE,
		    .attachmentCount = 3,
		    .pAttachments    = color_blend_attachment_states,
		};

		VkPipelineDepthStencilStateCreateInfo depth_stencil_state_create_info = {
		    .sType             = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		    .depthTestEnable   = VK_TRUE,
		    .depthWriteEnable  = VK_TRUE,
		    .depthCompareOp    = VK_COMPARE_OP_GREATER_OR_EQUAL,
		    .stencilTestEnable = VK_FALSE,

		    .front = {
		        .failOp    = VK_STENCIL_OP_KEEP,
		        .passOp    = VK_STENCIL_OP_KEEP,
		        .compareOp = VK_COMPARE_OP_ALWAYS,
		    },
		    .back = {
		        .failOp    = VK_STENCIL_OP_KEEP,
		        .passOp    = VK_STENCIL_OP_KEEP,
		        .compareOp = VK_COMPARE_OP_ALWAYS,
		    },
		};

		VkViewport viewport = {
		    .x        = 0,
		    .y        = 0,
		    .width    = static_cast<float>(width),
		    .height   = static_cast<float>(height),
		    .minDepth = 0.f,
		    .maxDepth = 1.f,
		};

		VkRect2D rect = {
		    .offset = VkOffset2D{
		        .x = 0,
		        .y = 0,
		    },
		    .extent = VkExtent2D{
		        .width  = width,
		        .height = height,
		    },
		};

		VkPipelineViewportStateCreateInfo viewport_state_create_info = {
		    .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		    .viewportCount = 1,
		    .pViewports    = &viewport,
		    .scissorCount  = 1,
		    .pScissors     = &rect,
		};

		VkPipelineMultisampleStateCreateInfo multisample_state_create_info = {
		    .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
		    .sampleShadingEnable  = VK_FALSE,
		};

		VkVertexInputAttributeDescription attribute_descriptions[] = {
		    {
		        .location = 0,
		        .binding  = 0,
		        .format   = VK_FORMAT_R32G32B32A32_SFLOAT,
		        .offset   = offsetof(Vertex, position),
		    },
		    {
		        .location = 1,
		        .binding  = 0,
		        .format   = VK_FORMAT_R32G32B32A32_SFLOAT,
		        .offset   = offsetof(Vertex, normal),
		    },
		    {
		        .location = 2,
		        .binding  = 0,
		        .format   = VK_FORMAT_R32G32B32A32_SFLOAT,
		        .offset   = offsetof(Vertex, tangent),
		    },
		};

		VkVertexInputBindingDescription binding_description = {
		    .binding   = 0,
		    .stride    = sizeof(Vertex),
		    .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
		};

		VkPipelineVertexInputStateCreateInfo vertex_input_state_create_info = {
		    .sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		    .vertexBindingDescriptionCount   = 1,
		    .pVertexBindingDescriptions      = &binding_description,
		    .vertexAttributeDescriptionCount = 3,
		    .pVertexAttributeDescriptions    = attribute_descriptions,
		};

		VkPipelineShaderStageCreateInfo pipeline_shader_stage_create_infos[] = {
		    {
		        .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		        .stage               = VK_SHADER_STAGE_VERTEX_BIT,
		        .module              = vert_shader,
		        .pName               = "main",
		        .pSpecializationInfo = nullptr,
		    },
		    {
		        .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		        .stage               = VK_SHADER_STAGE_FRAGMENT_BIT,
		        .module              = frag_shader,
		        .pName               = "main",
		        .pSpecializationInfo = nullptr,
		    },
		};

		VkGraphicsPipelineCreateInfo create_info = {
		    .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		    .pNext               = &pipeline_rendering_create_info,
		    .stageCount          = 2,
		    .pStages             = pipeline_shader_stage_create_infos,
		    .pVertexInputState   = &vertex_input_state_create_info,
		    .pInputAssemblyState = &input_assembly_state_create_info,
		    .pTessellationState  = nullptr,
		    .pViewportState      = &viewport_state_create_info,
		    .pRasterizationState = &rasterization_state_create_info,
		    .pMultisampleState   = &multisample_state_create_info,
		    .pDepthStencilState  = &depth_stencil_state_create_info,
		    .pColorBlendState    = &color_blend_state_create_info,
		    .pDynamicState       = nullptr,
		    .layout              = m_pipeline_layout,
		    .renderPass          = VK_NULL_HANDLE,
		    .subpass             = 0,
		    .basePipelineHandle  = VK_NULL_HANDLE,
		    .basePipelineIndex   = -1,
		};
		vkCreateGraphicsPipelines(m_context->vk_device, m_context->vk_pipeline_cache, 1, &create_info, nullptr, &m_pipeline);
	}

	// Destroy shader module
	vkDestroyShaderModule(m_context->vk_device, vert_shader, nullptr);
	vkDestroyShaderModule(m_context->vk_device, frag_shader, nullptr);

	// Create GBufferA
	{
		VkImageCreateInfo image_create_info = {
		    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		    .imageType     = VK_IMAGE_TYPE_2D,
		    .format        = VK_FORMAT_R8G8B8A8_UNORM,
		    .extent        = VkExtent3D{width, height, 1},
		    .mipLevels     = mip_level,
		    .arrayLayers   = 1,
		    .samples       = VK_SAMPLE_COUNT_1_BIT,
		    .tiling        = VK_IMAGE_TILING_OPTIMAL,
		    .usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		    .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
		    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};
		VmaAllocationCreateInfo allocation_create_info = {
		    .usage = VMA_MEMORY_USAGE_GPU_ONLY,
		};
		for (uint32_t i = 0; i < 2; i++)
		{
			vmaCreateImage(context.vma_allocator, &image_create_info, &allocation_create_info, &gbufferA[i].vk_image, &gbufferA[i].vma_allocation, nullptr);
			VkImageViewCreateInfo view_create_info = {
			    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			    .image            = gbufferA[i].vk_image,
			    .viewType         = VK_IMAGE_VIEW_TYPE_2D,
			    .format           = VK_FORMAT_R8G8B8A8_UNORM,
			    .components       = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
			    .subresourceRange = {
			        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
			        .baseMipLevel   = 0,
			        .levelCount     = mip_level,
			        .baseArrayLayer = 0,
			        .layerCount     = 1,
			    },
			};
			vkCreateImageView(m_context->vk_device, &view_create_info, nullptr, &gbufferA_view[i]);
			m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) gbufferA[i].vk_image, fmt::format("GBufferA - {}", i).c_str());
			m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) gbufferA_view[i], fmt::format("GBufferA View - {}", i).c_str());
		}
	}

	// Create GBufferB
	{
		for (uint32_t i = 0; i < 2; i++)
		{
			VkImageCreateInfo image_create_info = {
			    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			    .imageType     = VK_IMAGE_TYPE_2D,
			    .format        = VK_FORMAT_R16G16B16A16_SFLOAT,
			    .extent        = VkExtent3D{width, height, 1},
			    .mipLevels     = mip_level,
			    .arrayLayers   = 1,
			    .samples       = VK_SAMPLE_COUNT_1_BIT,
			    .tiling        = VK_IMAGE_TILING_OPTIMAL,
			    .usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			    .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
			    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			};
			VmaAllocationCreateInfo allocation_create_info = {
			    .usage = VMA_MEMORY_USAGE_GPU_ONLY,
			};
			vmaCreateImage(context.vma_allocator, &image_create_info, &allocation_create_info, &gbufferB[i].vk_image, &gbufferB[i].vma_allocation, nullptr);
			VkImageViewCreateInfo view_create_info = {
			    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			    .image            = gbufferB[i].vk_image,
			    .viewType         = VK_IMAGE_VIEW_TYPE_2D,
			    .format           = VK_FORMAT_R16G16B16A16_SFLOAT,
			    .components       = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
			    .subresourceRange = {
			        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
			        .baseMipLevel   = 0,
			        .levelCount     = mip_level,
			        .baseArrayLayer = 0,
			        .layerCount     = 1,
			    },
			};
			vkCreateImageView(m_context->vk_device, &view_create_info, nullptr, &gbufferB_view[i]);
			m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) gbufferB[i].vk_image, fmt::format("GBufferB - {}", i).c_str());
			m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) gbufferB_view[i], fmt::format("GBufferB View - {}", i).c_str());
		}
	}

	// Create GBufferC
	{
		for (uint32_t i = 0; i < 2; i++)
		{
			VkImageCreateInfo image_create_info = {
			    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			    .imageType     = VK_IMAGE_TYPE_2D,
			    .format        = VK_FORMAT_R16G16B16A16_SFLOAT,
			    .extent        = VkExtent3D{width, height, 1},
			    .mipLevels     = mip_level,
			    .arrayLayers   = 1,
			    .samples       = VK_SAMPLE_COUNT_1_BIT,
			    .tiling        = VK_IMAGE_TILING_OPTIMAL,
			    .usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			    .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
			    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			};
			VmaAllocationCreateInfo allocation_create_info = {
			    .usage = VMA_MEMORY_USAGE_GPU_ONLY,
			};
			vmaCreateImage(context.vma_allocator, &image_create_info, &allocation_create_info, &gbufferC[i].vk_image, &gbufferC[i].vma_allocation, nullptr);
			VkImageViewCreateInfo view_create_info = {
			    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			    .image            = gbufferC[i].vk_image,
			    .viewType         = VK_IMAGE_VIEW_TYPE_2D,
			    .format           = VK_FORMAT_R16G16B16A16_SFLOAT,
			    .components       = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
			    .subresourceRange = {
			        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
			        .baseMipLevel   = 0,
			        .levelCount     = mip_level,
			        .baseArrayLayer = 0,
			        .layerCount     = 1,
			    },
			};
			vkCreateImageView(m_context->vk_device, &view_create_info, nullptr, &gbufferC_view[i]);
			m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) gbufferC[i].vk_image, fmt::format("GBufferC - {}", i).c_str());
			m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) gbufferC_view[i], fmt::format("GBufferC View - {}", i).c_str());
		}
	}

	// Create depth stencil
	{
		for (uint32_t i = 0; i < 2; i++)
		{
			VkImageCreateInfo image_create_info = {
			    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			    .imageType     = VK_IMAGE_TYPE_2D,
			    .format        = VK_FORMAT_D32_SFLOAT,
			    .extent        = VkExtent3D{width, height, 1},
			    .mipLevels     = mip_level,
			    .arrayLayers   = 1,
			    .samples       = VK_SAMPLE_COUNT_1_BIT,
			    .tiling        = VK_IMAGE_TILING_OPTIMAL,
			    .usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			    .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
			    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			};
			VmaAllocationCreateInfo allocation_create_info = {
			    .usage = VMA_MEMORY_USAGE_GPU_ONLY,
			};
			vmaCreateImage(context.vma_allocator, &image_create_info, &allocation_create_info, &depth_buffer[i].vk_image, &depth_buffer[i].vma_allocation, nullptr);
			VkImageViewCreateInfo view_create_info = {
			    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			    .image            = depth_buffer[i].vk_image,
			    .viewType         = VK_IMAGE_VIEW_TYPE_2D,
			    .format           = VK_FORMAT_D32_SFLOAT,
			    .components       = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
			    .subresourceRange = {
			        .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
			        .baseMipLevel   = 0,
			        .levelCount     = mip_level,
			        .baseArrayLayer = 0,
			        .layerCount     = 1,
			    },
			};
			vkCreateImageView(m_context->vk_device, &view_create_info, nullptr, &depth_buffer_view[i]);
			m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) depth_buffer[i].vk_image, fmt::format("Depth Buffer - {}", i).c_str());
			m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) depth_buffer_view[i], fmt::format("Depth Buffer View - {}", i).c_str());
		}
	}

	// Create attachment info
	{
		for (uint32_t i = 0; i < 2; i++)
		{
			m_gbufferA_attachment_info[i] = {
			    .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
			    .imageView   = gbufferA_view[i],
			    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			    .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
			    .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
			    .clearValue  = {
			         .color = {
			             .uint32 = {0, 0, 0, 0},
                    },
                },
			};
			m_gbufferB_attachment_info[i] = {
			    .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
			    .imageView   = gbufferB_view[i],
			    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			    .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
			    .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
			    .clearValue  = {
			         .color = {
			             .float32 = {0, 0, 0, 0},
                    },
                },
			};
			m_gbufferC_attachment_info[i] = {
			    .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
			    .imageView   = gbufferC_view[i],
			    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			    .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
			    .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
			    .clearValue  = {
			         .color = {
			             .float32 = {0, 0, 0, 0},
                    },
                },
			};
			m_depth_stencil_view_attachment_info[i] = {
			    .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
			    .imageView   = depth_buffer_view[i],
			    .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
			    .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
			    .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
			    .clearValue  = {
			         .depthStencil = {
			             .depth = 0.f,
                    },
                },
			};
		}
	}
}

GBufferPass::~GBufferPass()
{
	vkDestroyPipelineLayout(m_context->vk_device, m_pipeline_layout, nullptr);
	vkDestroyPipeline(m_context->vk_device, m_pipeline, nullptr);
	vkDestroyDescriptorSetLayout(m_context->vk_device, m_descriptor_set_layout, nullptr);
	vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 1, &m_descriptor_set);
	for (uint32_t i = 0; i < 2; i++)
	{
		vkDestroyImageView(m_context->vk_device, gbufferA_view[i], nullptr);
		vkDestroyImageView(m_context->vk_device, gbufferB_view[i], nullptr);
		vkDestroyImageView(m_context->vk_device, gbufferC_view[i], nullptr);
		vkDestroyImageView(m_context->vk_device, depth_buffer_view[i], nullptr);
		vmaDestroyImage(m_context->vma_allocator, gbufferA[i].vk_image, gbufferA[i].vma_allocation);
		vmaDestroyImage(m_context->vma_allocator, gbufferB[i].vk_image, gbufferB[i].vma_allocation);
		vmaDestroyImage(m_context->vma_allocator, gbufferC[i].vk_image, gbufferC[i].vma_allocation);
		vmaDestroyImage(m_context->vma_allocator, depth_buffer[i].vk_image, depth_buffer[i].vma_allocation);
	}
}

void GBufferPass::init(VkCommandBuffer cmd_buffer)
{
	VkImageMemoryBarrier image_barriers[] = {
	    VkImageMemoryBarrier{
	        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .srcAccessMask       = 0,
	        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
	        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
	        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image               = gbufferA[0].vk_image,
	        .subresourceRange    = VkImageSubresourceRange{
	               .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	               .baseMipLevel   = 0,
	               .levelCount     = mip_level,
	               .baseArrayLayer = 0,
	               .layerCount     = 1,
            },
	    },
	    VkImageMemoryBarrier{
	        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .srcAccessMask       = 0,
	        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
	        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
	        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image               = gbufferB[0].vk_image,
	        .subresourceRange    = VkImageSubresourceRange{
	               .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	               .baseMipLevel   = 0,
	               .levelCount     = mip_level,
	               .baseArrayLayer = 0,
	               .layerCount     = 1,
            },
	    },
	    VkImageMemoryBarrier{
	        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .srcAccessMask       = 0,
	        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
	        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
	        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image               = gbufferC[0].vk_image,
	        .subresourceRange    = VkImageSubresourceRange{
	               .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	               .baseMipLevel   = 0,
	               .levelCount     = mip_level,
	               .baseArrayLayer = 0,
	               .layerCount     = 1,
            },
	    },
	    VkImageMemoryBarrier{
	        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .srcAccessMask       = 0,
	        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
	        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
	        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image               = depth_buffer[0].vk_image,
	        .subresourceRange    = VkImageSubresourceRange{
	               .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
	               .baseMipLevel   = 0,
	               .levelCount     = mip_level,
	               .baseArrayLayer = 0,
	               .layerCount     = 1,
            },
	    },
	    VkImageMemoryBarrier{
	        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .srcAccessMask       = 0,
	        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
	        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
	        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image               = gbufferA[1].vk_image,
	        .subresourceRange    = VkImageSubresourceRange{
	               .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	               .baseMipLevel   = 0,
	               .levelCount     = mip_level,
	               .baseArrayLayer = 0,
	               .layerCount     = 1,
            },
	    },
	    VkImageMemoryBarrier{
	        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .srcAccessMask       = 0,
	        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
	        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
	        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image               = gbufferB[1].vk_image,
	        .subresourceRange    = VkImageSubresourceRange{
	               .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	               .baseMipLevel   = 0,
	               .levelCount     = mip_level,
	               .baseArrayLayer = 0,
	               .layerCount     = 1,
            },
	    },
	    VkImageMemoryBarrier{
	        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .srcAccessMask       = 0,
	        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
	        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
	        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image               = gbufferC[1].vk_image,
	        .subresourceRange    = VkImageSubresourceRange{
	               .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	               .baseMipLevel   = 0,
	               .levelCount     = mip_level,
	               .baseArrayLayer = 0,
	               .layerCount     = 1,
            },
	    },
	    VkImageMemoryBarrier{
	        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .srcAccessMask       = 0,
	        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
	        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
	        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image               = depth_buffer[1].vk_image,
	        .subresourceRange    = VkImageSubresourceRange{
	               .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
	               .baseMipLevel   = 0,
	               .levelCount     = mip_level,
	               .baseArrayLayer = 0,
	               .layerCount     = 1,
            },
	    },
	};
	vkCmdPipelineBarrier(
	    cmd_buffer,
	    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	    0, 0, nullptr, 0, nullptr, 8, image_barriers);
}

void GBufferPass::update(const Scene &scene)
{
	VkDescriptorBufferInfo global_buffer_info = {
	    .buffer = scene.global_buffer.vk_buffer,
	    .offset = 0,
	    .range  = sizeof(GlobalBuffer),
	};

	VkDescriptorBufferInfo instance_buffer_info = {
	    .buffer = scene.instance_buffer.vk_buffer,
	    .offset = 0,
	    .range  = sizeof(Instance) * scene.scene_info.instance_count,
	};

	VkDescriptorBufferInfo material_buffer_info = {
	    .buffer = scene.material_buffer.vk_buffer,
	    .offset = 0,
	    .range  = sizeof(Material) * scene.scene_info.material_count,
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

	VkWriteDescriptorSet writes[] = {
	    {
	        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	        .dstSet           = m_descriptor_set,
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
	        .dstSet           = m_descriptor_set,
	        .dstBinding       = 1,
	        .dstArrayElement  = 0,
	        .descriptorCount  = static_cast<uint32_t>(texture_infos.size()),
	        .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	        .pImageInfo       = texture_infos.data(),
	        .pBufferInfo      = nullptr,
	        .pTexelBufferView = nullptr,
	    },
	    {
	        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	        .dstSet           = m_descriptor_set,
	        .dstBinding       = 2,
	        .dstArrayElement  = 0,
	        .descriptorCount  = 1,
	        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
	        .pImageInfo       = nullptr,
	        .pBufferInfo      = &instance_buffer_info,
	        .pTexelBufferView = nullptr,
	    },
	    {
	        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	        .dstSet           = m_descriptor_set,
	        .dstBinding       = 3,
	        .dstArrayElement  = 0,
	        .descriptorCount  = 1,
	        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
	        .pImageInfo       = nullptr,
	        .pBufferInfo      = &material_buffer_info,
	        .pTexelBufferView = nullptr,
	    },
	};

	vkUpdateDescriptorSets(m_context->vk_device, 4, writes, 0, nullptr);
}

void GBufferPass::draw(VkCommandBuffer cmd_buffer, const Scene &scene)
{
	m_context->begin_marker(cmd_buffer, "GBuffer Pass");
	{
		{
			VkImageMemoryBarrier image_barriers[] = {
			    {
			        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			        .srcAccessMask       = VK_ACCESS_SHADER_READ_BIT,
			        .dstAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			        .oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			        .newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .image               = gbufferA[m_context->ping_pong].vk_image,
			        .subresourceRange    = VkImageSubresourceRange{
			               .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
			               .baseMipLevel   = 0,
			               .levelCount     = mip_level,
			               .baseArrayLayer = 0,
			               .layerCount     = 1,
                    },
			    },
			    {
			        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			        .srcAccessMask       = VK_ACCESS_SHADER_READ_BIT,
			        .dstAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			        .oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			        .newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .image               = gbufferB[m_context->ping_pong].vk_image,
			        .subresourceRange    = VkImageSubresourceRange{
			               .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
			               .baseMipLevel   = 0,
			               .levelCount     = mip_level,
			               .baseArrayLayer = 0,
			               .layerCount     = 1,
                    },
			    },
			    {
			        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			        .srcAccessMask       = VK_ACCESS_SHADER_READ_BIT,
			        .dstAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			        .oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			        .newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .image               = gbufferC[m_context->ping_pong].vk_image,
			        .subresourceRange    = VkImageSubresourceRange{
			               .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
			               .baseMipLevel   = 0,
			               .levelCount     = mip_level,
			               .baseArrayLayer = 0,
			               .layerCount     = 1,
                    },
			    },
			    {
			        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			        .srcAccessMask       = VK_ACCESS_SHADER_READ_BIT,
			        .dstAccessMask       = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
			        .oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			        .newLayout           = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
			        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .image               = depth_buffer[m_context->ping_pong].vk_image,
			        .subresourceRange    = VkImageSubresourceRange{
			               .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
			               .baseMipLevel   = 0,
			               .levelCount     = mip_level,
			               .baseArrayLayer = 0,
			               .layerCount     = 1,
                    },
			    },
			};

			vkCmdPipelineBarrier(
			    cmd_buffer,
			    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			    0, 0, nullptr, 0, nullptr, 4, image_barriers);
		}

		m_context->begin_marker(cmd_buffer, "GBuffer");
		{
			VkRenderingAttachmentInfo color_attachments[] = {
			    m_gbufferA_attachment_info[m_context->ping_pong],
			    m_gbufferB_attachment_info[m_context->ping_pong],
			    m_gbufferC_attachment_info[m_context->ping_pong],
			};
			VkRenderingInfo rendering_info = {
			    .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
			    .renderArea           = {0, 0, width, height},
			    .layerCount           = 1,
			    .colorAttachmentCount = 3,
			    .pColorAttachments    = color_attachments,
			    .pDepthAttachment     = &m_depth_stencil_view_attachment_info[m_context->ping_pong],
			};
			VkDeviceSize offsets[] = {0};

			vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline_layout, 0, 1, &m_descriptor_set, 0, nullptr);
			vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
			vkCmdBindVertexBuffers(cmd_buffer, 0, 1, &scene.vertex_buffer.vk_buffer, offsets);
			vkCmdBindIndexBuffer(cmd_buffer, scene.index_buffer.vk_buffer, 0, VK_INDEX_TYPE_UINT32);
			vkCmdBeginRendering(cmd_buffer, &rendering_info);
			vkCmdDrawIndexedIndirect(cmd_buffer, scene.indirect_draw_buffer.vk_buffer, 0, scene.scene_info.instance_count, sizeof(VkDrawIndexedIndirectCommand));
			vkCmdEndRendering(cmd_buffer);
		}
		m_context->end_marker(cmd_buffer);

		m_context->begin_marker(cmd_buffer, "Generate Mipmaps");
		{
			{
				VkImageBlit blit_info = {
				    .srcSubresource = VkImageSubresourceLayers{
				        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
				        .mipLevel       = 0,
				        .baseArrayLayer = 0,
				        .layerCount     = 1,
				    },
				    .srcOffsets = {
				        VkOffset3D{
				            .x = 0,
				            .y = 0,
				            .z = 0,
				        },
				        VkOffset3D{
				            .x = static_cast<int32_t>(width),
				            .y = static_cast<int32_t>(height),
				            .z = 1,
				        },
				    },
				    .dstSubresource = VkImageSubresourceLayers{
				        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
				        .mipLevel       = 0,
				        .baseArrayLayer = 0,
				        .layerCount     = 1,
				    },
				    .dstOffsets = {
				        VkOffset3D{
				            .x = 0,
				            .y = 0,
				            .z = 0,
				        },
				        VkOffset3D{
				            .x = static_cast<int32_t>(width),
				            .y = static_cast<int32_t>(height),
				            .z = 1,
				        },
				    },
				};
				VkImageMemoryBarrier image_barriers[] = {
				    {
				        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				        .srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				        .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
				        .oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				        .image               = gbufferA[m_context->ping_pong].vk_image,
				        .subresourceRange    = VkImageSubresourceRange{
				               .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
				               .baseMipLevel   = 0,
				               .levelCount     = mip_level,
				               .baseArrayLayer = 0,
				               .layerCount     = 1,
                        },
				    },
				    {
				        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				        .srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				        .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
				        .oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				        .image               = gbufferB[m_context->ping_pong].vk_image,
				        .subresourceRange    = VkImageSubresourceRange{
				               .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
				               .baseMipLevel   = 0,
				               .levelCount     = mip_level,
				               .baseArrayLayer = 0,
				               .layerCount     = 1,
                        },
				    },
				    {
				        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				        .srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				        .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
				        .oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				        .image               = gbufferC[m_context->ping_pong].vk_image,
				        .subresourceRange    = VkImageSubresourceRange{
				               .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
				               .baseMipLevel   = 0,
				               .levelCount     = mip_level,
				               .baseArrayLayer = 0,
				               .layerCount     = 1,
                        },
				    },
				    {
				        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				        .srcAccessMask       = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
				        .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
				        .oldLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
				        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				        .image               = depth_buffer[m_context->ping_pong].vk_image,
				        .subresourceRange    = VkImageSubresourceRange{
				               .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
				               .baseMipLevel   = 0,
				               .levelCount     = mip_level,
				               .baseArrayLayer = 0,
				               .layerCount     = 1,
                        },
				    },
				};

				vkCmdPipelineBarrier(
				    cmd_buffer,
				    VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
				    VK_PIPELINE_STAGE_TRANSFER_BIT,
				    0, 0, nullptr, 0, nullptr, 4, image_barriers);
			}

			for (uint32_t i = 1; i < mip_level; i++)
			{
				VkImageBlit blit_info = {
				    .srcSubresource = VkImageSubresourceLayers{
				        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
				        .mipLevel       = i - 1,
				        .baseArrayLayer = 0,
				        .layerCount     = 1,
				    },
				    .srcOffsets = {
				        VkOffset3D{
				            .x = 0,
				            .y = 0,
				            .z = 0,
				        },
				        VkOffset3D{
				            .x = static_cast<int32_t>(width >> (i - 1)),
				            .y = static_cast<int32_t>(height >> (i - 1)),
				            .z = 1,
				        },
				    },
				    .dstSubresource = VkImageSubresourceLayers{
				        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
				        .mipLevel       = i,
				        .baseArrayLayer = 0,
				        .layerCount     = 1,
				    },
				    .dstOffsets = {
				        VkOffset3D{
				            .x = 0,
				            .y = 0,
				            .z = 0,
				        },
				        VkOffset3D{
				            .x = static_cast<int32_t>(width >> i),
				            .y = static_cast<int32_t>(height >> i),
				            .z = 1,
				        },
				    },
				};

				{
					VkImageMemoryBarrier image_barriers[] = {
					    {
					        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
					        .srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
					        .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
					        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					        .image               = gbufferA[m_context->ping_pong].vk_image,
					        .subresourceRange    = VkImageSubresourceRange{
					               .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
					               .baseMipLevel   = i,
					               .levelCount     = 1,
					               .baseArrayLayer = 0,
					               .layerCount     = 1,
                            },
					    },
					    {
					        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
					        .srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
					        .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
					        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					        .image               = gbufferB[m_context->ping_pong].vk_image,
					        .subresourceRange    = VkImageSubresourceRange{
					               .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
					               .baseMipLevel   = i,
					               .levelCount     = 1,
					               .baseArrayLayer = 0,
					               .layerCount     = 1,
                            },
					    },
					    {
					        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
					        .srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
					        .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
					        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					        .image               = gbufferC[m_context->ping_pong].vk_image,
					        .subresourceRange    = VkImageSubresourceRange{
					               .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
					               .baseMipLevel   = i,
					               .levelCount     = 1,
					               .baseArrayLayer = 0,
					               .layerCount     = 1,
                            },
					    },
					    {
					        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
					        .srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
					        .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
					        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					        .image               = depth_buffer[m_context->ping_pong].vk_image,
					        .subresourceRange    = VkImageSubresourceRange{
					               .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
					               .baseMipLevel   = i,
					               .levelCount     = 1,
					               .baseArrayLayer = 0,
					               .layerCount     = 1,
                            },
					    },
					};
					vkCmdPipelineBarrier(
					    cmd_buffer,
					    VK_PIPELINE_STAGE_TRANSFER_BIT,
					    VK_PIPELINE_STAGE_TRANSFER_BIT,
					    0, 0, nullptr, 0, nullptr, 4, image_barriers);
				}

				vkCmdBlitImage(
				    cmd_buffer,
				    gbufferA[m_context->ping_pong].vk_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				    gbufferA[m_context->ping_pong].vk_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				    1, &blit_info, VK_FILTER_LINEAR);
				vkCmdBlitImage(
				    cmd_buffer,
				    gbufferB[m_context->ping_pong].vk_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				    gbufferB[m_context->ping_pong].vk_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				    1, &blit_info, VK_FILTER_LINEAR);
				vkCmdBlitImage(
				    cmd_buffer,
				    gbufferC[m_context->ping_pong].vk_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				    gbufferC[m_context->ping_pong].vk_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				    1, &blit_info, VK_FILTER_LINEAR);
				blit_info.srcSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
				blit_info.dstSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
				vkCmdBlitImage(
				    cmd_buffer,
				    depth_buffer[m_context->ping_pong].vk_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				    depth_buffer[m_context->ping_pong].vk_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				    1, &blit_info, VK_FILTER_NEAREST);

				{
					VkImageMemoryBarrier image_barriers[] = {
					    {
					        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
					        .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
					        .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
					        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					        .image               = gbufferA[m_context->ping_pong].vk_image,
					        .subresourceRange    = VkImageSubresourceRange{
					               .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
					               .baseMipLevel   = i,
					               .levelCount     = 1,
					               .baseArrayLayer = 0,
					               .layerCount     = 1,
                            },
					    },
					    {
					        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
					        .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
					        .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
					        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					        .image               = gbufferB[m_context->ping_pong].vk_image,
					        .subresourceRange    = VkImageSubresourceRange{
					               .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
					               .baseMipLevel   = i,
					               .levelCount     = 1,
					               .baseArrayLayer = 0,
					               .layerCount     = 1,
                            },
					    },
					    {
					        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
					        .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
					        .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
					        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					        .image               = gbufferC[m_context->ping_pong].vk_image,
					        .subresourceRange    = VkImageSubresourceRange{
					               .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
					               .baseMipLevel   = i,
					               .levelCount     = 1,
					               .baseArrayLayer = 0,
					               .layerCount     = 1,
                            },
					    },
					    {
					        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
					        .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
					        .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
					        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					        .image               = depth_buffer[m_context->ping_pong].vk_image,
					        .subresourceRange    = VkImageSubresourceRange{
					               .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
					               .baseMipLevel   = i,
					               .levelCount     = 1,
					               .baseArrayLayer = 0,
					               .layerCount     = 1,
                            },
					    },
					};
					vkCmdPipelineBarrier(
					    cmd_buffer,
					    VK_PIPELINE_STAGE_TRANSFER_BIT,
					    VK_PIPELINE_STAGE_TRANSFER_BIT,
					    0, 0, nullptr, 0, nullptr, 4, image_barriers);
				}
			}

			{
				VkImageMemoryBarrier image_barriers[] = {
				    {
				        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				        .srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
				        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
				        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				        .image               = gbufferA[m_context->ping_pong].vk_image,
				        .subresourceRange    = VkImageSubresourceRange{
				               .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
				               .baseMipLevel   = 0,
				               .levelCount     = mip_level,
				               .baseArrayLayer = 0,
				               .layerCount     = 1,
                        },
				    },
				    {
				        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				        .srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
				        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
				        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				        .image               = gbufferB[m_context->ping_pong].vk_image,
				        .subresourceRange    = VkImageSubresourceRange{
				               .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
				               .baseMipLevel   = 0,
				               .levelCount     = mip_level,
				               .baseArrayLayer = 0,
				               .layerCount     = 1,
                        },
				    },
				    {
				        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				        .srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
				        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
				        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				        .image               = gbufferC[m_context->ping_pong].vk_image,
				        .subresourceRange    = VkImageSubresourceRange{
				               .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
				               .baseMipLevel   = 0,
				               .levelCount     = mip_level,
				               .baseArrayLayer = 0,
				               .layerCount     = 1,
                        },
				    },
				    {
				        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				        .srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
				        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
				        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				        .image               = depth_buffer[m_context->ping_pong].vk_image,
				        .subresourceRange    = VkImageSubresourceRange{
				               .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
				               .baseMipLevel   = 0,
				               .levelCount     = mip_level,
				               .baseArrayLayer = 0,
				               .layerCount     = 1,
                        },
				    },
				};
				vkCmdPipelineBarrier(
				    cmd_buffer,
				    VK_PIPELINE_STAGE_TRANSFER_BIT,
				    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				    0, 0, nullptr, 0, nullptr, 4, image_barriers);
			}
		}
		m_context->end_marker(cmd_buffer);
	}
	m_context->end_marker(cmd_buffer);
}
