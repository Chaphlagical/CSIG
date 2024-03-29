#pragma once

#include "context.hpp"
#include "pipeline/gbuffer.hpp"
#include "scene.hpp"

#include <random>

struct RayTracedGI
{
  public:
	RayTracedGI(const Context &context, const Scene &scene, const GBufferPass &gbuffer_pass, RayTracedScale scale = RayTracedScale::Full_Res);

	~RayTracedGI();

	void init();

	void resize();

	void update(const Scene &scene);

	void draw(CommandBufferRecorder &recorder, const Scene &scene, const GBufferPass &gbuffer_pass);

	void draw_probe(CommandBufferRecorder &recorder, const VkImageView &render_target, const VkImageView &depth_buffer, const Scene &scene) const;

	bool draw_ui();

  private:
	void create_resource();

	void update_descriptor();

	void destroy_resource();

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

	// sample probe grid
	Texture     sample_probe_grid_image;
	VkImageView sample_probe_grid_view = VK_NULL_HANDLE;

	Buffer uniform_buffer;

	struct
	{
		VkDescriptorSetLayout layout = VK_NULL_HANDLE;
		VkDescriptorSet       set    = VK_NULL_HANDLE;
	} descriptor;

	struct
	{
		VkDescriptorSetLayout          layout = VK_NULL_HANDLE;
		std::array<VkDescriptorSet, 2> sets   = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	} ddgi_descriptor;

  private:
	const Context *m_context = nullptr;

	RayTracedScale m_scale       = RayTracedScale::Full_Res;
	uint32_t       m_width       = 0;
	uint32_t       m_height      = 0;
	uint32_t       m_gbuffer_mip = 0;

	glm::vec3 m_scene_min_extent = glm::vec3(0.f);
	glm::vec3 m_scene_max_extent = glm::vec3(0.f);

	bool m_init = false;

	uint32_t m_frame_count = 0;

	std::mt19937                          m_random_generator;
	std::uniform_real_distribution<float> m_random_distrib;

	struct UBO
	{
		glm::vec3  grid_start;
		float      max_distance;
		glm::vec3  grid_step;
		float      depth_sharpness;
		glm::ivec3 probe_count;
		float      hysteresis;
		float      normal_bias;
		float      energy_preservation;
		uint32_t   rays_per_probe;
		uint32_t   visibility_test;
		uint32_t   irradiance_probe_side_length;
		uint32_t   irradiance_texture_width;
		uint32_t   irradiance_texture_height;
		uint32_t   depth_probe_side_length;
		uint32_t   depth_texture_width;
		uint32_t   depth_texture_height;
	};

	struct
	{
		struct
		{
			bool    infinite_bounces          = true;
			float   infinite_bounce_intensity = 1.7f;
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

	struct
	{
		struct
		{
			bool       visibility_test               = true;
			float      probe_distance                = 1.0f;
			float      recursive_energy_preservation = 0.85f;
			uint32_t   irradiance_oct_size           = 8;
			uint32_t   depth_oct_size                = 16;
			uint32_t   irradiance_width              = 0;
			uint32_t   irradiance_height             = 0;
			uint32_t   depth_width                   = 0;
			uint32_t   depth_height                  = 0;
			glm::vec3  grid_start                    = glm::vec3(0.f);
			glm::vec3  grid_offset                   = glm::vec3(0.f);
			glm::uvec3 probe_count                   = glm::uvec3(0);
			float      hysteresis                    = 0.98f;
			float      depth_sharpness               = 50.f;
			float      max_distance                  = 4.f;
			float      normal_bias                   = 0.25f;
		} params;

		struct
		{
			struct
			{
				uint32_t frame_count;
			} push_constants;

			VkPipelineLayout               pipeline_layout       = VK_NULL_HANDLE;
			VkPipeline                     irradiance_pipeline   = VK_NULL_HANDLE;
			VkPipeline                     depth_pipeline        = VK_NULL_HANDLE;
			VkDescriptorSetLayout          descriptor_set_layout = VK_NULL_HANDLE;
			std::array<VkDescriptorSet, 2> descriptor_sets       = {VK_NULL_HANDLE, VK_NULL_HANDLE};
		} update_probe;

		struct
		{
			VkPipelineLayout               pipeline_layout       = VK_NULL_HANDLE;
			VkPipeline                     irradiance_pipeline   = VK_NULL_HANDLE;
			VkPipeline                     depth_pipeline        = VK_NULL_HANDLE;
			VkDescriptorSetLayout          descriptor_set_layout = VK_NULL_HANDLE;
			std::array<VkDescriptorSet, 2> descriptor_sets       = {VK_NULL_HANDLE, VK_NULL_HANDLE};
		} update_border;
	} m_probe_update;

	struct
	{
		struct
		{
			float gi_intensity = 1.f;
		} params;

		struct
		{
			int32_t gbuffer_mip  = 0;
			float   gi_intensity = 1.f;
		} push_constants;

		VkPipelineLayout               pipeline_layout       = VK_NULL_HANDLE;
		VkPipeline                     pipeline              = VK_NULL_HANDLE;
		VkDescriptorSetLayout          descriptor_set_layout = VK_NULL_HANDLE;
		std::array<VkDescriptorSet, 2> descriptor_sets       = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	} m_probe_sample;

	struct
	{
		struct
		{
			float scale = 2.f;
		} push_constants;

		Buffer vertex_buffer;
		Buffer index_buffer;

		uint32_t vertex_count = 0;
		uint32_t index_count  = 0;

		VkPipelineLayout               pipeline_layout       = VK_NULL_HANDLE;
		VkPipeline                     pipeline              = VK_NULL_HANDLE;
		VkDescriptorSetLayout          descriptor_set_layout = VK_NULL_HANDLE;
		std::array<VkDescriptorSet, 2> descriptor_sets       = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	} m_probe_visualize;
};