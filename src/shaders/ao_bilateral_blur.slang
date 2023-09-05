#include "common.slangh"
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

[[vk::binding(0, 2)]] RWTexture2D<float> Bilateral_Image;
[[vk::binding(1, 2)]] Texture2D<uint> AO_Image;
[[vk::binding(2, 2)]] Texture2D<float> History_Length;
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

float bilateral_blur(int2 current_coord)
{
    const float deviation = float(push_constant.radius) / GAUSS_BLUR_DEVIATION;

    float total_ao = AO_Image.Load(int3(current_coord, 0)).r;
    float total_weight = 1.0f;

    float center_depth = linear_eye_depth(DepthBuffer.Load(int3(current_coord, push_constant.gbuffer_mip)).r, push_constant.z_buffer_params);
    float3 center_normal = octohedral_to_direction(GBufferB.Load(int3(current_coord, push_constant.gbuffer_mip)).rg);

    int radius = push_constant.radius;

    for (int i = -radius; i <= radius; i++)
    {
        if (i == 0)
            continue;

        int2 sample_coord = current_coord + push_constant.direction * int2(i);
        float sample_depth = linear_eye_depth(DepthBuffer.Load(int3(sample_coord, push_constant.gbuffer_mip)).r, push_constant.z_buffer_params);
        float sample_ao = AO_Image.Load(int3(sample_coord, 0)).r;
        float3 sample_normal = octohedral_to_direction(GBufferB.Load(int3(sample_coord, push_constant.gbuffer_mip)).rg);

        float weight = gaussian_weight(float(i), deviation);

        weight *= compute_edge_stopping_weight(center_depth,
                                               sample_depth,
                                               1.0f,
                                               center_normal,
                                               sample_normal,
                                               32.0f);

        total_ao += weight * sample_ao;
        total_weight += weight;
    }

    return total_ao / max(total_weight, 0.0001f);
}

[shader("compute")]
[numthreads(NUM_THREADS_X, NUM_THREADS_Y, 1)]
void main(CSParam param)
{
    // Query the G-Buffer image size
    int2 size = texture_size(GBufferA, push_constant.gbuffer_mip);

    // Compute current pixel coord
    int2 current_coord = DenoiseTileBuffer[param.GroupID.x] + int2(param.GroupThreadID.xy);

    // Skip pixel if it belongs to the background
    float depth = DepthBuffer.Load(int3(current_coord, push_constant.gbuffer_mip)).r;

    if (depth == 1.0f)
    {
        Bilateral_Image[current_coord] = 1.0;
        return;
    }

    // Blur
    float ao = bilateral_blur(current_coord);

    // Store blurred ao
    Bilateral_Image[current_coord] = ao;
}