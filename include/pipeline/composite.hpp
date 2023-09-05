#pragma once

#include "context.hpp"
#include "pipeline/gbuffer.hpp"
#include "pipeline/path_tracing.hpp"
#include "pipeline/raytrace_ao.hpp"
#include "pipeline/tonemap.hpp"

struct CompositePass
{
	enum class GBufferOption
	{
		Albedo,
		Normal,
		Metallic,
		Roughness,
	};

  public:
	CompositePass(const Context &context, const Scene &scene, const GBufferPass &gbuffer, const RayTracedAO &ao);

	~CompositePass();

	void init();

	void draw(CommandBufferRecorder &recorder, const Scene &scene, const RayTracedAO &ao);

	void draw(CommandBufferRecorder &recorder, const Scene &scene, const Tonemap &tonemap);

	void draw(CommandBufferRecorder &recorder, const Scene &scene, const GBufferPass &gbuffer, GBufferOption option);

  private:
	void blit(CommandBufferRecorder &recorder);

  public:
	Texture     composite_image;
	VkImageView composite_view = VK_NULL_HANDLE;

  private:
	const Context *m_context = nullptr;

	VkDescriptorSetLayout m_descriptor_layout = VK_NULL_HANDLE;
	VkDescriptorSet       m_descriptor_set    = VK_NULL_HANDLE;

	struct
	{
		VkPipelineLayout pipeline_layout    = VK_NULL_HANDLE;
		VkPipeline       albedo_pipeline    = VK_NULL_HANDLE;
		VkPipeline       normal_pipeline    = VK_NULL_HANDLE;
		VkPipeline       metallic_pipeline  = VK_NULL_HANDLE;
		VkPipeline       roughness_pipeline = VK_NULL_HANDLE;
	} m_gbuffer;

	struct
	{
		VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
		VkPipeline       pipeline        = VK_NULL_HANDLE;
	} m_ao;
};