#include "scene.slangh"
#include "gbuffer.slangh"
#define REPROJECTION_SINGLE_COLOR_CHANNEL
#include "reproject.slangh"

#define NUM_THREAD_X 8
#define NUM_THREAD_Y 8
#define RAY_MASK_SIZE_X 8
#define RAY_MASK_SIZE_Y 4

struct PushConstant
{
    float alpha;
    int gbuffer_mip;
    uint debug;
};

[[vk::binding(0, 2)]] Texture2D<uint> RayTracedImage;
[[vk::binding(1, 2)]] RWTexture2D<float> AOImage;
[[vk::binding(2, 2)]] RWTexture2D<float> HistoryLengthImage;
[[vk::binding(3, 2)]] Texture2D<float> PrevAOImage;
[[vk::binding(4, 2)]] Texture2D<float> PrevHistoryLengthImage;
[[vk::binding(5, 2)]] RWStructuredBuffer<int2> DenoiseTileBuffer;
[[vk::binding(6, 2)]] RWStructuredBuffer<DispatchIndirectCommand> DenoiseTileDispatchArgsBuffer;
[[vk::push_constant]] ConstantBuffer<PushConstant> push_constant;

groupshared uint shared_ao_masks[3][6];
groupshared float shared_mean[8][24];
groupshared bool shared_denoise;

void populate_cache(CSParam param)
{
    shared_denoise = false;

    if (param.GroupThreadID.x < 3 && param.GroupThreadID.y < 6)
    {
        const int2 coord = int2(param.GroupID.x, param.GroupID.y * 2) - int2(1, 2) + int2(param.GroupThreadID.xy);
        const int2 image_dim = texture_size(RayTracedImage, 0);
        if (any(coord < int2(0, 0)) || any(coord > image_dim - int2(1, 1)))
        {
            shared_ao_masks[param.GroupThreadID.x][param.GroupThreadID.y] = 0xFFFFFFFF;
        }
        else
        {
            shared_ao_masks[param.GroupThreadID.x][param.GroupThreadID.y] = RayTracedImage.Load(int3(coord, 0)).x;
        }
    }

    GroupMemoryBarrierWithGroupSync();
}

float unpack_ao_hit_value(CSParam param, int2 coord)
{
    const int2 work_group_start_coord = int2(param.GroupID.xy) * int2(NUM_THREAD_X, NUM_THREAD_Y);
    const int2 cache_start_coord = work_group_start_coord - int2(RAY_MASK_SIZE_X, RAY_MASK_SIZE_Y * 2);
    const int2 unpacked_cache_coord = coord - cache_start_coord;
    const int2 packed_cache_coord = unpacked_cache_coord / int2(RAY_MASK_SIZE_X, RAY_MASK_SIZE_Y);
    const int2 mask_start_coord = packed_cache_coord * int2(RAY_MASK_SIZE_X, RAY_MASK_SIZE_Y);
    const int2 relative_mask_coord = unpacked_cache_coord - mask_start_coord;
    const int hit_index = relative_mask_coord.y * RAY_MASK_SIZE_X + relative_mask_coord.x;
    return float((shared_ao_masks[packed_cache_coord.x][packed_cache_coord.y] >> hit_index) & 1u);
}

float horizontal_neighborhood_mean(CSParam param, int2 coord)
{
    float result = 0.0f;

    for (int x = -8; x <= 8; x++)
        result += unpack_ao_hit_value(param, int2(coord.x + x, coord.y));

    return result;
}

float neighborhood_mean(CSParam param, int2 coord)
{
    float top = horizontal_neighborhood_mean(param, int2(coord.x, coord.y - 8));
    float middle = horizontal_neighborhood_mean(param, int2(coord.x, coord.y));
    float bottom = horizontal_neighborhood_mean(param, int2(coord.x, coord.y + 8));

    shared_mean[param.GroupThreadID.x][param.GroupThreadID.y] = top;
    shared_mean[param.GroupThreadID.x][param.GroupThreadID.y + 8] = middle;
    shared_mean[param.GroupThreadID.x][param.GroupThreadID.y + 16] = bottom;

    GroupMemoryBarrierWithGroupSync();

    const int radius = 8;
    const float weight = (float(radius) * 2.0f + 1.0f) * (float(radius) * 2.0f + 1.0f);

    float mean = 0.0f;

    for (int y = 0; y <= 16; y++)
    {
        mean += shared_mean[param.GroupThreadID.x][param.GroupThreadID.y + y];
    }
    
	return mean / weight;
}

[shader("compute")]
[numthreads(NUM_THREAD_X, NUM_THREAD_Y, 1)]
void main(CSParam param)
{
    if (param.DispatchThreadID.x == 0 && param.DispatchThreadID.y == 0)
    {
        DenoiseTileDispatchArgsBuffer[0].x = 0;
        DenoiseTileDispatchArgsBuffer[0].y = 1;
        DenoiseTileDispatchArgsBuffer[0].z = 1;
    }

    GroupMemoryBarrierWithGroupSync();

    const int2 size = texture_size(PrevAOImage, 0);
    const int2 current_coord = int2(param.DispatchThreadID.xy);

    populate_cache(param);
    float mean = neighborhood_mean(param, current_coord);

    float depth = DepthBuffer.Load(int3(current_coord, push_constant.gbuffer_mip)).r;

    float out_ao = 1.0f;
    float history_length = 0.0f;

    if (depth != 1.0f)
    {
        float ao = unpack_ao_hit_value(param, current_coord);
        float history_ao;
        bool success = reproject(current_coord,
                                 depth,
                                 push_constant.gbuffer_mip,
                                 ViewBuffer.view_projection_inv,
                                 GBufferB,
                                 GBufferC,
                                 PrevGBufferB,
                                 PrevGBufferC,
                                 PrevDepthBuffer,
                                 PrevAOImage,
                                 PrevHistoryLengthImage,
                                 history_ao,
                                 history_length);

        history_length = min(32.0, success ? history_length + 1.0f : 1.0f);

        if (success)
        {
            float spatial_variance = mean;
            spatial_variance = max(spatial_variance - mean * mean, 0.0f);

            const float std_deviation = sqrt(spatial_variance);
            const float nmin = mean - 0.5f * std_deviation;
            const float nmax = mean + 0.5f * std_deviation;

            history_ao = clamp(history_ao, nmin, nmax);
        }

        const float alpha = success ? max(push_constant.alpha, 1.0 / history_length) : 1.0;

        out_ao = lerp(history_ao, ao, alpha);
    }

    AOImage[current_coord] = out_ao;
    HistoryLengthImage[current_coord] = history_length;

    if (out_ao < 1.0f)
    {
        shared_denoise = true;
    }
    
	GroupMemoryBarrierWithGroupSync();

    if (shared_denoise == 1 && param.GroupIndex == 0)
    {
        uint idx = 0;
        InterlockedAdd(DenoiseTileDispatchArgsBuffer[0].x, 1, idx);
        DenoiseTileBuffer[idx] = current_coord;
    }
}