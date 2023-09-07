#include "common.slangh"
#include "gbuffer.slangh"
#include "scene.slangh"
#define USE_EDGE_STOPPING_NORMAL_WEIGHT
#include "edge_stopping.slangh"

#define NUM_THREADS_X 8
#define NUM_THREADS_Y 8

struct PushConstant
{
    int gbuffer_mip;
};

[[vk::binding(0, 2)]] RWTexture2D<float4> Output_Image;
[[vk::binding(1, 2)]] Texture2D<float4> Input_Image;
[[vk::push_constant]] ConstantBuffer<PushConstant> push_constant;

static const float FLT_EPS = 0.00000001;

static const float2 kernel[4] = {
    float2(0.0f, 1.0f),
    float2(1.0f, 0.0f),
    float2(-1.0f, 0.0f),
    float2(0.0, -1.0f)
};

[shader("compute")]
[numthreads(NUM_THREADS_X, NUM_THREADS_Y, 1)]
void main(CSParam param)
{
    const int2 size = texture_size(GBufferA, 0);
    const float2 texel_size = float2(1.0f) / float2(texture_size(GBufferA, push_constant.gbuffer_mip));
    const int2 current_coord = int2(param.DispatchThreadID.xy);
    const float2 pixel_center = float2(current_coord) + float2(0.5);
    const float2 tex_coord = pixel_center / float2(size);

    float hi_res_depth = GBufferC.Load(int3(current_coord, 0)).a;

    if (hi_res_depth == -1.0f)
    {
        Output_Image[current_coord] = 0.0;
        return;
    }

    float3 hi_res_normal = octohedral_to_direction(GBufferB.Load(int3(current_coord, 0)).rg);

    float4 upsampled = float4(0.0f);
    float total_w = 0.0f;

    for (int i = 0; i < 4; i++)
    {
        float2 coarse_tex_coord = tex_coord + kernel[i] * texel_size;
        float coarse_depth = GBufferC.SampleLevel(Samplers[int(SamplerType::Linear)], coarse_tex_coord, push_constant.gbuffer_mip).a;

        if (coarse_depth == -1.0f)
            continue;

        float3 coarse_normal = octohedral_to_direction(GBufferB.SampleLevel(Samplers[int(SamplerType::Nearest)], coarse_tex_coord, push_constant.gbuffer_mip).rg);

        float w = compute_edge_stopping_weight(hi_res_depth,
                                               coarse_depth,
                                               1.0f,
                                               hi_res_normal,
                                               coarse_normal,
                                               32.0f);

        upsampled += Input_Image.SampleLevel(Samplers[int(SamplerType::Linear)], coarse_tex_coord, 0) * w;
        total_w += w;
    }

    upsampled = upsampled / max(total_w, FLT_EPS);
    Output_Image[current_coord] = upsampled;
}
