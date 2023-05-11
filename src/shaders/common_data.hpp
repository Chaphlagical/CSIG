#ifndef COMMON_DATA_HPP
#define COMMON_DATA_HPP

#ifdef CPP
#	include <glm/glm.hpp>
using mat4 = glm::mat4;
using vec4 = glm::vec4;
using vec3 = glm::vec3;
using vec2 = glm::vec2;
using uint = uint32_t;
#endif        // CPP

struct GlobalData
{
	mat4 view_inv;
	mat4 projection_inv;
	mat4 view_projection_inv;
	mat4 view_projection;
	mat4 prev_view;
	mat4 prev_projection;
	mat4 prev_view_projection;
	vec4 cam_pos;        // xyz - position, w - num_frames
	vec4 jitter;
};

struct Vertex
{
	vec4 position;        // xyz - position, w - texcoord u
	vec4 normal;          // xyz - normal, w - texcoord v
};

struct Instance
{
	mat4 transform;
	mat4 transform_inv;

	uint vertices_offset;
	uint vertices_count;
	uint indices_offset;
	uint indices_count;

	uint  mesh;
	uint  material;
	int  emitter;
	float area;
};

struct Emitter
{
	mat4 transform;
	vec3 intensity;
	uint instance_id;
};

struct Material
{
	uint    alpha_mode;        // 0 - opaque, 1 - mask, 2 - blend
	uint    double_sided;
	float   cutoff;
	float   metallic_factor;
	float   roughness_factor;
	float   transmission_factor;
	float   clearcoat_factor;
	float   clearcoat_roughness_factor;
	vec4    base_color;
	vec3    emissive_factor;
	int     base_color_texture;
	int32_t normal_texture;
	int32_t metallic_roughness_texture;
	vec2    padding;
};

struct SceneData
{
	uint     vertices_count;
	uint     indices_count;
	uint     instance_count;
	uint     material_count;
	vec3     min_extent;
	uint     emitter_count;
	vec3     max_extent;
	uint     mesh_count;
	uint64_t instance_buffer_addr;
	uint64_t emitter_buffer_addr;
	uint64_t material_buffer_addr;
	uint64_t vertex_buffer_addr;
	uint64_t index_buffer_addr;
	uint64_t emitter_alias_table_buffer_addr;
	uint64_t mesh_alias_table_buffer_addr;
};

struct AliasTable
{
	float prob;         // The i's column's event i's prob
	int   alias;        // The i's column's another event's idx
	float ori_prob;
	float alias_ori_prob;
};

#endif        // !COMMON_DATA_HPP
