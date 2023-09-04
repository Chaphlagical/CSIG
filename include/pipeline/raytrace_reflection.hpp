#pragma once

#include "context.hpp"
#include "pipeline/gbuffer.hpp"
#include "scene.hpp"

struct RayTracedReflection
{
  public:
	RayTracedReflection(const Context &context, const Scene &scene, const GBufferPass &gbuffer_pass /*, const RayTracedGI &raytraced_gi*/, RayTracedScale scale = RayTracedScale::Half_Res);

	~RayTracedReflection();

	void init();

	void draw(CommandBufferRecorder &recorder, const Scene &scene, const GBufferPass &gbuffer_pass /*, const RayTracedGI &raytraced_gi*/);

	bool draw_ui();

  public:
	// Ray trace image
	Texture     raytraced_image;
	VkImageView raytraced_view = VK_NULL_HANDLE;

	// Reprojection output image
	std::array<Texture, 2>     reprojection_output_image;
	std::array<VkImageView, 2> reprojection_output_view = {VK_NULL_HANDLE, VK_NULL_HANDLE};

	// Reprojection moment image
	std::array<Texture, 2>     reprojection_moment_image;
	std::array<VkImageView, 2> reprojection_moment_view = {VK_NULL_HANDLE, VK_NULL_HANDLE};

	// A-Trous image
	std::array<Texture, 2>     a_trous_image;
	std::array<VkImageView, 2> a_trous_view = {VK_NULL_HANDLE, VK_NULL_HANDLE};

	// Upsampling image
	Texture     upsampling_image;
	VkImageView upsampling_view = VK_NULL_HANDLE;

	Buffer denoise_tile_data_buffer;
	Buffer denoise_tile_dispatch_args_buffer;
	Buffer copy_tile_data_buffer;
	Buffer copy_tile_dispatch_args_buffer;

  private:
	const Context *m_context = nullptr;

	uint32_t m_width       = 0;
	uint32_t m_height      = 0;
	uint32_t m_gbuffer_mip = 0;

	struct
	{
		struct
		{
			int32_t gbuffer_mip = 0;
		} push_constants;

		VkPipelineLayout      pipeline_layout       = VK_NULL_HANDLE;
		VkPipeline            pipeline              = VK_NULL_HANDLE;
		VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
		VkDescriptorSet       descriptor_set        = VK_NULL_HANDLE;
	} m_raytrace;

	struct
	{
		struct
		{
			uint64_t denoise_tile_data_addr          = 0;
			uint64_t denoise_tile_dispatch_args_addr = 0;
			uint64_t copy_tile_data_addr             = 0;
			uint64_t copy_tile_dispatch_args_addr    = 0;
			int32_t  gbuffer_mip                     = 0;
			float    alpha                           = 0.01f;
			float    moments_alpha                   = 0.2f;
		} push_constants;

		VkPipelineLayout               pipeline_layout       = VK_NULL_HANDLE;
		VkPipeline                     pipeline              = VK_NULL_HANDLE;
		VkDescriptorSetLayout          descriptor_set_layout = VK_NULL_HANDLE;
		std::array<VkDescriptorSet, 2> descriptor_sets       = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	} m_reprojection;

	struct
	{
		struct
		{
			struct
			{
				uint64_t copy_tile_data_addr = 0;
			} push_constants;
			VkPipelineLayout               pipeline_layout        = VK_NULL_HANDLE;
			VkPipeline                     pipeline               = VK_NULL_HANDLE;
			VkDescriptorSetLayout          descriptor_set_layout  = VK_NULL_HANDLE;
			std::array<VkDescriptorSet, 2> copy_reprojection_sets = {VK_NULL_HANDLE, VK_NULL_HANDLE};
			std::array<VkDescriptorSet, 2> copy_atrous_sets       = {VK_NULL_HANDLE, VK_NULL_HANDLE};
		} copy_tiles;
		struct
		{
			struct
			{
				uint64_t denoise_tile_data_addr = 0;
				int32_t  gbuffer_mip            = 0;
				float    phi_color              = 10.0f;
				float    phi_normal             = 32.0f;
				int32_t  radius                 = 1;
				int32_t  step_size              = 1;
				float    sigma_depth            = 1.0f;
			} push_constants;
			VkPipelineLayout               pipeline_layout          = VK_NULL_HANDLE;
			VkPipeline                     pipeline                 = VK_NULL_HANDLE;
			VkDescriptorSetLayout          descriptor_set_layout    = VK_NULL_HANDLE;
			std::array<VkDescriptorSet, 2> filter_reprojection_sets = {VK_NULL_HANDLE, VK_NULL_HANDLE};
			std::array<VkDescriptorSet, 2> filter_atrous_sets       = {VK_NULL_HANDLE, VK_NULL_HANDLE};
		} a_trous;
	} m_denoise;

	struct
	{
		struct
		{
			int32_t gbuffer_mip = 0;
		} push_constants;
		VkPipelineLayout      pipeline_layout       = VK_NULL_HANDLE;
		VkPipeline            pipeline              = VK_NULL_HANDLE;
		VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
		VkDescriptorSet       descriptor_set        = VK_NULL_HANDLE;
	} m_upsampling;
};