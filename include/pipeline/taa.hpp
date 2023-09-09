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

	void resize();

	void draw(CommandBufferRecorder &recorder, const Scene &scene, const GBufferPass &gbuffer_pass, const DeferredPass &deferred);

	bool draw_ui();

  private:
	void create_resource();

	void update_descriptor();

	void destroy_resource();

  public:
	std::array<Texture, 2>     output_image;
	std::array<VkImageView, 2> output_view = {VK_NULL_HANDLE, VK_NULL_HANDLE};

	struct
	{
		VkDescriptorSetLayout          layout = VK_NULL_HANDLE;
		std::array<VkDescriptorSet, 2> sets   = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	} descriptor;

  private:
	const Context *m_context = nullptr;

	struct
	{
		glm::vec4 texel_size       = {};
		float     min_blend_factor = 0.3f;
	} m_push_constants;

	VkPipelineLayout               m_pipeline_layout       = VK_NULL_HANDLE;
	VkPipeline                     m_pipeline              = VK_NULL_HANDLE;
	VkDescriptorSetLayout          m_descriptor_set_layout = VK_NULL_HANDLE;
	std::array<VkDescriptorSet, 2> m_descriptor_sets       = {VK_NULL_HANDLE, VK_NULL_HANDLE};
};