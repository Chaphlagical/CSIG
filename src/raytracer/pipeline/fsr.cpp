#include "render/pipeline/fsr.hpp"
#include "render/common.hpp"

#define A_CPU
#include "ffx_a.h"
#include "ffx_fsr1.h"

static unsigned char g_fsr1_fp16_easu[] = {
#include "fsr1_fp16_easu.comp.spv.h"
};

static unsigned char g_fsr1_fp16_rcas[] = {
#include "fsr1_fp16_rcas.comp.spv.h"
};

static unsigned char g_fsr1_fp32_easu[] = {
#include "fsr1_fp32_easu.comp.spv.h"
};

static unsigned char g_fsr1_fp32_rcas[] = {
#include "fsr1_fp32_rcas.comp.spv.h"
};


FSR::FSR(const Context &context) :
    m_context(&context)
{
    // Upsampled Image
    {
        VkImageCreateInfo image_create_info = {
            .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType     = VK_IMAGE_TYPE_2D,
            .format        = VK_FORMAT_R16G16B16A16_UNORM,
            .extent        = VkExtent3D{m_context->extent.width, m_context->extent.height, 1},
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
        vmaCreateImage(context.vma_allocator, &image_create_info, &allocation_create_info, &upsampled_image.vk_image, &upsampled_image.vma_allocation, nullptr);
        VkImageViewCreateInfo view_create_info = {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image            = upsampled_image.vk_image,
            .viewType         = VK_IMAGE_VIEW_TYPE_2D,
            .format           = VK_FORMAT_R16G16B16A16_UNORM,
            .components       = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
            .subresourceRange = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel   = 0,
                .levelCount     = 1,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
        };
        vkCreateImageView(context.vk_device, &view_create_info, nullptr, &upsampled_image_view);
        m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) upsampled_image.vk_image, "FSR upsampled Image");
        m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) upsampled_image_view, "FSR upsampled Image View");
    }

    // Intermediate Image
    {
        VkImageCreateInfo image_create_info = {
            .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType     = VK_IMAGE_TYPE_2D,
            .format        = VK_FORMAT_R16G16B16A16_UNORM,
            .extent        = VkExtent3D{m_context->extent.width, m_context->extent.height, 1},
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
        vmaCreateImage(context.vma_allocator, &image_create_info, &allocation_create_info, &intermediate_image.vk_image, &intermediate_image.vma_allocation, nullptr);
        VkImageViewCreateInfo view_create_info = {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image            = intermediate_image.vk_image,
            .viewType         = VK_IMAGE_VIEW_TYPE_2D,
            .format           = VK_FORMAT_R16G16B16A16_UNORM,
            .components       = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
            .subresourceRange = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel   = 0,
                .levelCount     = 1,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
        };
        vkCreateImageView(context.vk_device, &view_create_info, nullptr, &intermediate_image_view);
        m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) intermediate_image.vk_image, "FSR intermediate Image");
        m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) intermediate_image_view, "FSR intermediate Image View");
    }

    // Image sampler
    {
        VkSamplerCreateInfo info = {};
        info.sType               = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter           = VK_FILTER_LINEAR;
        info.minFilter           = VK_FILTER_LINEAR;
        info.mipmapMode          = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU        = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV        = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW        = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.minLod              = -1000;
        info.maxLod              = 1000;
        info.maxAnisotropy       = 1.0f;
        VkResult res             = vkCreateSampler(context.vk_device, &info, NULL, &m_sampler);
        assert(res == VK_SUCCESS);
    }

    // FSR pass common infrastructure
    {
        // Create descriptor set layout
        {
            VkDescriptorSetLayoutBinding bindings[] = {
                // Uniform buffers for easu & rcas; offseted differently for each
                {
                    .binding            = 0,
                    .descriptorType     = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                    .descriptorCount    = 1,
                    .stageFlags         = VK_SHADER_STAGE_COMPUTE_BIT,
                    .pImmutableSamplers = NULL
                },
                // Input Image
                {
                    .binding            = 1,
                    .descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                    .descriptorCount    = 1,
                    .stageFlags         = VK_SHADER_STAGE_COMPUTE_BIT,
                    .pImmutableSamplers = NULL,
                },
                // Output Image
                {
                    .binding            = 2,
                    .descriptorType     = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    .descriptorCount    = 1,
                    .stageFlags         = VK_SHADER_STAGE_COMPUTE_BIT,
                    .pImmutableSamplers = NULL,
                },
                // Input Sampler
                {
                    .binding            = 3,
                    .descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLER,
                    .descriptorCount    = 1,
                    .stageFlags         = VK_SHADER_STAGE_COMPUTE_BIT,
                    .pImmutableSamplers = &m_sampler,
                }
            };
            VkDescriptorSetLayoutCreateInfo create_info = {
                .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                .flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
                .bindingCount = 4,
                .pBindings    = bindings,
            };
            vkCreateDescriptorSetLayout(m_context->vk_device, &create_info, nullptr, &m_descriptor_set_layout);
        }

        // Allocate descriptor set
        {
            VkDescriptorSetLayout descriptor_set_layouts[] = {
                m_descriptor_set_layout,
	            m_descriptor_set_layout,
	            m_descriptor_set_layout,
	            m_descriptor_set_layout,
            };

            VkDescriptorSetAllocateInfo allocate_info = {
                .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .pNext              = nullptr,
                .descriptorPool     = m_context->vk_descriptor_pool,
                .descriptorSetCount = 1,
                .pSetLayouts        = descriptor_set_layouts,
            };
	        vkAllocateDescriptorSets(m_context->vk_device, &allocate_info, &m_rcas_descriptor_set);
	        allocate_info.descriptorSetCount = 4;
	        vkAllocateDescriptorSets(m_context->vk_device, &allocate_info, m_easu_descriptor_sets);
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

        // Allocate uniform buffer
        {
            VkBufferCreateInfo buffer_create_info = {
                .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .size        = pad_uniform_buffer_size(context, sizeof(FSRPassUniforms)) * 2,
                .usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            };
            VmaAllocationCreateInfo allocation_create_info = {
                .usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
            };
            VmaAllocationInfo allocation_info = {};

            vmaCreateBuffer(context.vma_allocator, &buffer_create_info, &allocation_create_info, &m_fsr_params_buffer.vk_buffer, &m_fsr_params_buffer.vma_allocation, &allocation_info);
            m_context->set_object_name(VK_OBJECT_TYPE_BUFFER, (uint64_t) m_fsr_params_buffer.vk_buffer, "FSR easu and rcas parameter Buffer");
        }
    }

    // FSR easu pass
    {
        // Create shader module
        VkShaderModule easuShader = VK_NULL_HANDLE;
        if (context.FsrFp16Enabled)
        {
		    easuShader = build_shader_module(context, sizeof(g_fsr1_fp16_easu), reinterpret_cast<uint32_t *>(g_fsr1_fp16_easu));
        }
        else
        {
		    easuShader = build_shader_module(context, sizeof(g_fsr1_fp32_easu), reinterpret_cast<uint32_t *>(g_fsr1_fp32_easu));
        }

        // Create pipeline
        {
            VkComputePipelineCreateInfo create_info = {
                .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .stage = {
                    .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage               = VK_SHADER_STAGE_COMPUTE_BIT,
                    .module              = easuShader,
                    .pName               = "main",
                    .pSpecializationInfo = nullptr,
                },
                .layout             = m_pipeline_layout,
                .basePipelineHandle = VK_NULL_HANDLE,
                .basePipelineIndex  = -1,
            };
            vkCreateComputePipelines(m_context->vk_device, m_context->vk_pipeline_cache, 1, &create_info, nullptr, &m_pipeline_easu);
            vkDestroyShaderModule(m_context->vk_device, easuShader, nullptr);
        }
    }

    // FSR rcas pass
    {
        // Create shader module
        VkShaderModule rcasShader = VK_NULL_HANDLE;
        if (context.FsrFp16Enabled)
        {
		    rcasShader = build_shader_module(context, sizeof(g_fsr1_fp16_rcas), reinterpret_cast<uint32_t *>(g_fsr1_fp16_rcas));
        }
        else
        {
		    rcasShader = build_shader_module(context, sizeof(g_fsr1_fp32_rcas), reinterpret_cast<uint32_t *>(g_fsr1_fp32_rcas));
        }

        // Create pipeline
        {
            VkComputePipelineCreateInfo create_info = {
                .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .stage = {
                    .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage               = VK_SHADER_STAGE_COMPUTE_BIT,
                    .module              = rcasShader,
                    .pName               = "main",
                    .pSpecializationInfo = nullptr,
                },
                .layout             = m_pipeline_layout,
                .basePipelineHandle = VK_NULL_HANDLE,
                .basePipelineIndex  = -1,
            };
            vkCreateComputePipelines(m_context->vk_device, m_context->vk_pipeline_cache, 1, &create_info, nullptr, &m_pipeline_rcas);
            vkDestroyShaderModule(m_context->vk_device, rcasShader, nullptr);
        }
    }
}

