#include "ddgi.slangh"
#include "scene.slangh"
#include "gbuffer.slangh"

#define NUM_THREADS_X 8
#define NUM_THREADS_Y 8

struct PushConstant
{
    int gbuffer_mip;
    float gi_intensity;
};

[[vk::binding(0, 2)]] ConstantBuffer<DDGIUniforms> DDGIBuffer;
[[vk::binding(1, 2)]] Texture2D<float4> Irradiance;
[[vk::binding(2, 2)]] Texture2D<float4> Depth;
[[vk::binding(3, 2)]] RWTexture2D<float4> Output;
[[vk::push_constant]] ConstantBuffer<PushConstant> push_constant;

static const float FLT_EPS = 0.00000001;

[shader("compute")]
[numthreads(NUM_THREADS_X, NUM_THREADS_Y, 1)]
void main(CSParam param)
{
    const int2 size = texture_size(GBufferA, push_constant.gbuffer_mip);
    const float2 texel_size = 1.0 / float2(size);
    const int2 current_coord = int2(param.DispatchThreadID.xy);
    const float2 pixel_center = float2(current_coord) + float2(0.5);
    const float2 tex_coord = pixel_center / float2(size);

    float depth = DepthBuffer.Load(int3(current_coord, push_constant.gbuffer_mip)).r;

    if (depth == 1.0f)
    {
        Output[current_coord] = float4(0.0);
        return;
    }

    const float3 P = world_position_from_depth(tex_coord, depth, ViewBuffer.view_projection_inv);
    const float3 N = octohedral_to_direction(GBufferB.Load(int3(current_coord, push_constant.gbuffer_mip)).rg);
    const float3 Wo = normalize(ViewBuffer.cam_pos.xyz - P);

    float3 irradiance = push_constant.gi_intensity * sample_irradiance(DDGIBuffer, P, N, Wo, Irradiance, Depth);

    Output[current_coord] = float4(irradiance, 1.0);
}
