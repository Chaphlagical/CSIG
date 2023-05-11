#ifndef RAYTRACE_GLSL
#define RAYTRACE_GLSL

#include "scene.glsl"
#include "common_data.hpp"

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
	vec3 radiance;
	vec3 dir;
	float dist;
	float pdf;
	bool visible;
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

	return (rayQueryGetIntersectionTypeEXT(ray_query, true) != gl_RayQueryCommittedIntersectionNoneEXT);
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

	if(dot(sstate.normal, sstate.geom_normal) <= 0)
	{
		sstate.normal *= -1.0f;
	}

	sstate.eta = dot(sstate.normal, sstate.ffnormal) > 0.0 ? 1.0 / 1.5 : 1.5;
	sstate.primary = false;
	return sstate;
}

LightSample sample_light(Ray ray, ShadeState sstate, float normal_bias, inout uint seed)
{
	LightSample ls;
	ls.radiance = vec3(0);
	ls.visible = false;
	ls.pdf = 1.0;

	if(scene_data.emitter_count == 0)
	{
		return ls;
	}

	// Sample a emitter
	uint emitter_id;
	sample_emitter_alias_table(rand2(seed), emitter_id, ls.pdf);

	if(emitter_id < scene_data.emitter_count)
	{
		Emitter emitter = get_emitter(emitter_id);
		Instance instance = get_instance(emitter.instance_id);

		// Sample a triangle
		uint primitive_id = uint(float(instance.indices_count / 3) * rand(prd.seed));

		const uint ind0 = get_index(instance.indices_offset + primitive_id * 3 + 0);
		const uint ind1 = get_index(instance.indices_offset + primitive_id * 3 + 1);
		const uint ind2 = get_index(instance.indices_offset + primitive_id * 3 + 2);

		const Vertex v0 = get_vertex(instance.vertices_offset + ind0);
		const Vertex v1 = get_vertex(instance.vertices_offset + ind1);
		const Vertex v2 = get_vertex(instance.vertices_offset + ind2);

		float a = sqrt(rand(seed));
		vec3 position = v0.position.xyz + (v1.position - v0.position).xyz * (1.0 - a) + (v2.position - v0.position).xyz * (a * rand(seed));
		vec3 normal = normalize(cross(v1.position.xyz - v0.position.xyz, v2.position.xyz - v1.position.xyz));
		vec3 sample_pos = (instance.transform * vec4(position, 1.0)).xyz;
		vec3 light_norm = normalize(transpose(mat3(instance.transform_inv)) * normal.xyz);

		ls.dir = normalize(sample_pos - sstate.position);
		float dist = length(sample_pos - sstate.position);

		if(dot(ls.dir, sstate.ffnormal) > 0)
		{
			Ray shadow_ray;
			shadow_ray.origin = sstate.position + normal_bias * (dot(ls.dir, sstate.ffnormal) > 0 ? sstate.ffnormal : -sstate.ffnormal);
			shadow_ray.direction = ls.dir;
			if(!any_hit(shadow_ray, dist))
			{
				ls.visible = true;
				ls.pdf *= dist * dist / abs(dot(light_norm, -ls.dir));
				vec3 light_contrib = emitter.intensity / ls.pdf;
				float bsdf_pdf;
				vec3 f = eval_bsdf(sstate, -ray.direction, sstate.ffnormal, ls.dir, bsdf_pdf);
				float mis_weight = max(0.0, power_heuristic(ls.pdf, bsdf_pdf));
				ls.radiance = mis_weight * f * light_contrib;
			}
		}
	}
	
	return ls;
}

#endif