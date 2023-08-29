#pragma once

#include "context.hpp"
#include "path_tracing.hpp"

struct Tonemap
{
  public:
	Tonemap(const Context &context);

	~Tonemap();

	void draw(CommandBufferRecorder &recorder, const PathTracing &path_tracing);

	bool draw_ui();

  public:
	Texture     render_target;
	VkImageView render_target_view = VK_NULL_HANDLE;

  private:
	const Context *m_context = nullptr;

	struct
	{
		float brightness = 1.f;
		float contrast   = 1.f;
		float saturation = 1.f;
		float vignette   = 0.f;
		float avg_lum    = 1.f;
		float y_white    = 0.5f;
		float key        = 0.5f;
	} m_push_constant;

	VkPipelineLayout m_pipeline_layout = VK_NULL_HANDLE;
	VkPipeline       m_pipeline        = VK_NULL_HANDLE;

	struct
	{
		VkDescriptorSetLayout input_layout  = VK_NULL_HANDLE;
		VkDescriptorSetLayout output_layout = VK_NULL_HANDLE;
		VkDescriptorSet       output_set    = VK_NULL_HANDLE;
	}m_descriptor;
};