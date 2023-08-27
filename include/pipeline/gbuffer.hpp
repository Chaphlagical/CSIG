#pragma once

#include "context.hpp"
#include "scene.hpp"

class GBufferPass
{
  public:
	GBufferPass(const Context &context, const Scene &scene);

	~GBufferPass();

	void init(CommandBufferRecorder &recorder);

	void draw(CommandBufferRecorder &recorder, const Scene &scene);

  public:
	struct
	{
		VkDescriptorSetLayout layout = VK_NULL_HANDLE;
		VkDescriptorSet       sets[2];
	} descriptor;

  private:
	// RGB: Albedo, A: Metallic
	// RGBA8
	Texture gbufferA[2];

	// RG: Normal, BA: Motion Vector
	// RGBA16
	Texture gbufferB[2];

	// R: Roughness, G: Curvature, B: Instance ID, A: Linear Z
	// RGBA16
	Texture gbufferC[2];

	// Depth stencil buffer
	Texture depth_buffer[2];

	VkImageView gbufferA_view[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};

	VkImageView gbufferB_view[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};

	VkImageView gbufferC_view[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};

	VkImageView depth_buffer_view[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};

  private:
	const Context *m_context = nullptr;

	uint32_t m_width;
	uint32_t m_height;
	uint32_t m_mip_level;

	VkPipelineLayout      m_pipeline_layout       = VK_NULL_HANDLE;
	VkPipeline            m_pipeline              = VK_NULL_HANDLE;
};