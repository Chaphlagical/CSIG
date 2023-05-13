#pragma once

#include "render/common.hpp"
#include "render/context.hpp"
#include "render/pipeline/gbuffer.hpp"
#include "render/scene.hpp"

class PathTracing
{
  public:
	PathTracing(const Context &context, const Scene &scene, const GBufferPass &gbuffer_pass);

	~PathTracing();

	void init(VkCommandBuffer cmd_buffer);

	void update(const Scene &scene, const BlueNoise &blue_noise, const GBufferPass &gbuffer_pass);

	void draw(VkCommandBuffer cmd_buffer, const Scene &scene, const GBufferPass &gbuffer_pass);

	bool draw_ui();

	void reset_frames();

  public:
	Texture     path_tracing_image[2];
	VkImageView path_tracing_image_view[2];

  private:
	const Context *m_context = nullptr;

	struct
	{
		int32_t  max_depth   = 5;
		float    bias        = 0.0001f;
		uint32_t frame_count = 0;
	} m_push_constant;

	VkPipelineLayout      m_pipeline_layout       = VK_NULL_HANDLE;
	VkPipeline            m_pipeline              = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_descriptor_set_layout = VK_NULL_HANDLE;
	VkDescriptorSet       m_descriptor_sets[2]    = {VK_NULL_HANDLE, VK_NULL_HANDLE};
};