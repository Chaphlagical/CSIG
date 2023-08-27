#pragma once

#include "context.hpp"

struct Scene
{
	struct
	{
		VkDescriptorSetLayout layout = VK_NULL_HANDLE;
		VkDescriptorSet       set    = VK_NULL_HANDLE;
	} descriptor;

	AccelerationStructure              tlas;
	std::vector<AccelerationStructure> blas;

	struct
	{
		Buffer instance;
		Buffer emitter;
		Buffer material;
		Buffer vertex;
		Buffer index;
		Buffer indirect_draw;
		Buffer global;
		Buffer emitter_alias_table;
		Buffer mesh_alias_table;
		Buffer scene;
	} buffer;

	std::vector<Texture>     textures;
	std::vector<VkImageView> texture_views;

	struct
	{
		Texture texture;
		Texture irradiance_sh;
		Texture prefilter_map;

		VkImageView texture_view       = VK_NULL_HANDLE;
		VkImageView irradiance_sh_view = VK_NULL_HANDLE;
		VkImageView prefilter_map_view = VK_NULL_HANDLE;
	} envmap;

	struct
	{
		uint32_t  vertices_count = 0;
		uint32_t  indices_count  = 0;
		uint32_t  instance_count = 0;
		uint32_t  material_count = 0;
		glm::vec3 min_extent     = glm::vec3(std::numeric_limits<float>::max());
		uint32_t  emitter_count  = 0;
		glm::vec3 max_extent     = -glm::vec3(std::numeric_limits<float>::max());
		uint32_t  mesh_count     = 0;
	} scene_info;

	Scene(const Context &context);

	~Scene();

	void load_scene(const std::string &filename);

	void load_envmap(const std::string &filename);

  private:
	void destroy_scene();

	void destroy_envmap();

	const Context *m_context = nullptr;
};