FSR::~FSR()
{
    vkDestroyPipelineLayout(m_context->vk_device, m_pipeline_layout, nullptr);
    vkDestroyPipeline(m_context->vk_device, m_pipeline_easu, nullptr);
    vkDestroyPipeline(m_context->vk_device, m_pipeline_rcas, nullptr);
    vkDestroyDescriptorSetLayout(m_context->vk_device, m_descriptor_set_layout, nullptr);
    vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 4, m_easu_descriptor_sets);
	vkFreeDescriptorSets(m_context->vk_device, m_context->vk_descriptor_pool, 1, &m_rcas_descriptor_set);
    vkDestroyImageView(m_context->vk_device, upsampled_image_view, nullptr);
    vkDestroySampler(m_context->vk_device, m_sampler, nullptr);
    vmaDestroyImage(m_context->vma_allocator, upsampled_image.vk_image, upsampled_image.vma_allocation);
    vmaDestroyImage(m_context->vma_allocator, intermediate_image.vk_image, intermediate_image.vma_allocation);
	vmaDestroyBuffer(m_context->vma_allocator, m_fsr_params_buffer.vk_buffer, m_fsr_params_buffer.vma_allocation);
}

void FSR::init(VkCommandBuffer cmd_buffer)
{
    VkImageMemoryBarrier image_barrier_upsampled = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = 0,
        .dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = upsampled_image.vk_image,
        .subresourceRange    = VkImageSubresourceRange{
               .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
               .baseMipLevel   = 0,
               .levelCount     = 1,
               .baseArrayLayer = 0,
               .layerCount     = 1,
        },
    };
    vkCmdPipelineBarrier(
        cmd_buffer,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 0, nullptr, 0, nullptr, 1, &image_barrier_upsampled);

    VkImageMemoryBarrier image_barrier_intermediate = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = 0,
        .dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = intermediate_image.vk_image,
        .subresourceRange    = VkImageSubresourceRange{
               .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
               .baseMipLevel   = 0,
               .levelCount     = 1,
               .baseArrayLayer = 0,
               .layerCount     = 1,
        },
    };
    vkCmdPipelineBarrier(
        cmd_buffer,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 0, nullptr, 0, nullptr, 1, &image_barrier_intermediate);
}

