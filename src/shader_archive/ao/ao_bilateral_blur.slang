#include "common.slangh"
#include "scene.slangh"
#include "gbuffer.slangh"
#define USE_EDGE_STOPPING_NORMAL_WEIGHT
#include "edge_stopping.slangh"

#define NUM_THREADS_X 8
#define NUM_THREADS_Y 8
#define CACHE_SIZE_X NUM_THREADS_X * 2
#define CACHE_SIZE_Y NUM_THREADS_Y * 2
#define GAUSS_BLUR_DEVIATION 1.5

struct PushConstant
{
    float4 z_buffer_params;
    int2 direction;
    int radius;
    int gbuffer_mip;
};

[[vk::binding(0, 2)]] RWTexture2D<float> BilateralImage;
[[vk::binding(1, 2)]] Texture2D<float> AOImage;
[[vk::binding(2, 2)]] Texture2D<float> HistoryLength;
[[vk::binding(3, 2)]] StructuredBuffer<int2> DenoiseTileBuffer;
[[vk::push_constant]] ConstantBuffer<PushConstant> push_constant;

float linear_eye_depth(float z, float4 z_buffer_params)
{
    return 1.0 / (z_buffer_params.z * z + z_buffer_params.w);
}

float gaussian_weight(float offset, float deviation)
{
    float weight = 1.0 / sqrt(2.0 * PI * deviation * deviation);
    weight *= exp(-(offset * offset) / (2.0 * deviation * deviation));
    return weight;
}

float bilateral_blur(int2 coord)
{
    const float deviation = float(push_constant.radius) / GAUSS_BLUR_DEVIATION;
    
    float total_ao = AOImage.Load(int3(coord, 0)).r;
    float total_weight = 1.0;

    float center_depth = linear_eye_depth(DepthBuffer.Load(int3(coord, push_constant.gbuffer_mip)).r, push_constant.z_buffer_params);
    float3 center_normal = octohedral_to_direction(GBufferB.Load(int3(coord, push_constant.gbuffer_mip)).rg);

    int radius = push_constant.radius;

    for(int i = -radius; i <= radius; i++)
    {
        if (i == 0)
        {
            continue;
        }

        int2 sample_coord = coord + push_constant.direction * int2(i);
        float sample_depth = linear_eye_depth(DepthBuffer.Load(int3(sample_coord, push_constant.gbuffer_mip)).r, push_constant.z_buffer_params);
        float sample_ao = AOImage.Load(int3(sample_coord, 0)).r;
        float3 sample_normal = octohedral_to_direction(GBufferB.Load(int3(sample_coord, push_constant.gbuffer_mip)).rg);
    
        float weight = gaussian_weight(float(i), deviation) *
                        compute_edge_stopping_weight(
                            center_depth,
                            sample_depth,
                            1.0,
                            center_normal,
                            sample_normal,
                            32.0);

        total_ao += weight * sample_ao;
        total_weight += weight;
    }

    return total_ao / max(total_weight, 0.0001);
}

[shader("compute")]
[numthreads(NUM_THREADS_X, NUM_THREADS_Y, 1)]
void main(CSParam param)
{
    int2 size = texture_size(GBufferB, push_constant.gbuffer_mip);
    int2 coord = DenoiseTileBuffer[param.GroupID.x] + int2(param.GroupThreadID.xy);
    if(coord.x >= size.x || coord.y >= size.y)
    {
        return;
    }
    float depth = DepthBuffer.Load(int3(coord, push_constant.gbuffer_mip)).r;
    if(depth == 1.0)
    {
        BilateralImage[coord] = 1.0;
        return;
    }
    float ao = bilateral_blur(coord);
    BilateralImage[coord] = ao;
}