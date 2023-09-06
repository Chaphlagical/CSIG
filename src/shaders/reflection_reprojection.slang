#include "common.slangh"
#include "gbuffer.slangh"
#include "raytrace.slangh"
#include "scene.slangh"
#define REPROJECTION_REFLECTIONS
#define REPROJECTION_MOMENTS
#include "reproject.slangh"

#define NUM_THREADS_X 8
#define NUM_THREADS_Y 8

#define MIRROR_REFLECTIONS_ROUGHNESS_THRESHOLD 0.05f
#define DDGI_REFLECTIONS_ROUGHNESS_THRESHOLD 0.75f

struct PushConstant
{
    int gbuffer_mip;
    uint approximate_with_ddgi;
    float alpha;
    float moments_alpha;
};

[[vk::binding(0, 2)]] RWTexture2D<float4> Output_Image;
[[vk::binding(1, 2)]] RWTexture2D<float4> Moment_Image;
[[vk::binding(2, 2)]] Texture2D<float4> Input_Image;
[[vk::binding(3, 2)]] Texture2D<float4> Prev_Output_Image;
[[vk::binding(4, 2)]] Texture2D<float4> Prev_Moment_Image;
[[vk::binding(5, 2)]] RWStructuredBuffer<int2> DenoiseTileData;
[[vk::binding(6, 2)]] RWStructuredBuffer<uint3> DenoiseTileDispatchArgs;
[[vk::binding(7, 2)]] RWStructuredBuffer<int2> CopyTileData;
[[vk::binding(8, 2)]] RWStructuredBuffer<uint3> CopyTileDispatchArgs;
[[vk::push_constant]] ConstantBuffer<PushConstant> push_constant;

groupshared uint shared_should_denoise;

float3 clip_aabb(float3 aabb_min, float3 aabb_max, float3 history_sample)
{
    float3 aabb_center = 0.5f * (aabb_max + aabb_min);
    float3 extent_clip = 0.5f * (aabb_max - aabb_min) + 0.001f;

    float3 color_vector = history_sample - aabb_center;
    float3 color_vector_clip = color_vector / extent_clip;
    color_vector_clip = abs(color_vector_clip);
    float max_abs_unit = max(max(color_vector_clip.x, color_vector_clip.y), color_vector_clip.z);

    if (max_abs_unit > 1.0)
        return aabb_center + color_vector / max_abs_unit;
    else
        return history_sample;
}

void neighborhood_standard_deviation(int2 coord, out float3 mean, out float3 std_dev)
{
    float3 m1 = float3(0.0f);
    float3 m2 = float3(0.0f);

    int radius = 8;
    float weight = (float(radius) * 2.0f + 1.0f) * (float(radius) * 2.0f + 1.0f);

    for (int dx = -radius; dx <= radius; dx++)
    {
        for (int dy = -radius; dy <= radius; dy++)
        {
            int2 sample_coord = coord + int2(dx, dy);
            float3 sample_color = Input_Image.Load(int3(sample_coord, 0)).rgb;

            m1 += sample_color;
            m2 += sample_color * sample_color;
        }
    }

    mean = m1 / weight;
    float3 variance = (m2 / weight) - (mean * mean);

    std_dev = sqrt(max(variance, 0.0f));
}

float compute_max_accumulated_frame(float history_length)
{
    if (length(ViewBuffer.cam_pos.xyz - ViewBuffer.prev_cam_pos.xyz) > 0.0f)
        return 8.0f;
    else
        return history_length;
}

