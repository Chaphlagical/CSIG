#pragma once

#include "context.hpp"
#include "pipeline/tonemap.hpp"

struct FSR1Pass
{
  public:
	FSR1Pass(const Context &context, const Tonemap& tonemap);

	~FSR1Pass();

	void init();

	void draw(CommandBufferRecorder &recorder);

	bool draw_ui();

  public:
	Texture upsampled_image;
	Texture intermediate_image;

	VkImageView upsampled_image_view    = VK_NULL_HANDLE;
	VkImageView intermediate_image_view = VK_NULL_HANDLE;

	VkSampler sampler = VK_NULL_HANDLE;

  private:
	const Context *m_context = nullptr;

	bool  m_useRCAS         = true;
	float m_rcasAttenuation = 0.25f;
	bool  m_isHDR           = false;

	struct FSRPassUniforms
	{
		uint32_t Const0[4];
		uint32_t Const1[4];
		uint32_t Const2[4];
		uint32_t Const3[4];
		uint32_t Sample[4];
	};

	FSRPassUniforms m_easu_buffer_data;
	FSRPassUniforms m_rcas_buffer_data;

	Buffer m_fsr_params_buffer;

	VkPipelineLayout      m_pipeline_layout       = VK_NULL_HANDLE;
	VkPipeline            m_pipeline_easu         = VK_NULL_HANDLE;
	VkPipeline            m_pipeline_rcas         = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_descriptor_set_layout = VK_NULL_HANDLE;

	// prev -(easu)-> intermediate image
	VkDescriptorSet m_easu_descriptor_set = VK_NULL_HANDLE;

	// intermediate image -(rcas)-> final image (that is, upsampled_image)
	VkDescriptorSet m_rcas_descriptor_set = VK_NULL_HANDLE;
};