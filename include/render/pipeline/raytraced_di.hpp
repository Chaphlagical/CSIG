#pragma once

#include "render/common.hpp"
#include "render/context.hpp"
#include "render/pipeline/gbuffer.hpp"
#include "render/scene.hpp"

struct RayTracedDI
{
  public:
	RayTracedDI(const Context &context, const Scene &scene, const GBufferPass &gbuffer_pass, RayTracedScale scale = RayTracedScale::Full_Res);

	~RayTracedDI();

	void init(VkCommandBuffer cmd_buffer);

	void update(const Scene &scene, const BlueNoise &blue_noise, const GBufferPass &gbuffer_pass);

	void draw(VkCommandBuffer cmd_buffer, const Scene &scene, const GBufferPass &gbuffer_pass);

	bool draw_ui();

  public:
	Buffer temporal_reservoir_buffer;
	Buffer passthrough_reservoir_buffer;
	Buffer spatial_reservoir_buffer;
	Buffer denoise_tile_data_buffer;
	Buffer denoise_tile_dispatch_args_buffer;
	Buffer copy_tile_data_buffer;
	Buffer copy_tile_dispatch_args_buffer;

	// Output image
	Texture     output_image;
	VkImageView output_view = VK_NULL_HANDLE;

	// Reprojection output image
	Texture     reprojection_output_image[2];
	VkImageView reprojection_output_view[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};

	// Reprojection moment image
	Texture     reprojection_moment_image[2];
	VkImageView reprojection_moment_view[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};

	// A-Trous image
	Texture     a_trous_image[2];
	VkImageView a_trous_view[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};

	// Upsampling image
	Texture     upsampling_image;
	VkImageView upsampling_view = VK_NULL_HANDLE;

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
			uint64_t temporal_reservoir_addr    = 0;
			uint64_t passthrough_reservoir_addr = 0;
			uint32_t temporal_reuse             = 0;
			int32_t  M                          = 4;
			int32_t  clamp_threshold            = 4;
		} push_constants;

		VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
		VkPipeline       pipeline        = VK_NULL_HANDLE;
	} m_temporal_pass;

	struct
	{
		struct
		{
			uint64_t passthrough_reservoir_addr = 0;
			uint64_t spatial_reservoir_addr     = 0;
			uint32_t spatial_reuse              = 0;
			float    radius                     = 10.f;
			int32_t  samples                    = 5;
		} push_constants;

		VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
		VkPipeline       pipeline        = VK_NULL_HANDLE;
	} m_spatial_pass;

	struct
	{
		struct
		{
			uint64_t passthrough_reservoir_addr = 0;
			uint64_t temporal_reservoir_addr    = 0;
			uint64_t spatial_reservoir_addr     = 0;
			float    normal_bias                = 0.0001f;
		} push_constants;

		VkPipelineLayout      pipeline_layout       = VK_NULL_HANDLE;
		VkPipeline            pipeline              = VK_NULL_HANDLE;
		VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
		VkDescriptorSet       descriptor_set        = VK_NULL_HANDLE;
	} m_composite_pass;

	struct
	{
		struct
		{
			uint64_t denoise_tile_data_addr          = 0;
			uint64_t denoise_tile_dispatch_args_addr = 0;
			uint64_t copy_tile_data_addr             = 0;
			uint64_t copy_tile_dispatch_args_addr    = 0;
			int      gbuffer_mip                     = 0;
			float    alpha                           = 0.01f;
			float    moments_alpha                   = 0.2f;
		} push_constants;

		VkPipelineLayout      pipeline_layout       = VK_NULL_HANDLE;
		VkPipeline            pipeline              = VK_NULL_HANDLE;
		VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
		VkDescriptorSet       descriptor_sets[2]    = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	} m_reprojection;

	struct
	{
		struct
		{
			struct
			{
				uint64_t copy_tile_data_addr = 0;
			} push_constants;
			VkPipelineLayout      pipeline_layout           = VK_NULL_HANDLE;
			VkPipeline            pipeline                  = VK_NULL_HANDLE;
			VkDescriptorSetLayout descriptor_set_layout     = VK_NULL_HANDLE;
			VkDescriptorSet       copy_reprojection_sets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
			VkDescriptorSet       copy_atrous_sets[2]       = {VK_NULL_HANDLE, VK_NULL_HANDLE};
		} copy_tiles;
		struct
		{
			struct
			{
				uint64_t denoise_tile_data_addr = 0;
				int      gbuffer_mip            = 0;
				float    phi_color              = 10.0f;
				float    phi_normal             = 32.0f;
				int      radius                 = 1;
				int      step_size              = 1;
				float    sigma_depth            = 1.0f;
			} push_constants;
			VkPipelineLayout      pipeline_layout             = VK_NULL_HANDLE;
			VkPipeline            pipeline                    = VK_NULL_HANDLE;
			VkDescriptorSetLayout descriptor_set_layout       = VK_NULL_HANDLE;
			VkDescriptorSet       filter_reprojection_sets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
			VkDescriptorSet       filter_atrous_sets[2]       = {VK_NULL_HANDLE, VK_NULL_HANDLE};
		} a_trous;
	} m_denoise;

	struct
	{
		struct
		{
			int gbuffer_mip = 0;
		} push_constants;
		VkPipelineLayout      pipeline_layout       = VK_NULL_HANDLE;
		VkPipeline            pipeline              = VK_NULL_HANDLE;
		VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
		VkDescriptorSet       descriptor_set        = VK_NULL_HANDLE;
	} m_upsampling;
};