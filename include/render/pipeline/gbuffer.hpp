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
	Texture gbufferA;

	// RG: Normal, BA: Motion Vector
	// RGBA16
	Texture gbufferB;

	// R: Roughness, G: Curvature, B: Instance ID, A: Linear Z
	// RGBA16
	Texture gbufferC;

	// Depth stencil buffer
	Texture depth_buffer;

	VkImageView gbufferA_view      = VK_NULL_HANDLE;
	VkImageView gbufferB_view      = VK_NULL_HANDLE;
	VkImageView gbufferC_view      = VK_NULL_HANDLE;
	VkImageView depth_stencil_view = VK_NULL_HANDLE;

	uint32_t width     = 0;
	uint32_t height    = 0;
	uint32_t mip_level = 0;

  private:
	const Context *m_context = nullptr;

	VkPipelineLayout      m_pipeline_layout       = VK_NULL_HANDLE;
	VkPipeline            m_pipeline              = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_descriptor_set_layout = VK_NULL_HANDLE;
	VkDescriptorSet       m_descriptor_set        = VK_NULL_HANDLE;

	VkRenderingAttachmentInfo m_gbufferA_attachment_info           = {};
	VkRenderingAttachmentInfo m_gbufferB_attachment_info           = {};
	VkRenderingAttachmentInfo m_gbufferC_attachment_info           = {};
	VkRenderingAttachmentInfo m_depth_stencil_view_attachment_info = {};
};