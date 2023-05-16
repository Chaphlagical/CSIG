#ifndef COMMON_DATA_HPP
#define COMMON_DATA_HPP

#ifdef CPP
#	include <glm/glm.hpp>
using mat4 = glm::mat4;
using vec4 = glm::vec4;
using vec3 = glm::vec3;
using vec2 = glm::vec2;
using uint = uint32_t;
#else
const float PI            = 3.14159265358979323846;
const float InvPI         = 0.31830988618379067154;
const float Inv2PI        = 0.15915494309189533577;
const float Inv4PI        = 0.07957747154594766788;
const float PIOver2       = 1.57079632679489661923;
const float PIOver4       = 0.78539816339744830961;
const float Sqrt2         = 1.41421356237309504880;
const float ShadowEpsilon = 0.0001;
const float Epsilon       = 1e-7;
const float Infinity      = 1e32;

void coordinate_system(vec3 N, out vec3 Nt, out vec3 Nb)
{
	Nt = normalize(((abs(N.z) > 0.99999f) ? vec3(-N.x * N.y, 1.0f - N.y * N.y, -N.y * N.z) :
	                                        vec3(-N.x * N.z, -N.y * N.z, 1.0f - N.z * N.z)));
	Nb = normalize(cross(Nt, N));
}

float luminance(vec3 color)
{
	return dot(color, vec3(0.212671, 0.715160, 0.072169));
}
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
	mat4 prev_view_projection_inv;
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
	int   emitter;
	float area;
};

struct Emitter
{
	vec4 p0;
	vec4 p1;
	vec4 p2;
};

#ifndef CPP
void unpack_emitter(Emitter emitter, out vec3 p0, out vec3 p1, out vec3 p2, out vec3 intensity)
{
	p0        = emitter.p0.xyz;
	p1        = emitter.p1.xyz;
	p2        = emitter.p2.xyz;
	intensity = vec3(emitter.p0.w, emitter.p1.w, emitter.p2.w);
}
#endif

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

#ifdef CPP
	Material() :
	    alpha_mode(0),
	    double_sided(false),
	    cutoff(0.f),
	    metallic_factor(0.f),
	    roughness_factor(0.f),
	    transmission_factor(0.f),
	    clearcoat_factor(0.f),
	    clearcoat_roughness_factor(0.f),
	    base_color(glm::vec4(1.f)),
	    emissive_factor(glm::vec3(1.f)),
	    base_color_texture(-1),
	    normal_texture(-1),
	    metallic_roughness_texture(-1)
	{
	}
#endif
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

struct Reservoir
{
	int   light_id;
	float p_hat;
	float sum_weights;
	float w;
	uint  num_samples;
};

#endif        // !COMMON_DATA_HPP
