#pragma once

#include "render/common.hpp"
#include "render/context.hpp"
#include "render/pipeline/gbuffer.hpp"
#include "render/scene.hpp"

struct RayTracedShadow
{
  public:
	RayTracedShadow(const Context &context, RayTracedScale scale = RayTracedScale::Half_Res);

	~RayTracedShadow();

	void init(VkCommandBuffer cmd_buffer);

	void update(const Scene &scene, const BlueNoise &blue_noise, const GBufferPass &gbuffer_pass);

	void draw(VkCommandBuffer cmd_buffer);

	bool draw_ui();

  public:
	// Raytraced image
	Texture     raytraced_image;
	VkImageView raytraced_image_view = VK_NULL_HANDLE;

  private:
	const Context *m_context = nullptr;

	uint32_t m_width       = 0;
	uint32_t m_height      = 0;
	uint32_t m_gbuffer_mip = 0;

	struct
	{
		struct
		{
			float   bias        = 0.03f;
			int32_t gbuffer_mip = 0;
		} push_constant;

		VkPipelineLayout      pipeline_layout       = VK_NULL_HANDLE;
		VkPipeline            pipeline              = VK_NULL_HANDLE;
		VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
		VkDescriptorSet       descriptor_sets[2];
	} m_raytraced;

	struct
	{
		struct
		{
		} temporal_accumulation;

		struct
		{
		} a_trous;
	} m_denoise;

	struct
	{
	} m_upsampling;
};