void FSR::update(const Scene &scene, VkImageView pt_result[2], VkImageView hybrid_result[2])
{
	// easu descriptors
	for (int i = 0; i < 4; i++)
    {
		VkImageView previous_result = (i >= 2) ? hybrid_result[i - 2] : pt_result[i];

		VkDescriptorImageInfo previous_result_info = {
            .sampler     = VK_NULL_HANDLE,
	        .imageView   = previous_result,
	        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    };

		VkDescriptorImageInfo intermediate_write_info = {
		    .sampler     = VK_NULL_HANDLE,
		    .imageView   = intermediate_image_view,
		    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
		};

        VkDescriptorBufferInfo easu_uniform_info = {
		    .buffer = m_fsr_params_buffer.vk_buffer,
		    .offset = 0,
		    .range  = sizeof(FSRPassUniforms)
        };

        VkWriteDescriptorSet writes[] = {
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_easu_descriptor_sets[i],
		        .dstBinding       = 0,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		        .pImageInfo       = nullptr,
		        .pBufferInfo      = &easu_uniform_info,
		        .pTexelBufferView = nullptr,
		    },
            // Input for easu
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_easu_descriptor_sets[i],
		        .dstBinding       = 1,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
		        .pImageInfo       = &previous_result_info,
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
            // Output for easu
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_easu_descriptor_sets[i],
		        .dstBinding       = 2,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		        .pImageInfo       = &intermediate_write_info,
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		};

        vkUpdateDescriptorSets(m_context->vk_device, 3, writes, 0, nullptr);
	}

	// rcas descriptors
	{
		VkDescriptorImageInfo easu_result_info = {
		    .sampler     = VK_NULL_HANDLE,
		    .imageView   = intermediate_image_view,
		    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		};

		VkDescriptorImageInfo result_write_info = {
		    .sampler     = VK_NULL_HANDLE,
		    .imageView   = upsampled_image_view,
		    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
		};

		VkDescriptorBufferInfo rcas_uniform_info = {
		    .buffer = m_fsr_params_buffer.vk_buffer,
		    .offset = pad_uniform_buffer_size(*m_context, sizeof(FSRPassUniforms)),
		    .range  = sizeof(FSRPassUniforms)
        };

		VkWriteDescriptorSet writes[] = {
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_rcas_descriptor_set,
		        .dstBinding       = 0,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		        .pImageInfo       = nullptr,
		        .pBufferInfo      = &rcas_uniform_info,
		        .pTexelBufferView = nullptr,
		    },
		    // Input for rcas
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_rcas_descriptor_set,
		        .dstBinding       = 1,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
		        .pImageInfo       = &easu_result_info,
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		    // Output for rcas
		    {
		        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .dstSet           = m_rcas_descriptor_set,
		        .dstBinding       = 2,
		        .dstArrayElement  = 0,
		        .descriptorCount  = 1,
		        .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		        .pImageInfo       = &result_write_info,
		        .pBufferInfo      = nullptr,
		        .pTexelBufferView = nullptr,
		    },
		};

		vkUpdateDescriptorSets(m_context->vk_device, 3, writes, 0, nullptr);
	}

    // initialize uniform buffer data; this will only need to do once hadn't the viewport changed
    {
        memset(&m_easu_buffer_data, 0, sizeof(FSRPassUniforms));
        memset(&m_rcas_buffer_data, 0, sizeof(FSRPassUniforms));

		FsrEasuCon(
		    reinterpret_cast<AU1 *>(&m_easu_buffer_data.Const0),
		    reinterpret_cast<AU1 *>(&m_easu_buffer_data.Const1),
		    reinterpret_cast<AU1 *>(&m_easu_buffer_data.Const2),
		    reinterpret_cast<AU1 *>(&m_easu_buffer_data.Const3),
            // Input sizes
            static_cast<AF1>(m_context->renderExtent.width),
		    static_cast<AF1>(m_context->renderExtent.height),
		    static_cast<AF1>(m_context->renderExtent.width),
		    static_cast<AF1>(m_context->renderExtent.height),
            // Output sizes
		    (AF1) m_context->extent.width, (AF1) m_context->extent.height
        );
        // fsr sample: (hdr && !pState->bUseRcas) ? 1 : 0;
		m_easu_buffer_data.Sample[0] = (m_isHDR && !m_useRCAS) ? 1 : 0;

        FsrRcasCon(reinterpret_cast<AU1 *>(&m_rcas_buffer_data.Const0), m_rcasAttenuation);
        // hdr ? 1 : 0
		m_rcas_buffer_data.Sample[0] = (m_isHDR ? 1 : 0);

        // initiate a transfer to uniform buffer
		std::vector<char> paddedData(pad_uniform_buffer_size(*m_context, sizeof(FSRPassUniforms)) * 2);
		std::memcpy(paddedData.data(), &m_easu_buffer_data, sizeof(FSRPassUniforms));
		std::memcpy(paddedData.data() + pad_uniform_buffer_size(*m_context, sizeof(FSRPassUniforms)), &m_rcas_buffer_data, sizeof(FSRPassUniforms));

        copy_to_vulkan_buffer(*m_context, m_fsr_params_buffer, paddedData.data(), paddedData.size());
    }
}

