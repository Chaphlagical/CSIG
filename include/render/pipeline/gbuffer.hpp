#pragma once

#include "render/context.hpp"
#include "render/scene.hpp"

class GBufferPass
{
  public:
	GBufferPass(const Context &context);

	~GBufferPass();

	void init(VkCommandBuffer cmd_buffer);

	void update(const Scene &scene);

	void draw(VkCommandBuffer cmd_buffer, const Scene &scene);

  public:
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

	VkImageView gbufferA_view[2];
	VkImageView gbufferB_view[2];
	VkImageView gbufferC_view[2];
	VkImageView depth_buffer_view[2];

	uint32_t width     = 0;
	uint32_t height    = 0;
	uint32_t mip_level = 0;

  public:
	struct
	{
		VkDescriptorSetLayout layout = VK_NULL_HANDLE;
		VkDescriptorSet       sets[2];
	} descriptor;

  private:
	const Context *m_context = nullptr;

	VkPipelineLayout      m_pipeline_layout       = VK_NULL_HANDLE;
	VkPipeline            m_pipeline              = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_descriptor_set_layout = VK_NULL_HANDLE;
	VkDescriptorSet       m_descriptor_set        = VK_NULL_HANDLE;

	VkRenderingAttachmentInfo m_gbufferA_attachment_info[2]           = {};
	VkRenderingAttachmentInfo m_gbufferB_attachment_info[2]           = {};
	VkRenderingAttachmentInfo m_gbufferC_attachment_info[2]           = {};
	VkRenderingAttachmentInfo m_depth_stencil_view_attachment_info[2] = {};
};