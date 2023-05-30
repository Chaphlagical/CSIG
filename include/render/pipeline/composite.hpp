#pragma once

#include "render/common.hpp"
#include "render/context.hpp"
#include "render/pipeline/gbuffer.hpp"
#include "render/scene.hpp"

#include <glm/glm.hpp>

struct Composite
{
  public:
	Composite(const Context &context, const LUT &lut, const Scene &scene, const GBufferPass &gbuffer_pass);

	~Composite();

	void init(VkCommandBuffer cmd_buffer);

	void update(const Scene &scene, VkImageView direct, VkImageView reflection, VkImageView ao, VkImageView gi);

	void draw(VkCommandBuffer cmd_buffer, const Scene &scene, const GBufferPass &gbuffer_pass, const LUT& lut);

	bool draw_ui();

  public:
	Texture     output_image;
	VkImageView output_view = VK_NULL_HANDLE;

  private:
	const Context *m_context = nullptr;

	struct
	{
		int32_t display_mode = 0;	// 0 - composite result, 1 - AO, 2 - GI, 3 - Reflection
	} m_push_constants;

	VkPipelineLayout      m_pipeline_layout       = VK_NULL_HANDLE;
	VkPipeline            m_pipeline              = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_descriptor_set_layout = VK_NULL_HANDLE;
	VkDescriptorSet       m_descriptor_set        = VK_NULL_HANDLE;
};