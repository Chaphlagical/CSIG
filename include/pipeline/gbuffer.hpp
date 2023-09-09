#pragma once

#include "context.hpp"
#include "scene.hpp"

class GBufferPass
{
  public:
	GBufferPass(const Context &context, const Scene &scene);

	~GBufferPass();

	void init();

	void resize();

	void draw(CommandBufferRecorder &recorder, const Scene &scene);

  private:
	void create_resource();

	void update_descriptor();

	void destroy_resource();

  public:
	struct
	{
		VkDescriptorSetLayout          layout = VK_NULL_HANDLE;
		std::array<VkDescriptorSet, 2> sets;
	} descriptor;

	// RGB: Albedo, A: Metallic
	// RGBA8
	std::array<Texture, 2> gbufferA;

	// RG: Normal, BA: Motion Vector
	// RGBA16
	std::array<Texture, 2> gbufferB;

	// R: Roughness, G: Curvature, B: Instance ID, A: Linear Z
	// RGBA16
	std::array<Texture, 2> gbufferC;

	// Depth stencil buffer
	std::array<Texture, 2> depth_buffer;

	std::array<VkImageView, 2> gbufferA_view     = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	std::array<VkImageView, 2> gbufferB_view     = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	std::array<VkImageView, 2> gbufferC_view     = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	std::array<VkImageView, 2> depth_buffer_view = {VK_NULL_HANDLE, VK_NULL_HANDLE};

  private:
	const Context *m_context = nullptr;

	uint32_t m_width;
	uint32_t m_height;
	uint32_t m_mip_level;

	VkPipelineLayout m_pipeline_layout = VK_NULL_HANDLE;
	VkPipeline       m_pipeline        = VK_NULL_HANDLE;
};