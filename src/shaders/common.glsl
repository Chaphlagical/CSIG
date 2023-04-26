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
	float area;
	float padding;
};

struct Material
{
	uint  alpha_mode; // 0 - opaque, 1 - mask, 2 - blend
	uint  double_sided;
	float cutoff;
	float metallic_factor;
	float roughness_factor;
	float transmission_factor;
	float clearcoat_factor;
	float clearcoat_roughness_factor;
	vec4  base_color;
	vec3  emissive_factor;
	int  base_color_texture;
	int  normal_texture;
	int  metallic_roughness_texture;
	vec2  padding;
};

struct Vertex
{
	vec4 position;
	vec4 normal;
	vec4 tangent;
};

struct Emitter
{
	mat4 transform;
	vec3 intensity;
	uint instance_id;
};

struct ShadeState
{
	vec3 normal;
	vec3 geom_normal;
	vec3 ffnormal;
	vec3 position;
	vec3 tangent;
	vec3 bitangent;
	float eta;
	bool primary;
	Material mat;
};

void coordinate_system(vec3 N, out vec3 Nt, out vec3 Nb)
{
	Nt = normalize(((abs(N.z) > 0.99999f) ? vec3(-N.x * N.y, 1.0f - N.y * N.y, -N.y * N.z) :
                                          vec3(-N.x * N.z, -N.y * N.z, 1.0f - N.z * N.z)));
	Nb = normalize(cross(Nt, N));
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

vec3 offset_ray(vec3 p, vec3 n)
{
  const float intScale   = 256.0f;
  const float floatScale = 1.0f / 65536.0f;
  const float origin     = 1.0f / 32.0f;

  ivec3 of_i = ivec3(intScale * n.x, intScale * n.y, intScale * n.z);

  vec3 p_i = vec3(intBitsToFloat(floatBitsToInt(p.x) + ((p.x < 0) ? -of_i.x : of_i.x)),
                  intBitsToFloat(floatBitsToInt(p.y) + ((p.y < 0) ? -of_i.y : of_i.y)),
                  intBitsToFloat(floatBitsToInt(p.z) + ((p.z < 0) ? -of_i.z : of_i.z)));

  return vec3(abs(p.x) < origin ? p.x + floatScale * n.x : p_i.x,  //
              abs(p.y) < origin ? p.y + floatScale * n.y : p_i.y,  //
              abs(p.z) < origin ? p.z + floatScale * n.z : p_i.z);
}

#endif        // !COMMON_GLSL
