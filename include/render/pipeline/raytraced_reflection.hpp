#pragma once

#include "render/common.hpp"
#include "render/context.hpp"
#include "render/pipeline/gbuffer.hpp"
#include "render/scene.hpp"

struct RayTracedReflection
{
  public:
	RayTracedReflection(const Context &context, const Scene &scene, const GBufferPass &gbuffer_pass, const BlueNoise &blue_noise, const LUT &lut, RayTracedScale scale = RayTracedScale::Full_Res);

	~RayTracedReflection();

	void init(VkCommandBuffer cmd_buffer);

	void update(const Scene &scene, const GBufferPass &gbuffer_pass, const BlueNoise &blue_noise, const LUT &lut);

	void draw(VkCommandBuffer cmd_buffer, const Scene &scene, const GBufferPass &gbuffer_pass, const BlueNoise &blue_noise, const LUT &lut);

	bool draw_ui();

  public:
	// Ray trace image
	Texture     raytraced_image;
	VkImageView raytraced_view = VK_NULL_HANDLE;

	// Reprojection output image
	Texture     reprojection_output_image[2];
	VkImageView reprojection_output_view[2];

	// Reprojection moment image
	Texture     reprojection_moment_image[2];
	VkImageView reprojection_moment_view[2];

	// Reprojection previous image
	Texture     reprojection_prev_image;
	VkImageView reprojection_prev_view = VK_NULL_HANDLE;

	// A-Trous image
	Texture     a_trous_image[2];
	VkImageView a_trous_view[2];

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
			int gbuffer_mip = 0;
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
			VkPipelineLayout      pipeline_layout       = VK_NULL_HANDLE;
			VkPipeline            pipeline              = VK_NULL_HANDLE;
			VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
			VkDescriptorSet       descriptor_sets[2]    = {VK_NULL_HANDLE, VK_NULL_HANDLE};
		} a_trous;
	} m_denoise;

	struct
	{
	} m_upsampling;

	/*struct
	{
	    struct
	    {
	    } push_constants;

	    struct
	    {
	        VkPipelineLayout      pipeline_layout       = VK_NULL_HANDLE;
	        VkPipeline            pipeline              = VK_NULL_HANDLE;
	        VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
	        VkDescriptorSet       descriptor_set        = VK_NULL_HANDLE;
	    }atrous;
	} m_denoise;

	struct
	{
	    struct
	    {
	    } push_constants;

	    VkPipelineLayout      pipeline_layout       = VK_NULL_HANDLE;
	    VkPipeline            pipeline              = VK_NULL_HANDLE;
	    VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
	    VkDescriptorSet       descriptor_set        = VK_NULL_HANDLE;
	} m_upsample;*/
};