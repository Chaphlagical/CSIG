#pragma once

#include "context.hpp"
#include "pipeline/deferred.hpp"
#include "pipeline/path_tracing.hpp"
#include "pipeline/taa.hpp"

struct Bloom
{
  public:
	Bloom(const Context &context);

	~Bloom();

	void init();

	void resize();

	void draw(CommandBufferRecorder &recorder, const PathTracing &path_tracing);

	void draw(CommandBufferRecorder &recorder, const TAA &taa);

	bool draw_ui();

  private:
	void draw(CommandBufferRecorder &recorder, VkDescriptorSet input_set);

	void create_resource();

	void update_descriptor();

	void destroy_resource();

  public:
	Texture     mask_image;
	VkImageView mask_view = VK_NULL_HANDLE;

	Texture     output_image;
	VkImageView output_view = VK_NULL_HANDLE;

	std::array<Texture, 4>     level_image;
	std::array<VkImageView, 4> level_view{VK_NULL_HANDLE};

	std::array<Texture, 4>     blur_image;
	std::array<VkImageView, 4> blur_view{VK_NULL_HANDLE};

	VkSampler sampler = VK_NULL_HANDLE;

	struct
	{
		VkDescriptorSetLayout layout = VK_NULL_HANDLE;
		VkDescriptorSet       set    = VK_NULL_HANDLE;
	} descriptor;

  private:
	const Context *m_context = nullptr;

	struct
	{
		struct
		{
			float threshold = 0.75f;
		} push_constants;

		VkPipelineLayout      pipeline_layout   = VK_NULL_HANDLE;
		VkPipeline            pipeline          = VK_NULL_HANDLE;
		VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
		VkDescriptorSet       descriptor_set    = VK_NULL_HANDLE;
	} m_mask;

	struct
	{
		VkPipelineLayout               pipeline_layout   = VK_NULL_HANDLE;
		VkPipeline                     pipeline          = VK_NULL_HANDLE;
		VkDescriptorSetLayout          descriptor_layout = VK_NULL_HANDLE;
		std::array<VkDescriptorSet, 4> descriptor_sets{VK_NULL_HANDLE};
	} m_dowsample;

	struct
	{
		VkPipelineLayout               pipeline_layout   = VK_NULL_HANDLE;
		VkPipeline                     pipeline          = VK_NULL_HANDLE;
		VkDescriptorSetLayout          descriptor_layout = VK_NULL_HANDLE;
		std::array<VkDescriptorSet, 4> descriptor_sets{VK_NULL_HANDLE};
	} m_blur;

	struct
	{
		struct
		{
			float radius = 0.75f;
		} push_constants;

		VkPipelineLayout               pipeline_layout   = VK_NULL_HANDLE;
		VkPipeline                     pipeline          = VK_NULL_HANDLE;
		VkDescriptorSetLayout          descriptor_layout = VK_NULL_HANDLE;
		std::array<VkDescriptorSet, 3> descriptor_sets{VK_NULL_HANDLE};
	} m_upsample;

	struct
	{
		struct
		{
			float intensity = 1.f;
		} push_constants;

		VkPipelineLayout      pipeline_layout   = VK_NULL_HANDLE;
		VkPipeline            pipeline          = VK_NULL_HANDLE;
		VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
		VkDescriptorSet       descriptor_set    = VK_NULL_HANDLE;
	} m_blend;
};