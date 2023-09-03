#ifndef RAYTRACE_GLSL
#define RAYTRACE_GLSL

#include "scene.glsl"
#include "common_data.hpp"
#include "common.glsl"
#include "shade_state.glsl"

struct Ray
{
  vec3 origin;
  vec3 direction;
};

struct RtPayload
{
  uint   seed;
  float  hit_t;
  int    primitive_id;
  int    instance_id;
  int    instance_custom_index;
  vec2   bary_coord;
  mat4x3 object_to_world;
  mat4x3 world_to_object;
} prd;

struct LightSample
{
	vec3 le;
	vec3 dir;
	vec3 pos;
	vec3 norm;
	float dist;
	float pdf;
	uint id;
};

float power_heuristic(float a, float b)
{
  float t = a * a;
  return t / (b * b + t);
}

bool hit_test(in rayQueryEXT ray_query, in Ray r)
{
	int instance_id = rayQueryGetIntersectionInstanceIdEXT(ray_query, false);
	int primitive_id = rayQueryGetIntersectionPrimitiveIndexEXT(ray_query, false);

	Instance instance = get_instance(instance_id);
	Material material = get_material(instance.material);

	if(material.alpha_mode == 0)	// Opaque
	{
		return true;
	}

	float base_color_alpha = material.base_color.a;
	if(material.base_color_texture > -1)
	{
		const uint ind0 = get_index(instance.indices_offset + primitive_id * 3 + 0);
		const uint ind1 = get_index(instance.indices_offset + primitive_id * 3 + 1);
		const uint ind2 = get_index(instance.indices_offset + primitive_id * 3 + 2);

		const Vertex v0 = get_vertex(instance.vertices_offset + ind0);
		const Vertex v1 = get_vertex(instance.vertices_offset + ind1);
		const Vertex v2 = get_vertex(instance.vertices_offset + ind2);

		const vec2 uv0 = vec2(v0.position.w, v0.normal.w);
		const vec2 uv1 = vec2(v1.position.w, v1.normal.w);
		const vec2 uv2 = vec2(v2.position.w, v2.normal.w);

		vec2 bary = rayQueryGetIntersectionBarycentricsEXT(ray_query, false);
		const vec2 tex_coord = uv0 * (1.0 - bary.x - bary.y) + uv1 * bary.x + uv2 * bary.y;

		base_color_alpha *= texture(textures[material.base_color_texture], tex_coord).a;
	}

	float opacity;
	if(material.alpha_mode == 1)
	{
		// Masking
		opacity = base_color_alpha > material.cutoff ? 1.0 : 0.0;
	}
	else
	{
		// Blending
		opacity = base_color_alpha;
	}

	if(rand(prd.seed) > opacity)
	{
		return false;
	}

	return true;
}

bool closest_hit(Ray ray)
{
	uint ray_flags = gl_RayFlagsCullBackFacingTrianglesEXT;
  	prd.hit_t      = Infinity;

	rayQueryEXT ray_query;
	rayQueryInitializeEXT(ray_query,
						tlas,
						ray_flags,
						0xFF,
						ray.origin,
						0.0,
						ray.direction,
						Infinity);

	while(rayQueryProceedEXT(ray_query))
	{
		if(rayQueryGetIntersectionTypeEXT(ray_query, false) == gl_RayQueryCandidateIntersectionTriangleEXT)
		{
			if(hit_test(ray_query, ray))
			{
				rayQueryConfirmIntersectionEXT(ray_query);
			}
		}
	}

	bool hit = (rayQueryGetIntersectionTypeEXT(ray_query, true) != gl_RayQueryCommittedIntersectionNoneEXT);

	if(hit)
	{
		prd.hit_t               	= rayQueryGetIntersectionTEXT(ray_query, true);
    	prd.primitive_id         	= rayQueryGetIntersectionPrimitiveIndexEXT(ray_query, true);
    	prd.instance_id          	= rayQueryGetIntersectionInstanceIdEXT(ray_query, true);
    	prd.instance_custom_index 	= rayQueryGetIntersectionInstanceCustomIndexEXT(ray_query, true);
    	prd.bary_coord           	= rayQueryGetIntersectionBarycentricsEXT(ray_query, true);
    	prd.object_to_world       	= rayQueryGetIntersectionObjectToWorldEXT(ray_query, true);
    	prd.world_to_object       	= rayQueryGetIntersectionWorldToObjectEXT(ray_query, true);
	}
	else
	{
		prd.hit_t               	= Infinity;
    	prd.primitive_id         	= 0;
    	prd.instance_id          	= 0;
    	prd.instance_custom_index 	= 0;
    	prd.bary_coord           	= vec2(0);
	}

	return hit;
}