void FSR::draw(VkCommandBuffer cmd_buffer)
{
	// This value is the image region dimension that each thread group of the FSR shader operates on
	static const int threadGroupWorkRegionDim = 16;

	int dispatchX = (m_context->extent.width + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;
	int dispatchY = (m_context->extent.height + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;

    {
		VkImageMemoryBarrier image_barriers[] = {
		    {
		        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask       = VK_ACCESS_SHADER_READ_BIT,
		        .dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
		        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
		        .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image               = intermediate_image.vk_image,
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
		        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
		        .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image               = upsampled_image.vk_image,
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

	m_context->begin_marker(cmd_buffer, "FSR EASU");
	{
        // pt0, pt1, hybrid0, hybrid1
		int input_set_id = 0;
		if (!m_is_pathtracing)
			input_set_id += 2;

        if (m_context->ping_pong)
			input_set_id += 1;

		vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline_layout, 0, 1, &m_easu_descriptor_sets[input_set_id], 0, nullptr);
		vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline_easu);
		vkCmdDispatch(cmd_buffer, dispatchX, dispatchY, 1);
	}
	m_context->end_marker(cmd_buffer);

    {
		VkImageMemoryBarrier image_barriers[] = {
		    {
		        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
		        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
		        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
		        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image               = intermediate_image.vk_image,
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

	m_context->begin_marker(cmd_buffer, "FSR RCAS");
	{
		vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline_layout, 0, 1, &m_rcas_descriptor_set, 0, nullptr);
		vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline_rcas);
		vkCmdDispatch(cmd_buffer, dispatchX, dispatchY, 1);
	}
	m_context->end_marker(cmd_buffer);
}

bool FSR::draw_ui()
{
    return false;
}

void FSR::set_pathtracing(bool enable)
{
	m_is_pathtracing = enable;
}
