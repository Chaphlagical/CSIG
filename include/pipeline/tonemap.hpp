#pragma once

#include "context.hpp"
#include "pipeline/bloom.hpp"
#include "pipeline/deferred.hpp"
#include "pipeline/path_tracing.hpp"
#include "pipeline/taa.hpp"

struct Tonemap
{
  public:
	Tonemap(const Context &context);

	~Tonemap();

	void init();

	void resize();

	void draw(CommandBufferRecorder &recorder, const PathTracing &path_tracing);

	void draw(CommandBufferRecorder &recorder, const DeferredPass &deferred);

	void draw(CommandBufferRecorder &recorder, const TAA &taa);

	void draw(CommandBufferRecorder &recorder, const Bloom &bloom);

	bool draw_ui();

  private:
	void create_resource();

	void update_descriptor();

	void destroy_resource();

  public:
	Texture     render_target;
	VkImageView render_target_view = VK_NULL_HANDLE;

	struct
	{
		VkDescriptorSetLayout layout = VK_NULL_HANDLE;
		VkDescriptorSet       set    = VK_NULL_HANDLE;
	} descriptor;

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

	struct
	{
		VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
		VkPipeline       pipeline        = VK_NULL_HANDLE;
	} m_average_lum;

	VkPipelineLayout m_pipeline_layout = VK_NULL_HANDLE;
	VkPipeline       m_pipeline        = VK_NULL_HANDLE;

	struct
	{
		VkDescriptorSetLayout input_layout  = VK_NULL_HANDLE;
		VkDescriptorSetLayout output_layout = VK_NULL_HANDLE;
		VkDescriptorSet       output_set    = VK_NULL_HANDLE;
	} m_descriptor;
};