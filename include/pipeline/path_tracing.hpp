#pragma once

#include "context.hpp"
#include "gbuffer.hpp"

struct PathTracing
{
  public:
	PathTracing(const Context &context, const Scene &scene, const GBufferPass &gbuffer_pass);

	~PathTracing();

	void draw(CommandBufferRecorder &recorder, const Scene &scene, const GBufferPass &gbuffer_pass);

	bool draw_ui();

	void reset_frames();

  public:
	std::array<Texture, 2>     render_target;
	std::array<VkImageView, 2> render_target_view = {VK_NULL_HANDLE, VK_NULL_HANDLE};

  private:
	const Context *m_context = nullptr;

	struct
	{
		int32_t  max_depth   = 5;
		float    bias        = 0.0001f;
		uint32_t frame_count = 0;
	} m_push_constant;

	VkPipelineLayout               m_pipeline_layout       = VK_NULL_HANDLE;
	VkPipeline                     m_pipeline              = VK_NULL_HANDLE;
	VkDescriptorSetLayout          m_descriptor_set_layout = VK_NULL_HANDLE;
	std::array<VkDescriptorSet, 2> m_descriptor_sets       = {VK_NULL_HANDLE, VK_NULL_HANDLE};
};