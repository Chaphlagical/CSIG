#pragma once

#include "render/common.hpp"
#include "render/context.hpp"
#include "render/scene.hpp"

#include <glm/glm.hpp>
#include <map>

// 1.0 version of AMD FidelityFX Super Resolution
struct FSR
{
  public:
	FSR(const Context &context);

	~FSR();

	void init(VkCommandBuffer cmd_buffer);

	void update(const Scene &scene, VkImageView previous_result);

	void draw(VkCommandBuffer cmd_buffer);

	bool draw_ui();

	void set_pathtracing(bool enable);
	
	static inline VkExtent2D get_render_extent(float scaleFactor, VkExtent2D extent)
	{
		VkExtent2D renderExtent;
		renderExtent.width = (int)(extent.width / scaleFactor);
		renderExtent.height = (int)(extent.height / scaleFactor);

		return renderExtent;
	}

  public:
	Texture     upsampled_image, intermediate_image;
	VkSampler   m_sampler = VK_NULL_HANDLE;
	VkImageView upsampled_image_view = VK_NULL_HANDLE;
	VkImageView intermediate_image_view = VK_NULL_HANDLE;

  private:
	const Context *m_context = nullptr;

	bool m_is_pathtracing = false;
	
	// TODO: parse this from context config
	bool  m_useRCAS         = true;
	float m_rcasAttenuation = 1.0f;
	bool  m_isHDR           = true;
	
	struct FSRPassUniforms
	{
		uint Const0[4];
		uint Const1[4];
		uint Const2[4];
		uint Const3[4];
		uint Sample[4];
	};

	FSRPassUniforms m_easu_buffer_data, m_rcas_buffer_data;

	Buffer m_fsr_params_buffer;

	VkPipelineLayout      m_pipeline_layout           = VK_NULL_HANDLE;
	VkPipeline            m_pipeline_easu             = VK_NULL_HANDLE;
	VkPipeline            m_pipeline_rcas             = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_descriptor_set_layout     = VK_NULL_HANDLE;
	
	// prev -(easu)-> intermediate image
	VkDescriptorSet       m_easu_descriptor_set = VK_NULL_HANDLE;
	
	// intermediate image -(rcas)-> final image (that is, upsampled_image)
	VkDescriptorSet       m_rcas_descriptor_set = VK_NULL_HANDLE;
	
	inline static VkShaderModule build_shader_module(const Context &context, size_t codeSize, uint32_t* codeData)
	{
		// Create shader module
		VkShaderModule shader = VK_NULL_HANDLE;
		{
			VkShaderModuleCreateInfo create_info = {
			    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			    .codeSize = codeSize,
			    .pCode    = codeData,
			};
			vkCreateShaderModule(context.vk_device, &create_info, nullptr, &shader);
		}

		return shader;
	}

	inline static size_t pad_uniform_buffer_size(const Context &context, size_t originalSize)
	{
		// Calculate required alignment based on minimum device offset alignment
		size_t minUboAlignment = context.physical_device_properties.limits.minUniformBufferOffsetAlignment;
		size_t alignedSize     = originalSize;
		if (minUboAlignment > 0)
		{
			alignedSize = (alignedSize + minUboAlignment - 1) & ~(minUboAlignment - 1);
		}
		return alignedSize;
	}
};