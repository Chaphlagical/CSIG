#pragma once

#include "render/context.hpp"

#include <glm/glm.hpp>

#include <string>
#include <vector>

struct Context;

struct GlobalBuffer
{
	glm::mat4 view;
	glm::mat4 projection;
	glm::mat4 view_projection;
};

struct Scene
{
	AccelerationStructure tlas;

	std::vector<AccelerationStructure> blas;

	Buffer instances;
	Buffer lights;
	Buffer materials;
	Buffer vertex_buffer;
	Buffer index_buffer;
	Buffer global;

	std::vector<Texture> textures;

	Texture envmap;

	uint32_t vertices_count = 0;
	uint32_t indices_count = 0;

	Scene(const std::string &filename, const Context &context);

	~Scene();

	void load_scene(const std::string &filename);
	void load_envmap(const std::string &filename);

  private:
	void destroy_scene();

  private:
	const Context *m_context = nullptr;
};