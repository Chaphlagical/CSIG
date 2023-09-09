#pragma once

#include "context.hpp"
#include "pipeline/fsr.hpp"
#include "pipeline/gbuffer.hpp"
#include "pipeline/path_tracing.hpp"
#include "pipeline/raytrace_ao.hpp"
#include "pipeline/raytrace_di.hpp"
#include "pipeline/raytrace_gi.hpp"
#include "pipeline/raytrace_reflection.hpp"
#include "pipeline/tonemap.hpp"

struct CompositePass
{
	enum class GBufferOption
	{
		Albedo,
		Normal,
		Metallic,
		Roughness,
		Position,
	};

  public:
	CompositePass(const Context             &context,
	              const Scene               &scene,
	              const GBufferPass         &gbuffer,
	              const RayTracedAO         &ao,
	              const RayTracedDI         &di,
	              const RayTracedGI         &gi,
	              const RayTracedReflection &reflection);

	~CompositePass();

	void init();

	void resize();

	void draw(CommandBufferRecorder     &recorder,
	          const Scene               &scene,
	          const GBufferPass         &gbuffer,
	          const RayTracedAO         &ao,
	          const RayTracedDI         &di,
	          const RayTracedGI         &gi,
	          const RayTracedReflection &reflection,
	          const FSR1Pass            &fsr);

	bool draw_ui();

  private:
	void create_resource();

	void update_descriptor();

	void destroy_resource();

	void blit(CommandBufferRecorder &recorder);

  public:
	Texture     composite_image;
	VkImageView composite_view = VK_NULL_HANDLE;

  private:
	const Context *m_context = nullptr;

	VkDescriptorSetLayout m_descriptor_layout = VK_NULL_HANDLE;
	VkDescriptorSet       m_descriptor_set    = VK_NULL_HANDLE;

	enum class Option : int32_t
	{
		Result,
		Albedo,
		Normal,
		Metallic,
		Roughness,
		Position,
		AO,
		Reflection,
		DI,
		GI
	} option = Option::Result;

	struct
	{
		VkPipelineLayout pipeline_layout    = VK_NULL_HANDLE;
		VkPipeline       albedo_pipeline    = VK_NULL_HANDLE;
		VkPipeline       normal_pipeline    = VK_NULL_HANDLE;
		VkPipeline       metallic_pipeline  = VK_NULL_HANDLE;
		VkPipeline       roughness_pipeline = VK_NULL_HANDLE;
		VkPipeline       position_pipeline  = VK_NULL_HANDLE;
	} m_gbuffer;

	struct
	{
		VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
		VkPipeline       pipeline        = VK_NULL_HANDLE;
	} m_ao;

	struct
	{
		VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
		VkPipeline       pipeline        = VK_NULL_HANDLE;
	} m_reflection;

	struct
	{
		VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
		VkPipeline       pipeline        = VK_NULL_HANDLE;
	} m_di;

	struct
	{
		VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
		VkPipeline       pipeline        = VK_NULL_HANDLE;
	} m_gi;
};