#include "restir.slangh"
#include "gbuffer.slangh"

#define NUM_THREADS_X 8
#define NUM_THREADS_Y 8

struct PushConstant
{
    uint gbuffer_mip;
    uint temporal_reuse;
    int M;
    int clamp_threshold;
};

[[vk::binding(0, 2)]] StructuredBuffer<Reservoir> TemporalReservoirBuffer;
[[vk::binding(1, 2)]] RWStructuredBuffer<Reservoir> PassthroughReservoirBuffer;
[[vk::push_constant]] ConstantBuffer<PushConstant> push_constant;

[shader("compute")]
[numthreads(NUM_THREADS_X, NUM_THREADS_Y, 1)]
void main(CSParam param)
{
    const int2 coord = int2(param.DispatchThreadID.xy);
    const int2 size = texture_size(GBufferA, push_constant.gbuffer_mip);
    const uint pixel_id = coord.x * size.y + coord.y;

    if (coord.x >= size.x || coord.y >= size.y)
    {
        return;
    }

    PassthroughReservoirBuffer[pixel_id] = init_reservoir();

    GroupMemoryBarrierWithGroupSync();

    prd.seed = tea(size.x * param.DispatchThreadID.y + param.DispatchThreadID.x, uint(ViewBuffer.cam_pos.w));

    Reservoir r = init_reservoir();

    ShadeState sstate;
    if (get_primary_state(float2(coord) + float2(0.5), push_constant.gbuffer_mip, sstate))
    {
        const uint light_samples = min(SceneBuffer.emitter_count, push_constant.M);

        for (uint i = 0; i < light_samples; i++)
        {
            LightSample ls = sample_light(sstate);
            float p_hat = eval_phat(ls.id, sstate);
            add_sample_to_reservoir(r, int(ls.id), ls.pdf, p_hat);
        }

        // Visibility check
        if (r.num_samples > 0)
        {
            LightSample ls = sample_light_idx(sstate, r.light_id);
            RayDesc shadow_ray;
            shadow_ray.Origin = offset_ray(sstate.position, dot(ls.dir, sstate.ffnormal) > 0 ? sstate.ffnormal : -sstate.ffnormal);
            shadow_ray.Direction = ls.dir;
            if (any_hit(shadow_ray.Origin, shadow_ray.Direction, length(shadow_ray.Origin - ls.pos)))
            {
                r.w = 0.0;
                r.sum_weights = 0.0;
            }
        }

        if (push_constant.temporal_reuse == 1)
        {
            float4 ndc = mul(ViewBuffer.prev_view_projection, float4(sstate.position, 1.0));
            ndc /= ndc.w;
            float2 prev_frame_pos = (ndc.xy + float2(1)) * 0.5 * float2(size);
            if (all(prev_frame_pos.xy > float2(0.0)) &&
                all(prev_frame_pos.xy < float2(size)))
            {
                ShadeState prev_sstate;
                if (get_prev_primary_state(float2(uint2(prev_frame_pos.xy)), push_constant.gbuffer_mip, prev_sstate))
                {
                    float3 pos_diff = sstate.position - prev_sstate.position;
                    if (dot(pos_diff, pos_diff) < 0.01 &&
                        abs(sstate.depth - prev_sstate.depth) < prev_sstate.depth * 0.001)
                    {
                        float3 base_color_diff = sstate.mat.base_color.rgb - prev_sstate.mat.base_color.rgb;
                        if (dot(base_color_diff, base_color_diff) < 0.01)
                        {
                            float normal_dot = dot(sstate.ffnormal, prev_sstate.ffnormal);
                            if (normal_dot > 0.5)
                            {
                                const uint prev_idx = uint(prev_frame_pos.x) * size.y + uint(prev_frame_pos.y);
                                Reservoir prev_r = TemporalReservoirBuffer.Load(prev_idx);
                                prev_r.num_samples = min(prev_r.num_samples, push_constant.clamp_threshold * r.num_samples);
                                if (prev_r.num_samples > 0)
                                {
                                    float p_hat = eval_phat(prev_r.light_id, sstate);
                                    combine_reservoir(r, prev_r, p_hat);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    PassthroughReservoirBuffer[pixel_id] = r;
}