bool any_hit(Ray ray, float max_dist)
{
	rayQueryEXT ray_query;
	rayQueryInitializeEXT(
		ray_query, 
		tlas, 
		gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT, 
		0xFF,
		ray.origin,
		ShadowEpsilon,
		ray.direction,
		(1.0 - ShadowEpsilon) * max_dist);

	while(rayQueryProceedEXT(ray_query))
	{
		if(rayQueryGetIntersectionTypeEXT(ray_query, false) == gl_RayQueryCandidateIntersectionTriangleEXT)
		{
			if(hit_test(ray_query, ray))
			{
				rayQueryConfirmIntersectionEXT(ray_query);
			}
		}
	}

	return rayQueryGetIntersectionTypeEXT(ray_query, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}

vec3 offset_ray(vec3 p, vec3 n)
{
    const float intScale = 256.0f;
    const float floatScale = 1.0f / 65536.0f;
    const float origin = 1.0f / 32.0f;

    ivec3 of_i = ivec3(intScale * n.x, intScale * n.y, intScale * n.z);

    vec3 p_i = vec3(intBitsToFloat (floatBitsToInt(p.x) + ((p.x < 0) ? -of_i.x : of_i.x)),
                  intBitsToFloat (floatBitsToInt(p.y) + ((p.y < 0) ? -of_i.y : of_i.y)),
                  intBitsToFloat (floatBitsToInt(p.z) + ((p.z < 0) ? -of_i.z : of_i.z)));

    return vec3(abs(p.x) < origin ? p.x + floatScale * n.x : p_i.x, //
              abs(p.y) < origin ? p.y + floatScale * n.y : p_i.y, //
              abs(p.z) < origin ? p.z + floatScale * n.z : p_i.z);
}

ShadeState get_shade_state(Ray ray, RtPayload payload)
{
	const uint instance_id = payload.instance_custom_index;
	const uint primitive_id = payload.primitive_id;
	const vec3 bary = vec3(1.0 - payload.bary_coord.x - payload.bary_coord.y, payload.bary_coord.x, payload.bary_coord.y);
	const Instance instance = get_instance(instance_id);

	const uint ind0 = get_index(instance.indices_offset + primitive_id * 3 + 0);
	const uint ind1 = get_index(instance.indices_offset + primitive_id * 3 + 1);
	const uint ind2 = get_index(instance.indices_offset + primitive_id * 3 + 2);

	const Vertex v0 = get_vertex(instance.vertices_offset + ind0);
	const Vertex v1 = get_vertex(instance.vertices_offset + ind1);
	const Vertex v2 = get_vertex(instance.vertices_offset + ind2);

	const vec3 position = v0.position.xyz * bary.x + v1.position.xyz * bary.y + v2.position.xyz * bary.z;
	const vec3 world_position = vec3(payload.object_to_world * vec4(position, 1.0));

	const vec3 normal = v0.normal.xyz * bary.x + v1.normal.xyz * bary.y + v2.normal.xyz * bary.z;
	vec3 world_normal = normalize(vec3(normal * payload.world_to_object));
  	vec3 geom_normal  = normalize(cross(v1.position.xyz - v0.position.xyz, v2.position.xyz - v0.position.xyz));
  	vec3 wgeom_normal = normalize(vec3(geom_normal * payload.world_to_object));
	vec3 ffnormal = dot(world_normal, ray.direction) <= 0.0 ? world_normal : -world_normal;

	vec3 world_tangent, world_bitangent;
	coordinate_system(ffnormal, world_tangent, world_bitangent);

	const vec2 uv0 = vec2(v0.position.w, v0.normal.w);
	const vec2 uv1 = vec2(v1.position.w, v1.normal.w);
	const vec2 uv2 = vec2(v2.position.w, v2.normal.w);

	const vec2 tex_coord = uv0 * bary.x + uv1 * bary.y + uv2 * bary.z;

	Material material = get_material(instance.material);

	if(material.normal_texture > -1)
	{
		mat3 TBN = mat3(world_tangent, world_bitangent, world_normal);
		vec3 normal_vec = textureLod(textures[material.normal_texture], tex_coord, 0).xyz;
		normal_vec = normalize(normal_vec * 2.0 - 1.0);
		world_normal = normalize(TBN * normal_vec);
		ffnormal = dot(world_normal, ray.direction) <= 0.0 ? world_normal : -world_normal;
		coordinate_system(ffnormal, world_tangent, world_bitangent);
	}

	if(material.metallic_roughness_texture > -1)
	{
		vec3 metallic_roughness = textureLod(textures[material.metallic_roughness_texture], tex_coord, 0).xyz;
		material.roughness_factor *= metallic_roughness.g;
		material.metallic_factor *= metallic_roughness.b;
	}
	material.roughness_factor = max(0.001, material.roughness_factor);

	if(material.base_color_texture > -1)
	{
		vec4 base_color = textureLod(textures[material.base_color_texture], tex_coord, 0);
		base_color.rgb = pow(base_color.rgb, vec3(2.2));
		material.base_color *= base_color;
	}

	ShadeState sstate;
	sstate.normal = world_normal;
	sstate.geom_normal = wgeom_normal;
	sstate.ffnormal = ffnormal;
	sstate.position = world_position;
	sstate.tangent = world_tangent;
	sstate.bitangent = world_bitangent;
	sstate.mat = material;
	sstate.depth = payload.hit_t;

	if(dot(sstate.normal, sstate.geom_normal) <= 0)
	{
		sstate.normal *= -1.0f;
	}

	sstate.eta = dot(sstate.normal, sstate.ffnormal) > 0.0 ? 1.0 / 1.5 : 1.5;
	sstate.primary = false;
	return sstate;
}

LightSample sample_light_idx(ShadeState sstate, uint idx)
{
	LightSample ls;
	ls.le = vec3(0);
	ls.pdf = 1.0;
	ls.id = idx;

	if(idx < scene_data.emitter_count)
	{
		Emitter emitter = get_emitter(idx);
		vec3 p0 = emitter.p0.xyz;
		vec3 p1 = emitter.p1.xyz;
		vec3 p2 = emitter.p2.xyz;
		vec3 n0 = emitter.n0.xyz;
		vec3 n1 = emitter.n1.xyz;
		vec3 n2 = emitter.n2.xyz;
		vec3 intensity = emitter.intensity.xyz;

		float area = 0.5 * length(cross(p1 - p0, p2 - p1));
		float a = sqrt(rand(prd.seed));
		float b = a * rand(prd.seed);
		
		ls.pos = p0.xyz + (p1 - p0).xyz * (1.0 - a) + (p2 - p0).xyz * b;
		ls.norm = normalize(n0.xyz + (n1 - n0).xyz * (1.0 - a) + (n2 - n0).xyz * b);
		ls.dir = normalize(ls.pos - sstate.position);
		ls.dist = length(ls.pos - sstate.position);
		ls.le = intensity;
		ls.pdf = ls.dist * ls.dist / (area * abs(dot(ls.norm, -ls.dir)));
	}
	return ls;
}

LightSample sample_light(ShadeState sstate)
{
	if(scene_data.emitter_count == 0)
	{
		LightSample ls;
		ls.le = vec3(0);
		ls.pdf = 1.0;
		return ls;
	}

	// Sample a emitter
	uint emitter_id;
	float emitter_pdf;
	sample_emitter_alias_table(rand2(prd.seed), emitter_id, emitter_pdf);
	LightSample ls = sample_light_idx(sstate, emitter_id);
	ls.id = emitter_id;
	ls.pdf *= emitter_pdf;
	return ls;
}

#endif