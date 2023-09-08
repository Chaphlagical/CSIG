#pragma once

#include "context.hpp"
#include "pipeline/deferred.hpp"
#include "pipeline/gbuffer.hpp"
#include "scene.hpp"

struct TAA
{
  public:
	TAA(const Context &context, const Scene &scene, const GBufferPass &gbuffer_pass, const DeferredPass &deferred);

	~TAA();

	void init();

	void draw(VkCommandBuffer cmd_buffer, const Scene &scene, const GBufferPass &gbuffer_pass, const DeferredPass &deferred);

	bool draw_ui();

  public:
	std::array<Texture, 2>     output_image;
	std::array<VkImageView, 2> output_view = {VK_NULL_HANDLE, VK_NULL_HANDLE};

  private:
	const Context *m_context = nullptr;

	float m_delta_time = 0.f;

	struct
	{
		glm::vec4 time_params   = {};
		glm::vec4 texel_size    = {};
		float    feed_back_min = 0.88f;
		float    feed_back_max = 0.97f;
		uint32_t sharpen       = 1;
	} m_push_constants;

	VkPipelineLayout               m_pipeline_layout       = VK_NULL_HANDLE;
	VkPipeline                     m_pipeline              = VK_NULL_HANDLE;
	VkDescriptorSetLayout          m_descriptor_set_layout = VK_NULL_HANDLE;
	std::array<VkDescriptorSet, 2> m_descriptor_sets       = {VK_NULL_HANDLE, VK_NULL_HANDLE};
};