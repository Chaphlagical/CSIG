#pragma once

#include "context.hpp"
#include "pipeline/gbuffer.hpp"
#include "pipeline/raytrace_ao.hpp"
#include "pipeline/raytrace_di.hpp"
#include "pipeline/raytrace_gi.hpp"
#include "pipeline/raytrace_reflection.hpp"

struct DeferredPass
{
  public:
	DeferredPass(const Context             &context,
	             const Scene               &scene,
	             const GBufferPass         &gbuffer,
	             const RayTracedAO         &ao,
	             const RayTracedDI         &di,
	             const RayTracedGI         &gi,
	             const RayTracedReflection &reflection);

	~DeferredPass();

	void init();

	void draw(CommandBufferRecorder     &recorder,
	          const Scene               &scene,
	          const GBufferPass         &gbuffer,
	          const RayTracedAO         &ao,
	          const RayTracedDI         &di,
	          const RayTracedGI         &gi,
	          const RayTracedReflection &reflection);

	bool draw_ui();

  public:
	Texture     deferred_image;
	VkImageView deferred_view = VK_NULL_HANDLE;

	struct
	{
		VkDescriptorSetLayout layout = VK_NULL_HANDLE;
		VkDescriptorSet       set    = VK_NULL_HANDLE;
	} descriptor;

  private:
	const Context *m_context = nullptr;

	struct
	{
		uint32_t enable_ao                  = 1;
		uint32_t enable_reflection          = 1;
		uint32_t enable_gi                  = 1;
		float    indirect_specular_strength = 1.f;
	} m_push_constant;

	VkPipelineLayout      m_pipeline_layout   = VK_NULL_HANDLE;
	VkPipeline            m_pipeline          = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_descriptor_layout = VK_NULL_HANDLE;
	VkDescriptorSet       m_descriptor_set    = VK_NULL_HANDLE;
};