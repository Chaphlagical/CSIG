#pragma once

#include "context.hpp"
#include "pipeline/tonemap.hpp"

struct FSR1Pass
{
  public:
	FSR1Pass(const Context &context, const Tonemap &tonemap);

	~FSR1Pass();

	void resize();

	void init();

	void draw(CommandBufferRecorder &recorder, const Tonemap &tonemap);

	bool draw_ui();

  public:
	void create_resource();

	void update_descriptor();

	void destroy_resource();

  public:
	Texture upsampled_image;
	Texture intermediate_image;

	VkImageView upsampled_image_view    = VK_NULL_HANDLE;
	VkImageView intermediate_image_view = VK_NULL_HANDLE;

	VkSampler sampler = VK_NULL_HANDLE;

	struct
	{
		VkDescriptorSetLayout layout = VK_NULL_HANDLE;
		VkDescriptorSet       set    = VK_NULL_HANDLE;
	} descriptor;

	enum class FSROption : int32_t
	{
		Disable,             // 1.f
		UltraQuality,        // 1.3f
		Quality,             // 1.5f
		Balanced,            // 1.7f
		Performance,         // 2.f
	} option = FSROption::UltraQuality;

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

	struct
	{
		VkPipelineLayout      pipeline_layout   = VK_NULL_HANDLE;
		VkPipeline            pipeline          = VK_NULL_HANDLE;
		VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
		VkDescriptorSet       descriptor_set    = VK_NULL_HANDLE;
	} m_easu;

	struct
	{
		VkPipelineLayout      pipeline_layout   = VK_NULL_HANDLE;
		VkPipeline            pipeline          = VK_NULL_HANDLE;
		VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
		VkDescriptorSet       descriptor_set    = VK_NULL_HANDLE;
	} m_rcas;
};