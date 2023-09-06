#include "gbuffer.slangh"
#include "restir.slangh"

#define NUM_THREADS_X 8
#define NUM_THREADS_Y 8

struct PushConstant
{
    uint gbuffer_mip;
    uint spatial_reuse;
    float radius;
    uint samples;
};

[[vk::binding(0, 2)]] RWStructuredBuffer<Reservoir> SpatialReservoirBuffer;
[[vk::binding(1, 2)]] StructuredBuffer<Reservoir> PassthroughReservoirBuffer;
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

    SpatialReservoirBuffer[pixel_id] = init_reservoir();
    GroupMemoryBarrierWithGroupSync();

    prd.seed = tea(size.x * param.DispatchThreadID.y + param.DispatchThreadID.x, uint(ViewBuffer.cam_pos.w));

    Reservoir r = PassthroughReservoirBuffer.Load(pixel_id);
    ShadeState sstate;

    if (push_constant.spatial_reuse == 1 &&
        get_primary_state(float2(coord) + float2(0.5), push_constant.gbuffer_mip, sstate) &&
        dot(sstate.ffnormal, sstate.ffnormal) != 0)
    {
        for (int i = 0; i < push_constant.samples; i++)
        {
            const float angle = rand(prd.seed) * 2 * PI;
            const float radius = sqrt(rand(prd.seed)) * push_constant.radius;
            int2 rand_offset = int2(int(floor(cos(angle) * radius)), int(floor(sin(angle) * radius)));
            const int2 neighbor_coord = clamp(int2(coord.xy) + rand_offset, int2(0), int2(size.xy) - 1);
            ShadeState neighbor_sstate;
            if (get_primary_state(float2(neighbor_coord) + float2(0.5), push_constant.gbuffer_mip, neighbor_sstate))
            {
                float3 pos_diff = sstate.position - neighbor_sstate.position;
                if (dot(pos_diff, pos_diff) < 0.01 && abs(sstate.depth - neighbor_sstate.depth) < sstate.depth * 0.1)
                {
                    float3 base_color_diff = sstate.mat.base_color.rgb - neighbor_sstate.mat.base_color.rgb;
                    if (dot(base_color_diff, base_color_diff) < 0.01)
                    {
                        float normal_dot = dot(sstate.ffnormal, neighbor_sstate.ffnormal);
                        if (normal_dot > 0.9063)
                        {
                            const int neighbor_idx = neighbor_coord.x * size.y + neighbor_coord.y;
                            const Reservoir r_neightbor = PassthroughReservoirBuffer.Load(neighbor_idx);
                            if (r_neightbor.num_samples > 0)
                            {
                                float p_hat = eval_phat(r_neightbor.light_id, sstate);
                                combine_reservoir(r, r_neightbor, p_hat);
                            }
                        }
                    }
                }
            }
        }
    }
    SpatialReservoirBuffer[pixel_id] = r;
}