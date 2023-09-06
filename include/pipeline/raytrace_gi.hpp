#pragma once

#include "context.hpp"
#include "gbuffer.hpp"
#include "scene.hpp"

struct RayTracedGI
{
  public:
	RayTracedGI(const Context &context, const Scene &scene, const GBufferPass &gbuffer_pass, RayTracedScale scale = RayTracedScale::Full_Res);

	~RayTracedGI();

	void init();

	void draw(CommandBufferRecorder &recorder, const Scene &scene, const GBufferPass &gbuffer_pass);

	bool draw_ui();

  public:
	// ray trace radiance
	Texture     radiance_image;
	VkImageView radiance_view = VK_NULL_HANDLE;

	// ray trace direction depth
	Texture     direction_depth_image;
	VkImageView direction_depth_view = VK_NULL_HANDLE;

	// probe grid irradiance image
	std::array<Texture, 2>     probe_grid_irradiance_image;
	std::array<VkImageView, 2> probe_grid_irradiance_view = {VK_NULL_HANDLE, VK_NULL_HANDLE};

	// probe grid depth image
	std::array<Texture, 2>     probe_grid_depth_image;
	std::array<VkImageView, 2> probe_grid_depth_view = {VK_NULL_HANDLE, VK_NULL_HANDLE};

	// sample probee grid
	Texture     sample_probe_grid_image;
	VkImageView sample_probe_grid_view = VK_NULL_HANDLE;

	Buffer ddgi_buffer;

	struct
	{
		VkDescriptorSetLayout          layout = VK_NULL_HANDLE;
		std::array<VkDescriptorSet, 2> sets   = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	} descriptor;

  private:
	const Context *m_context = nullptr;

	uint32_t m_width       = 0;
	uint32_t m_height      = 0;
	uint32_t m_gbuffer_mip = 0;

	glm::vec3 m_scene_min_extent = glm::vec3(0.f);
	glm::vec3 m_scene_max_extent = glm::vec3(0.f);

	struct
	{
		struct
		{
			bool    infinite_bounces          = true;
			float   infinite_bounce_intensity = 0.3f;
			int32_t rays_per_probe            = 256;
		} params;

		struct
		{
			glm::mat4 random_orientation;
			uint32_t  num_frames;
			uint32_t  infinite_bounces;
			float     gi_intensity;
		} push_constants;

		VkPipelineLayout               pipeline_layout       = VK_NULL_HANDLE;
		VkPipeline                     pipeline              = VK_NULL_HANDLE;
		VkDescriptorSetLayout          descriptor_set_layout = VK_NULL_HANDLE;
		std::array<VkDescriptorSet, 2> descriptor_sets       = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	} m_raytraced;
};