#include "scene.slangh"
#include "gbuffer.slangh"
#include "common.slangh"
#include "random.slangh"
#include "raytrace.slangh"
#include "bsdf.slangh"
#include "lighting.slangh"

struct PushConstant
{
	int max_depth;
	float bias;
	uint frame_count;
};

[[vk::binding(0, 2)]] RWTexture2D<float4> RenderTarget;
[[vk::binding(1, 2)]] Texture2D<float4> PrevRenderTarget;
[[vk::push_constant]] ConstantBuffer<PushConstant> push_constant;

float3 path_trace(float2 frag_coord)
{
	float3 radiance   = float3(0.0);
  	float3 throughput = float3(1.0);
	RayDesc ray;
	ShadeState sstate;
	bool is_hit = get_primary_state(frag_coord, 0, sstate);
	ray.Origin = ViewBuffer.cam_pos.xyz;
	ray.Direction = normalize(sstate.position - ViewBuffer.cam_pos.xyz);

	if(!is_hit)
	{
		return EnvMap.SampleLevel(Samplers[int(SamplerType::Linear)], ray.Direction, 0).rgb;
	}

	for(uint trace_depth = 0; trace_depth < push_constant.max_depth; trace_depth++)
	{
		if(!is_hit)
		{
			radiance += throughput * EnvMap.SampleLevel(Samplers[int(SamplerType::Linear)], ray.Direction, 0).rgb;
			break;
		}

		radiance += sstate.mat.emissive_factor * throughput;

		if(SceneBuffer.emitter_count > 0)
		{
			LightSample ls = sample_light(sstate);
			RayDesc shadow_ray;
			shadow_ray.Origin = offset_ray(sstate.position, dot(ls.dir, sstate.ffnormal) > 0 ? sstate.ffnormal : -sstate.ffnormal);
			shadow_ray.Direction = ls.dir;
			if(!any_hit(shadow_ray.Origin, shadow_ray.Direction, length(shadow_ray.Origin - ls.pos)))
			{
				float bsdf_pdf;
				float3 f = eval_bsdf(sstate, -ray.Direction, sstate.ffnormal, ls.dir, bsdf_pdf);
				float mis_weight = max(0.0, power_heuristic(ls.pdf, bsdf_pdf));
				radiance += throughput * mis_weight * f * ls.le * abs(dot(sstate.ffnormal, ls.dir)) / ls.pdf;
			}
		}
		
		BSDFSample bs = sample_bsdf(sstate, -ray.Direction, prd.seed);
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

		ray.Direction = bs.L;
		ray.Origin = sstate.position + push_constant.bias * (dot(bs.L, sstate.ffnormal) > 0 ? sstate.ffnormal : -sstate.ffnormal);

		if(rand(prd.seed) >= rrPcont)
		{
			break;
		}

		throughput /= rrPcont;

		is_hit = closest_hit(ray.Origin, ray.Direction, RAY_FLAG_CULL_BACK_FACING_TRIANGLES);
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
    const int2 size = texture_size(GBufferA, 0);
	const int2 coord = int2(param.DispatchThreadID.xy);

	prd.seed = tea(size.x * param.DispatchThreadID.y + param.DispatchThreadID.x, uint(ViewBuffer.cam_pos.w));

	const float2 jittered_coord = float2(coord) + rand2(prd.seed);
	const float2 tex_coord = jittered_coord / float2(size);
	const float2 tex_coord_neg_to_pos = tex_coord * 2.0 - 1.0;

	float3 pixel_color = path_trace(float2(coord) + float2(0.5));

	if(push_constant.frame_count == 0)
	{
		RenderTarget[param.DispatchThreadID.xy] = float4(pixel_color, 1);
	}
	else
	{
		if(!any(isnan(pixel_color)))
		{
			float3 prev_color = PrevRenderTarget.Load(int3(param.DispatchThreadID.xy, 0)).rgb;
			float3 accumulated_color = lerp(prev_color, pixel_color, 1.0 / float(push_constant.frame_count));
			RenderTarget[param.DispatchThreadID.xy] = float4(accumulated_color, 1);
		}
	}
}