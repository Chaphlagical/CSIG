#include "common.slangh"
#define USE_EDGE_STOPPING_NORMAL_WEIGHT
#define USE_EDGE_STOPPING_LUMA_WEIGHT
#include "edge_stopping.slangh"
#include "scene.slangh"
#include "gbuffer.slangh"

#define NUM_THREADS_X 8
#define NUM_THREADS_Y 8

#define MIRROR_REFLECTIONS_ROUGHNESS_THRESHOLD 0.05f
#define DDGI_REFLECTIONS_ROUGHNESS_THRESHOLD 0.75f

struct PushConstant
{
    int radius;
    int step_size;
    float phi_color;
    float phi_normal;
    float sigma_depth;
    int gbuffer_mip;
    int approximate_with_ddgi;
};

[[vk::binding(0, 2)]] RWTexture2D<float4> Output_Image;
[[vk::binding(1, 2)]] Texture2D<float4> Input_Image;
[[vk::binding(2, 2)]] RWStructuredBuffer<int2> DenoiseTileData;
[[vk::push_constant]] ConstantBuffer<PushConstant> push_constant;

float compute_variance_center(int2 ipos)
{
    float sum = 0.0f;

    const float kernel[2][2] = {
        { 1.0 / 4.0, 1.0 / 8.0 },
        { 1.0 / 8.0, 1.0 / 16.0 }
    };

    const int radius = 1;
    for (int yy = -radius; yy <= radius; yy++)
    {
        for (int xx = -radius; xx <= radius; xx++)
        {
            int2 p = ipos + int2(xx, yy);
            float k = kernel[abs(xx)][abs(yy)];
            sum += Input_Image.Load(int3(p, 0)).a * k;
        }
    }

    return sum;
}

[shader("compute")]
[numthreads(NUM_THREADS_X, NUM_THREADS_Y, 1)]
void main(CSParam param)
{
    int2 size = texture_size(GBufferA, push_constant.gbuffer_mip);
    const int2 ipos = DenoiseTileData[param.GroupID.x] + int2(param.GroupThreadID.xy);

    const float eps_variance = 1e-10;
    const float kernel_weights[3] = { 1.0, 2.0 / 3.0, 1.0 / 6.0 };

    const float4 color_center = Input_Image.Load(int3(ipos, 0));
    const float center_color_luma = luminance(color_center.rgb);

    const float var = compute_variance_center(ipos);

    float4 center_g_buffer_2 = GBufferB.Load(int3(ipos, push_constant.gbuffer_mip));
    float4 center_g_buffer_3 = GBufferC.Load(int3(ipos, push_constant.gbuffer_mip));

    float3 current_normal = octohedral_to_direction(center_g_buffer_2.xy);
    float center_depth = center_g_buffer_3.w;

    const float depth = DepthBuffer.Load(int3(ipos, push_constant.gbuffer_mip)).r;
    const float roughness = GBufferC.Load(int3(ipos, push_constant.gbuffer_mip)).r;

    if (depth == 1.0f)
    {
        Output_Image[ipos] = 0.0;
        return;
    }
    else if ((roughness < MIRROR_REFLECTIONS_ROUGHNESS_THRESHOLD) || (push_constant.approximate_with_ddgi == 1 && (roughness > DDGI_REFLECTIONS_ROUGHNESS_THRESHOLD)))
    {
        Output_Image[ipos] = color_center;
        Output_Image[ipos] = color_center;
        return;
    }

    const float phi_color = push_constant.phi_color * sqrt(max(0.0, eps_variance + var.r));
    float sum_w_color = 1.0;
    float4 sum_color = color_center;

    for (int yy = -push_constant.radius; yy <= push_constant.radius; yy++)
    {
        for (int xx = -push_constant.radius; xx <= push_constant.radius; xx++)
        {
            const int2 p = ipos + int2(xx, yy) * push_constant.step_size;
            const bool inside = all(p >= int2(0, 0)) && all(p >= size);
            const float kernel = kernel_weights[abs(xx)] * kernel_weights[abs(yy)];

            if (inside && (xx != 0 || yy != 0))
            {
                const float4 sample_color = Input_Image.Load(int3(p, 0));
                const float sample_color_luma = luminance(sample_color.rgb);

                float4 sample_g_buffer_2 = GBufferB.Load(int3(p, push_constant.gbuffer_mip));
                float4 sample_g_buffer_3 = GBufferC.Load(int3(p, push_constant.gbuffer_mip));

                float3 sample_normal = octohedral_to_direction(sample_g_buffer_2.xy);
                float sample_depth = sample_g_buffer_3.w;

                const float w = compute_edge_stopping_weight(center_depth,
                                                             sample_depth,
                                                             push_constant.sigma_depth,
                                                             current_normal,
                                                             sample_normal,
                                                             push_constant.phi_normal,
                                                             center_color_luma,
                                                             sample_color_luma,
                                                             phi_color);

                const float w_color = w * kernel;

                sum_w_color += w_color;
                sum_color += float4(float3(w_color), w_color * w_color) * sample_color;
            }
        }
    }

    float4 out_color = sum_color / float4(float3(sum_w_color), sum_w_color * sum_w_color);
    Output_Image[ipos] = out_color;
}