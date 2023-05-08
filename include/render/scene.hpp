#pragma once

#include "render/context.hpp"

#include <glm/glm.hpp>

#include <string>
#include <vector>

struct Context;

struct GlobalBuffer
{
	glm::mat4 view_inv;
	glm::mat4 projection_inv;
	glm::mat4 view_projection_inv;
	glm::mat4 view_projection;
	glm::mat4 prev_view_projection;
	glm::vec4 cam_pos;        // xyz - position, w - num_frames
	glm::vec4 jitter;
};

struct Vertex
{
	glm::vec4 position = glm::vec4(0.f);        // xyz - position, w - texcoord u
	glm::vec4 normal   = glm::vec4(0.f);        // xyz - normal, w - texcoord v
};

struct Instance
{
	glm::mat4 transform;
	glm::mat4 transform_inv;

	uint32_t vertices_offset = 0;
	uint32_t vertices_count  = 0;
	uint32_t indices_offset  = 0;
	uint32_t indices_count   = 0;

	alignas(16) uint32_t mesh = ~0u;
	uint32_t material         = ~0u;
	float    area             = 0.f;
};

struct Emitter
{
	glm::mat4 transform   = glm::mat4(1.f);
	glm::vec3 intensity   = glm::vec3(1.f);
	uint32_t  instance_id = ~0u;
};

struct Material
{
	uint32_t  alpha_mode                 = 0;        // 0 - opaque, 1 - mask, 2 - blend
	uint32_t  double_sided               = false;
	float     cutoff                     = 0.f;
	float     metallic_factor            = 0.f;
	float     roughness_factor           = 0.f;
	float     transmission_factor        = 0.f;
	float     clearcoat_factor           = 0.f;
	float     clearcoat_roughness_factor = 0.f;
	glm::vec4 base_color                 = glm::vec4(1.f);
	glm::vec3 emissive_factor            = glm::vec3(1.f);
	int32_t   base_color_texture         = -1;
	alignas(16) int32_t normal_texture   = -1;
	int32_t metallic_roughness_texture   = -1;
};

struct Envmap
{
	Texture texture;
	Texture irradiance_sh;
	Texture prefilter_map;

	VkImageView texture_view       = VK_NULL_HANDLE;
	VkImageView irradiance_sh_view = VK_NULL_HANDLE;
	VkImageView prefilter_map_view = VK_NULL_HANDLE;
};

struct Scene
{
	AccelerationStructure tlas;

	std::vector<AccelerationStructure> blas;

	Buffer instance_buffer;
	Buffer emitter_buffer;
	Buffer material_buffer;
	Buffer vertex_buffer;
	Buffer index_buffer;
	Buffer indirect_draw_buffer;
	Buffer global_buffer;
	Buffer scene_buffer;

	std::vector<Texture>     textures;
	std::vector<VkImageView> texture_views;

	Envmap envmap;

	VkSampler linear_sampler  = VK_NULL_HANDLE;
	VkSampler nearest_sampler = VK_NULL_HANDLE;

	struct
	{
		uint32_t  vertices_count = 0;
		uint32_t  indices_count  = 0;
		uint32_t  instance_count = 0;
		uint32_t  material_count = 0;
		glm::vec3 min_extent     = glm::vec3(0.f);
		uint32_t  emitter_count  = 0;
		glm::vec3 max_extent     = glm::vec3(0.f);
	} scene_info;

	Scene(const std::string &scene_filename, const std::string &hdr_filename, const Context &context);

	~Scene();

	void load_scene(const std::string &filename);

	void load_envmap(const std::string &filename);

  private:
	void destroy_scene();

	void destroy_envmap();

  private:
	const Context *m_context = nullptr;
};