#pragma once

#include "render/common.hpp"
#include "render/context.hpp"
#include "render/scene.hpp"

class RayTracedAO
{
  public:
	RayTracedAO(const Context &context, RayTracedScale scale = RayTracedScale::Half_Res);

	~RayTracedAO();

	void init(VkCommandBuffer cmd_buffer);

	void draw(VkCommandBuffer cmd_buffer);

	void draw_ui();

	void update(const Scene &scene, const BlueNoise &blue_noise, VkImageView gbufferB, VkImageView depth_buffer);

  private:
	// Raytraced AO image
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
			float   ray_length  = 1.3f;
			float   bias        = 0.3f;
			int32_t gbuffer_mip = 0;
		} push_constant;

		VkPipelineLayout      pipeline_layout       = VK_NULL_HANDLE;
		VkPipeline            pipeline              = VK_NULL_HANDLE;
		VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
		VkDescriptorSet       descriptor_set        = VK_NULL_HANDLE;
	} m_raytraced;
};