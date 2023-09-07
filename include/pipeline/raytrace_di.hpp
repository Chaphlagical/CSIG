#pragma once

#include "context.hpp"
#include "gbuffer.hpp"
#include "scene.hpp"

#define NUM_THREADS_X 8
#define NUM_THREADS_Y 8

struct RayTracedDI
{
  public:
	RayTracedDI(const Context &context, const Scene &scene, const GBufferPass &gbuffer_pass, RayTracedScale scale = RayTracedScale::Full_Res);

	~RayTracedDI();

	void init();

	void draw(CommandBufferRecorder &recorder, const Scene &scene, const GBufferPass &gbuffer_pass);

	bool draw_ui();

  public:
	// Output image
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

	Buffer temporal_reservoir_buffer;
	Buffer passthrough_reservoir_buffer;
	Buffer spatial_reservoir_buffer;
	Buffer denoise_tile_data_buffer;
	Buffer denoise_tile_dispatch_args_buffer;
	Buffer copy_tile_data_buffer;
	Buffer copy_tile_dispatch_args_buffer;

	struct
	{
		VkDescriptorSetLayout layout = VK_NULL_HANDLE;
		VkDescriptorSet       set    = VK_NULL_HANDLE;
	} descriptor;

  private:
	const Context *m_context = nullptr;

	bool m_spatial_reuse  = true;
	bool m_temporal_reuse = true;

	uint32_t m_width       = 0;
	uint32_t m_height      = 0;
	uint32_t m_gbuffer_mip = 0;

	struct
	{
		struct
		{
			struct
			{
				int32_t  gbuffer_mip     = 0;
				uint32_t temporal_reuse  = 1;
				int32_t  M               = 4;
				int32_t  clamp_threshold = 4;
			} push_constants;

			VkPipelineLayout      pipeline_layout       = VK_NULL_HANDLE;
			VkPipeline            pipeline              = VK_NULL_HANDLE;
			VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
			VkDescriptorSet       descriptor_set        = VK_NULL_HANDLE;
		} temporal;

		struct
		{
			struct
			{
				int32_t  gbuffer_mip   = 0;
				uint32_t spatial_reuse = 1;
				float    radius        = 10.f;
				int32_t  samples       = 5;
			} push_constants;

			VkPipelineLayout      pipeline_layout       = VK_NULL_HANDLE;
			VkPipeline            pipeline              = VK_NULL_HANDLE;
			VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
			VkDescriptorSet       descriptor_set        = VK_NULL_HANDLE;
		} spatial;

		struct
		{
			struct
			{
				int32_t gbuffer_mip = 0;
				float   normal_bias = 0.0001f;
			} push_constants;

			VkPipelineLayout      pipeline_layout       = VK_NULL_HANDLE;
			VkPipeline            pipeline              = VK_NULL_HANDLE;
			VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
			VkDescriptorSet       descriptor_set        = VK_NULL_HANDLE;
		} composite;
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