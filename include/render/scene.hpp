#pragma once

#include "render/common.hpp"
#include "render/context.hpp"
#define CPP
#include "common_data.hpp"

#include <glm/glm.hpp>

#include <string>
#include <vector>

struct Context;

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
	Buffer emitter_alias_table_buffer;
	Buffer mesh_alias_table_buffer;
	Buffer scene_buffer;

	std::vector<Texture>     textures;
	std::vector<VkImageView> texture_views;

	Texture     ggx_preintegration;
	VkImageView ggx_preintegration_view = VK_NULL_HANDLE;

	Envmap envmap;

	VkSampler linear_sampler  = VK_NULL_HANDLE;
	VkSampler nearest_sampler = VK_NULL_HANDLE;

	SceneData scene_info = {};

	Scene(const Context &context);

	~Scene();

	void load_scene(const std::string &filename);

	void load_envmap(const std::string &filename);

	void update_descriptor();

  public:
	struct
	{
		VkDescriptorSetLayout layout = VK_NULL_HANDLE;
		VkDescriptorSet       set    = VK_NULL_HANDLE;
	} descriptor;

  private:
	void destroy_scene();
	void destroy_envmap();

  private:
	const Context *m_context = nullptr;
};