#pragma once

#include "render/common.hpp"
#include "render/context.hpp"
#include "render/pipeline/gbuffer.hpp"
#include "render/scene.hpp"

class RayTracedAO
{
  public:
	RayTracedAO(const Context &context, RayTracedScale scale = RayTracedScale::Half_Res);

	~RayTracedAO();

	void init(VkCommandBuffer cmd_buffer);

	void update(const Scene &scene, const BlueNoise &blue_noise, const GBufferPass &gbuffer_pass);

	void draw(VkCommandBuffer cmd_buffer);

	bool draw_ui();

  public:
	// Raytraced AO image
	Texture     raytraced_image;
	VkImageView raytraced_image_view = VK_NULL_HANDLE;

	// AO image
	Texture     ao_image[2];
	VkImageView ao_image_view[2];

	// History length image
	Texture     history_length_image[2];
	VkImageView history_length_image_view[2];

	// Bilateral blur image
	Texture     bilateral_blur_image[2];
	VkImageView bilateral_blur_image_view[2];

	// Upsampling ao image
	Texture     upsampled_ao_image;
	VkImageView upsampled_ao_image_view = VK_NULL_HANDLE;

	// Denoise tile buffer
	Buffer denoise_tile_buffer;

	// Denoise tile dispatch argument buffer
	Buffer denoise_tile_dispatch_args_buffer;

  private:
	const Context *m_context = nullptr;

	uint32_t m_width       = 0;
	uint32_t m_height      = 0;
	uint32_t m_gbuffer_mip = 0;

	struct
	{
		struct
		{
			float   ray_length  = 0.3f;
			float   bias        = 0.03f;
			int32_t gbuffer_mip = 0;
		} push_constant;

		VkPipelineLayout      pipeline_layout       = VK_NULL_HANDLE;
		VkPipeline            pipeline              = VK_NULL_HANDLE;
		VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
		VkDescriptorSet       descriptor_sets[2]    = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	} m_raytraced;

	struct
	{
		struct
		{
			struct
			{
				float    alpha       = 0.2f;
				int32_t  gbuffer_mip = 0;
				uint32_t debug       = 0;
			} push_constant;

			VkPipelineLayout      pipeline_layout       = VK_NULL_HANDLE;
			VkPipeline            pipeline              = VK_NULL_HANDLE;
			VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
			VkDescriptorSet       descriptor_sets[2]    = {VK_NULL_HANDLE, VK_NULL_HANDLE};
		} temporal_accumulation;

		struct
		{
			struct
			{
				glm::vec4  z_buffer_params = glm::vec4(0.f);
				glm::ivec2 direction       = glm::ivec2(0);
				int32_t    radius          = 0;
				int32_t    gbuffer_mip     = 0;
			} push_constant;

			VkPipelineLayout      pipeline_layout       = VK_NULL_HANDLE;
			VkPipeline            pipeline              = VK_NULL_HANDLE;
			VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
			VkDescriptorSet       descriptor_sets[2][2];
		} bilateral_blur;
	} m_denoise;

	struct
	{
		struct
		{
			int32_t gbuffer_mip = 0;
			float   power       = 1.2f;
			uint32_t debug       = 0;
		} push_constant;

		VkPipelineLayout      pipeline_layout       = VK_NULL_HANDLE;
		VkPipeline            pipeline              = VK_NULL_HANDLE;
		VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
		VkDescriptorSet       descriptor_sets[2];
	} m_upsampling;
};