#ifndef COMMON_GLSL
#define COMMON_GLSL

const float PI = 3.14159265358979323846;
const float InvPI = 0.31830988618379067154;
const float Inv2PI = 0.15915494309189533577;
const float Inv4PI = 0.07957747154594766788;
const float PIOver2 = 1.57079632679489661923;
const float PIOver4 = 0.78539816339744830961;
const float Sqrt2 = 1.41421356237309504880;
const float ShadowEpsilon = 0.0001;
const float Epsilon = 1e-7;
const float Infinity = 1e32;

struct Instance
{
	mat4 transform;
	mat4 transform_inv;
	uint vertices_offset;
	uint vertices_count;
	uint indices_offset;
	uint indices_count;
	uint mesh;
	uint material;
	vec2 padding;
};

struct Material
{
	uint  alpha_mode;
	uint  double_sided;
	float cutoff;
	float metallic_factor;
	float roughness_factor;
	float transmission_factor;
	float clearcoat_factor;
	float clearcoat_roughness_factor;
	vec4  base_color;
	vec3  emissive_factor;
	uint  base_color_texture;
	uint  normal_texture;
	uint  metallic_roughness_texture;
	vec2  padding;
};

struct Vertex
{
	vec4 position;
	vec4 normal;
	vec4 tangent;
};

struct PointLight
{
	vec3 intensity;
	uint instance_id;
	vec3 position;
};

struct AreaLight
{
	mat4 transform;
	vec3 intensity;
};

void coordinate_system(vec3 v1, out vec3 v2, out vec3 v3)
{
	const vec3 ref = abs(dot(v1, vec3(0, 1, 0))) > 0.99f ? vec3(0, 0, 1) : vec3(0, 1, 0);
	v2 = normalize(cross(ref, v1));
	v3 = cross(v1, v2);
}

vec2 direction_to_octohedral(vec3 normal)
{
    vec2 p = normal.xy * (1.0f / dot(abs(normal), vec3(1.0)));
    return normal.z > 0.0f ? p : (1.0f - abs(p.yx)) * (step(0.0, p) * 2.0 - vec2(1.0));
}

vec3 octohedral_to_direction(vec2 e)
{
    vec3 v = vec3(e, 1.0 - abs(e.x) - abs(e.y));

    if (v.z < 0.0)
	{
        v.xy = (1.0 - abs(v.yx)) * (step(0.0, v.xy) * 2.0 - vec2(1.0));
	}

    return normalize(v);
}

vec3 world_position_from_depth(vec2 uv, float ndc_depth, mat4 view_proj_inv)
{
    vec2 screen_pos = uv * 2.0 - 1.0;
    vec4 ndc_pos = vec4(screen_pos, ndc_depth, 1.0);
	vec4 world_pos  = view_proj_inv * ndc_pos;
    world_pos = world_pos / world_pos.w;
    return world_pos.xyz;
}

vec3 local_to_world(vec3 n, vec3 v)
{
	const vec3 ref = abs(dot(n, vec3(0, 1, 0))) > 0.99 ? vec3(0, 0, 1) : vec3(0, 1, 0);
    const vec3 x = normalize(cross(ref, n));
    const vec3 y = cross(n, x);
	return normalize(mat3(x,y,n) * v);
}

#endif        // !COMMON_GLSL
