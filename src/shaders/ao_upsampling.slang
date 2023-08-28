#include "gbuffer.slangh"
#include "scene.slangh"
#include "common.slangh"
#define USE_EDGE_STOPPING_NORMAL_WEIGHT
#include "edge_stopping.slangh"

#define NUM_THREADS_X 8
#define NUM_THREADS_Y 8

struct PushConstant
{
    int gbuffer_mip;
    float power;
    uint debug;
};

[[vk::binding(0, 2)]] RWTexture2D<float> UpsampleImage;
[[vk::binding(1, 2)]] Texture2D<float> BilateralImage;
[[vk::push_constant]] ConstantBuffer<PushConstant> push_constant;

static const float FLT_EPS = 0.00000001;
static const float2 kernal[4] = {
    float2(0.0, 1.0),
    float2(1.0, 0.0),
    float2(-1.0, 0.0),
    float2(0.0, -1.0),
};

[shader("compute")]
[numthreads(NUM_THREADS_X, NUM_THREADS_Y, 1)]
void main(CSParam param)
{
    const int2 size = texture_size(GBufferB, 0);
    const float2 texel_size = float2(1.0) / float2(texture_size(GBufferB, push_constant.gbuffer_mip));
    const int2 coord = int2(param.DispatchThreadID.xy);
    const float2 pixel_center = float2(coord) + float2(0.5);
    const float2 tex_coord = pixel_center / float2(size);

    if(coord.x >= size.x || coord.y >= size.y)
    {
        return;
    }

    float full_depth = GBufferC.Load(int3(coord, 0)).a;

    if(push_constant.debug == 1)
    {
        UpsampleImage[coord] = BilateralImage.Load(int3(coord/2, 0));
        return;
    }


    if(full_depth == -1.0)
    {
        UpsampleImage[coord] = 1.0;
        return;
    }

    float3 full_normal = octohedral_to_direction(GBufferB.Load(int3(coord, 0)).rg);

    float upsampled = 0.0;
    float total_w = 0.0;

    SamplerState sampler = Samplers[int(SamplerType::PointWarp)];

    for(int i = 0; i < 4; i++)
    {
        float2 coarse_tex_coord = tex_coord + kernal[i] * texel_size;
        float coarse_depth = GBufferC.SampleLevel(sampler, coarse_tex_coord, push_constant.gbuffer_mip).a;
        if(coarse_depth == -1.0)
        {
            continue;
        }
        float3 coarse_normal = octohedral_to_direction(GBufferB.SampleLevel(sampler, coarse_tex_coord, push_constant.gbuffer_mip).rg);
        float w = compute_edge_stopping_weight(full_depth,
                                               coarse_depth,
                                               1.0,
                                               full_normal,
                                               coarse_normal,
                                               32.0);
        upsampled += BilateralImage.SampleLevel(sampler, coarse_tex_coord, 0).r * w;
        total_w += w;
    }

    upsampled = upsampled / max(total_w, FLT_EPS);
    upsampled = pow(upsampled, push_constant.power);

    UpsampleImage[coord] = upsampled;
}