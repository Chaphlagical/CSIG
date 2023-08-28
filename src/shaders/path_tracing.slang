#include "scene.slangh"
#include "gbuffer.slangh"
#include "common.slangh"

struct PushConstant
{
	int max_depth;
	float bias;
	uint frame_count;
};

[[vk::binding(0, 2)]] RWTexture2D<float4> RenderTarget;
[[vk::binding(1, 2)]] Texture2D<float4> PrevRenderTarget;
[[vk::push_constant]] ConstantBuffer<PushConstant> push_constant;

vec3 path_trace(vec2 frag_coord)
{
	vec3 radiance   = vec3(0.0);
  	vec3 throughput = vec3(1.0);
	Ray ray;
	ShadeState sstate;
	bool is_hit = get_primary_state(frag_coord, 0, sstate);
	ray.origin = ubo.cam_pos.xyz;
	ray.direction = normalize(sstate.position - ubo.cam_pos.xyz);
	
	for(uint trace_depth = 0; trace_depth < push_constants.max_depth; trace_depth++)
	{
		if(!is_hit)
		{
			radiance += throughput * textureLod(skybox, ray.direction, 0).rgb;
			break;
		}

		radiance += sstate.mat.emissive_factor * throughput;

		if(scene_data.emitter_count > 0)
		{
			LightSample ls = sample_light(sstate);
			Ray shadow_ray;
			shadow_ray.origin = offset_ray(sstate.position, dot(ls.dir, sstate.ffnormal) > 0 ? sstate.ffnormal : -sstate.ffnormal);
			shadow_ray.direction = ls.dir;
			if(!any_hit(shadow_ray, length(shadow_ray.origin - ls.pos)))
			{
				float bsdf_pdf;
				vec3 f = eval_bsdf(sstate, -ray.direction, sstate.ffnormal, ls.dir, bsdf_pdf);
				float mis_weight = max(0.0, power_heuristic(ls.pdf, bsdf_pdf));
				radiance += throughput * mis_weight * f * ls.le * abs(dot(sstate.ffnormal, ls.dir)) / ls.pdf;
			}
		}
		
		BSDFSample bs = sample_bsdf(sstate, -ray.direction, prd.seed);
		if(bs.pdf > 0.0)
		{
			throughput *= bs.f * abs(dot(sstate.ffnormal, bs.L)) / bs.pdf;
		}
		else
		{
			break;
		}

		float rrPcont = (trace_depth >= 3) ?
			min(max(throughput.x, max(throughput.y, throughput.z)) * sstate.eta * sstate.eta + 0.001, 0.95) :
			1.0;

		ray.direction = bs.L;
		ray.origin = sstate.position + push_constants.bias * (dot(bs.L, sstate.ffnormal) > 0 ? sstate.ffnormal : -sstate.ffnormal);

		if(rand(prd.seed) >= rrPcont)
		{
			break;
		}

		throughput /= rrPcont;

		is_hit = closest_hit(ray);
		if(is_hit)
		{
			sstate = get_shade_state(ray, prd);
		}
	}

	float lum = luminance(radiance);
	if(lum > 1.0)
	{
		radiance *= 1.0 / lum;
	}

	return radiance;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(CSParam param)
{
    const ivec2 size = textureSize(gbufferA, 0);
	const ivec2 coord = ivec2(gl_GlobalInvocationID.xy);

	prd.seed = tea(size.x * gl_GlobalInvocationID.y + gl_GlobalInvocationID.x, uint(ubo.cam_pos.w));

	const vec2 jittered_coord = vec2(coord) + rand2(prd.seed);
	const vec2 tex_coord = jittered_coord / vec2(size);
	const vec2 tex_coord_neg_to_pos = tex_coord * 2.0 - 1.0;

	vec3 pixel_color = path_trace(vec2(coord) + vec2(0.5));

	if(push_constants.frame_count == 0)
	{
		imageStore(path_tracing_image, ivec2(gl_GlobalInvocationID.xy), vec4(pixel_color, 1));
	}
	else
	{
		if(!any(isnan(pixel_color)))
		{
			vec3 prev_color = imageLoad(prev_path_tracing_image, ivec2(gl_GlobalInvocationID.xy)).rgb;
			vec3 accumulated_color = mix(prev_color, pixel_color, 1.0 / float(push_constants.frame_count));
			imageStore(path_tracing_image, ivec2(gl_GlobalInvocationID.xy), vec4(accumulated_color, 1));
		}
	}
}