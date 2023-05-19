#pragma once

#include "render/common.hpp"
#include "render/context.hpp"
#include "render/pipeline/gbuffer.hpp"
#include "render/scene.hpp"

#include <glm/glm.hpp>

struct TAA
{
  public:
	TAA(const Context &context, const Scene &scene, const GBufferPass &gbuffer_pass);

	~TAA();

	void init(VkCommandBuffer cmd_buffer);

	void update(const Scene &scene, const GBufferPass &gbuffer_pass, VkImageView result);

	void draw(VkCommandBuffer cmd_buffer, const Scene &scene, const GBufferPass &gbuffer_pass);

	bool draw_ui();

	void set_pathtracing(bool enable);

  public:
	Texture     output_image[2];
	VkImageView output_view[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};

  private:
	const Context *m_context = nullptr;

	struct
	{
		float    feed_back_min = 0.88f;
		float    feed_back_max = 0.97f;
		uint32_t sharpen       = 1;
	} m_push_constants;

	bool m_is_pathtracing = false;

	VkPipelineLayout      m_pipeline_layout       = VK_NULL_HANDLE;
	VkPipeline            m_pipeline              = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_descriptor_set_layout = VK_NULL_HANDLE;
	VkDescriptorSet       m_descriptor_sets[2]    = {VK_NULL_HANDLE, VK_NULL_HANDLE};
};