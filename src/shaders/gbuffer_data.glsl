#ifndef GBUFFER_DATA_GLSL
#define GBUFFER_DATA_GLSL

#include "shade_state.glsl"
#include "scene.glsl"

layout(set = 1, binding = 0) uniform sampler2D gbufferA;
layout(set = 1, binding = 1) uniform sampler2D gbufferB;
layout(set = 1, binding = 2) uniform sampler2D gbufferC;
layout(set = 1, binding = 3) uniform sampler2D depth_buffer;
layout(set = 1, binding = 4) uniform sampler2D prev_gbufferA;
layout(set = 1, binding = 5) uniform sampler2D prev_gbufferB;
layout(set = 1, binding = 6) uniform sampler2D prev_gbufferC;
layout(set = 1, binding = 7) uniform sampler2D prev_depth_buffer;

bool get_primary_state(vec2 frag_coord, int mip, out ShadeState sstate)
{
	const vec4 gbufferA_data = texelFetch(gbufferA, ivec2(frag_coord), mip);
	const vec4 gbufferB_data = texelFetch(gbufferB, ivec2(frag_coord), mip);
	const vec4 gbufferC_data = texelFetch(gbufferC, ivec2(frag_coord), mip);
	const float depth = texelFetch(depth_buffer, ivec2(frag_coord), mip).r;
	const ivec2 image_size = textureSize(depth_buffer, mip);

	uint instance_id = uint(gbufferC_data.b);
	const Instance instance = get_instance(instance_id);
	
	Material material = get_material(instance.material);

	vec3 p = world_position_from_depth(frag_coord / vec2(image_size), depth, ubo.view_projection_inv);
	vec3 n = octohedral_to_direction(gbufferB_data.rg);
	material.base_color.rgb = gbufferA_data.rgb; 
	material.roughness_factor = max(0.001, gbufferC_data.r);
	material.metallic_factor = gbufferA_data.a;

	sstate.normal = n;
	sstate.geom_normal = n;
	sstate.ffnormal = dot(n, p - ubo.cam_pos.xyz) < 0.0 ? n : -n;;
	sstate.position = p;
	sstate.mat = material;
	
	coordinate_system(sstate.ffnormal, sstate.tangent, sstate.bitangent);

	sstate.eta = dot(sstate.normal, sstate.ffnormal) > 0.0 ? 1.0 / 1.5 : 1.5;
	sstate.primary = true;

	return depth != 0;

    return false;
}

#endif