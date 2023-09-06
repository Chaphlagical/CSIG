#include "gbuffer.slangh"
#include "restir.slangh"

#define NUM_THREADS_X 8
#define NUM_THREADS_Y 8

struct PushConstant
{
    uint gbuffer_mip;
    float normal_bias;
};

[[vk::binding(0, 2)]] RWStructuredBuffer<Reservoir> TemporalReservoirBuffer;
[[vk::binding(1, 2)]] StructuredBuffer<Reservoir> SpatialReservoirBuffer;
[[vk::binding(2, 2)]] RWTexture2D<float4> Output_Image;
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

    TemporalReservoirBuffer[pixel_id] = init_reservoir();
    GroupMemoryBarrierWithGroupSync();

    prd.seed = tea(size.x * param.DispatchThreadID.y + param.DispatchThreadID.x, uint(ViewBuffer.cam_pos.w));

    Reservoir r = SpatialReservoirBuffer.Load(pixel_id);

    float3 radiance = 0.0;
    ShadeState sstate;

    if (get_primary_state(float2(coord) + float2(0.5), push_constant.gbuffer_mip, sstate))
    {
        radiance += sstate.mat.emissive_factor;
        if (r.w > 0)
        {
            radiance += r.w * eval_L(r.light_id, sstate);
            TemporalReservoirBuffer[pixel_id] = r;
        }
    }

    float lum = dot(radiance, float3(0.212671, 0.715160, 0.072169));
    if (lum > 1.0)
    {
        radiance *= 1.0 / lum;
    }

    if (any(isnan(radiance)))
    {
        radiance = float3(0);
    }

    Output_Image[int2(coord.xy)] = float4(radiance, 1.0);
}