#pragma once

#include "context.hpp"
#include "gbuffer.hpp"
#include "scene.hpp"

struct RayTracedAO
{
  public:
	RayTracedAO(const Context &context, const Scene &scene, const GBufferPass &gbuffer_pass, RayTracedScale scale = RayTracedScale::Full_Res);

	~RayTracedAO();

	void init();

	void draw(CommandBufferRecorder &recorder, const Scene &scene, const GBufferPass &gbuffer_pass);

	bool draw_ui();

  public:
	// Raytraced AO image
	Texture     raytraced_image;
	VkImageView raytraced_image_view = VK_NULL_HANDLE;

	// AO image
	std::array<Texture, 2>     ao_image;
	std::array<VkImageView, 2> ao_image_view = {VK_NULL_HANDLE, VK_NULL_HANDLE};

	// History length image
	std::array<Texture, 2>     history_length_image;
	std::array<VkImageView, 2> history_length_image_view = {VK_NULL_HANDLE, VK_NULL_HANDLE};

	// Bilateral blur image
	std::array<Texture, 2>     bilateral_blur_image;
	std::array<VkImageView, 2> bilateral_blur_image_view = {VK_NULL_HANDLE, VK_NULL_HANDLE};

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
		VkDescriptorSet       descriptor_set;
	} m_raytraced;

	struct
	{
		struct
		{
			float    alpha       = 0.2f;
			int32_t  gbuffer_mip = 0;
		} push_constant;

		VkPipelineLayout               pipeline_layout       = VK_NULL_HANDLE;
		VkPipeline                     pipeline              = VK_NULL_HANDLE;
		VkDescriptorSetLayout          descriptor_set_layout = VK_NULL_HANDLE;
		std::array<VkDescriptorSet, 2> descriptor_sets       = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	} m_temporal_accumulation;

	struct
	{
		struct
		{
			glm::vec4  z_buffer_params = glm::vec4(0.f);
			glm::ivec2 direction       = glm::ivec2(0);
			int32_t    radius          = 3;
			int32_t    gbuffer_mip     = 0;
		} push_constant;

		VkPipelineLayout                              pipeline_layout       = VK_NULL_HANDLE;
		VkPipeline                                    pipeline              = VK_NULL_HANDLE;
		VkDescriptorSetLayout                         descriptor_set_layout = VK_NULL_HANDLE;
		std::array<std::array<VkDescriptorSet, 2>, 2> descriptor_sets;
	} m_bilateral_blur;

	struct
	{
		struct
		{
			int32_t  gbuffer_mip = 0;
			float    power       = 1.2f;
		} push_constant;

		VkPipelineLayout      pipeline_layout       = VK_NULL_HANDLE;
		VkPipeline            pipeline              = VK_NULL_HANDLE;
		VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
		VkDescriptorSet       descriptor_set        = VK_NULL_HANDLE;
	} m_upsampling;
};