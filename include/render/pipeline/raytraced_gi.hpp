#pragma once

#include "render/common.hpp"
#include "render/context.hpp"
#include "render/pipeline/gbuffer.hpp"
#include "render/scene.hpp"

struct RayTracedGI
{
  public:
	RayTracedGI(const Context &context, RayTracedScale scale = RayTracedScale::Full_Res);

	~RayTracedGI();

	void init(VkCommandBuffer cmd_buffer);

	void update(const Scene &scene, const BlueNoise &blue_noise, const GBufferPass &gbuffer_pass);

	void draw(VkCommandBuffer cmd_buffer);

	bool draw_ui();

  public:
	// ray trace radiance
	Texture     radiance_image;
	VkImageView radiance_view = VK_NULL_HANDLE;

	// ray trace direction depth
	Texture     direction_depth_image;
	VkImageView direction_depth_view = VK_NULL_HANDLE;

	// probe grid irradiance image
	Texture     probe_grid_irradiance_image[2];
	VkImageView probe_grid_irradiance_view[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};

	// probe grid depth image
	Texture     probe_grid_depth_image[2];
	VkImageView probe_grid_depth_view[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};

	// sample probee grid
	Texture     sample_probe_grid_image;
	VkImageView sample_probe_grid_view = VK_NULL_HANDLE;

	Buffer uniform_buffer;

  private:
	void create_resource();
	void destroy_resource();

  private:
	const Context *m_context = nullptr;

	uint32_t m_width       = 0;
	uint32_t m_height      = 0;
	uint32_t m_gbuffer_mip = 0;

	glm::vec3 m_scene_min_extent = glm::vec3(0.f);
	glm::vec3 m_scene_max_extent = glm::vec3(0.f);

	struct
	{
		// glm::ivec3 probe_count         = glm::ivec3(0);
		// float      max_distance        = 0.f;
		// glm::vec3  grid_start          = glm::vec3(0.f);
		// float      normal_bias         = 0.f;
		// glm::vec3  grid_step           = glm::vec3(0.f);
		// uint32_t   rays_per_probe      = 256;
		// uint32_t   irradiance_oct_size = 8;
	} m_ubo;

	struct
	{
		struct
		{
			bool     infinite_bounces          = true;
			float    infinite_bounce_intensity = 1.7f;
			uint32_t rays_per_probe            = 256;
		} params;

		VkPipelineLayout      pipeline_layout       = VK_NULL_HANDLE;
		VkPipeline            pipeline              = VK_NULL_HANDLE;
		VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
		VkDescriptorSet       descriptor_set        = VK_NULL_HANDLE;
	} m_raytraced;

	struct
	{
		struct
		{
			bool       visibility_test               = true;
			float      probe_distance                = 1.f;
			float      recursive_energy_preservation = 0.85f;
			uint32_t   irradiance_oct_size           = 8;
			uint32_t   depth_oct_size                = 16;
			glm::vec3  grid_start                    = glm::vec3(0.f);
			glm::uvec3 probe_count                   = glm::uvec3(0);
			float      hysteresis                    = 0.98f;
			float      depth_sharpness               = 50.f;
			float      max_distance                  = 4.f;
			float      normal_bias                   = 0.25f;
		} params;
	} m_probe_update;

	struct
	{
		struct
		{
			float gi_intensity = 1.f;
		} params;
	} m_probe_sample;
};