[shader("compute")]
[numthreads(NUM_THREADS_X, NUM_THREADS_Y, 1)]
void main(CSParam param)
{
    shared_should_denoise = 0;

    if (param.DispatchThreadID.x == 0 && param.DispatchThreadID.y == 0)
    {
        DenoiseTileDispatchArgs[0].x = 0;
        DenoiseTileDispatchArgs[0].y = 1;
        DenoiseTileDispatchArgs[0].z = 1;
        CopyTileDispatchArgs[0].x = 0;
        CopyTileDispatchArgs[0].y = 1;
        CopyTileDispatchArgs[0].z = 1;
    }

    AllMemoryBarrierWithGroupSync();

    const int2 size = texture_size(Prev_Output_Image, 0);
    const int2 current_coord = int2(param.DispatchThreadID.xy);
    const float2 pixel_center = float2(current_coord) + float2(0.5);
    const float2 tex_coord = pixel_center / float2(size);

    const float depth = DepthBuffer.Load(int3(current_coord, push_constant.gbuffer_mip)).r;
    const float roughness = GBufferC.Load(int3(current_coord, push_constant.gbuffer_mip)).r;

    float4 output_radiance = float4(0.0f);
    float4 output_moments = float4(0.0f);

    if (depth != 1.0f)
    {
        float4 color_ray_length = Input_Image.Load(int3(current_coord, 0));
        float3 color = color_ray_length.rgb;
        const float ray_length = color_ray_length.a;

        float3 history_color;
        float2 history_moments;
        float history_length;

        bool success = reproject(current_coord,
                                 depth,
                                 push_constant.gbuffer_mip,
                                 ViewBuffer.cam_pos.xyz,
                                 ViewBuffer.view_projection_inv,
                                 ViewBuffer.prev_view_projection,
                                 ray_length,
                                 GBufferB,
                                 GBufferC,
                                 PrevGBufferB,
                                 PrevGBufferC,
                                 PrevDepthBuffer,
                                 Prev_Output_Image,
                                 Prev_Moment_Image,
                                 history_color,
                                 history_moments,
                                 history_length);

        history_length = min(32.0f, success ? history_length + 1.0f : 1.0f);

        if (success)
        {
            float3 std_dev;
            float3 mean;

            neighborhood_standard_deviation(int2(param.DispatchThreadID.xy), mean, std_dev);

            float3 radiance_min = mean - std_dev;
            float3 radiance_max = mean + std_dev;

            history_color.xyz = clip_aabb(radiance_min, radiance_max, history_color.xyz);
        }

        const float max_accumulated_frame = compute_max_accumulated_frame(history_length);
        const float alpha = success ? max(push_constant.alpha, 1.0 / max_accumulated_frame) : 1.0;
        const float alpha_moments = success ? max(push_constant.moments_alpha, 1.0 / max_accumulated_frame) : 1.0;

        float2 moments = float2(0.0f);
        moments.r = luminance(color);
        moments.g = moments.r * moments.r;
        moments = lerp(history_moments, moments, alpha_moments);
        float variance = max(0.0f, moments.g - moments.r * moments.r);
        float3 accumulated_color = lerp(history_color, color, alpha);
        output_moments = float4(moments, history_length, 0.0f);
        output_radiance = float4(accumulated_color, variance);
    }

    Moment_Image[int2(param.DispatchThreadID.xy)] = output_moments;
    Output_Image[int2(param.DispatchThreadID.xy)] = output_radiance;

    if (depth != 1.0f && roughness >= MIRROR_REFLECTIONS_ROUGHNESS_THRESHOLD)
    {
        if (push_constant.approximate_with_ddgi == 1)
        {
            if (roughness <= DDGI_REFLECTIONS_ROUGHNESS_THRESHOLD)
                shared_should_denoise = 1;
        }
        else
            shared_should_denoise = 1;
    }

    GroupMemoryBarrierWithGroupSync();

    if (param.GroupIndex == 0)
    {
        uint idx = 0;
        if (shared_should_denoise == 1)
        {
            InterlockedAdd(DenoiseTileDispatchArgs[0][0], 1, idx);
            DenoiseTileData[idx] = current_coord;
        }
        else
        {
            InterlockedAdd(CopyTileDispatchArgs[0][0], 1, idx);
            CopyTileData[idx] = current_coord;
        }
    }
}