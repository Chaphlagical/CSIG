#include "scene.slangh"
#include "gbuffer.slangh"
#define USE_EDGE_STOPPING_NORMAL_WEIGHT
#include "edge_stopping.slangh"

#define NUM_THREADS_X 8
#define NUM_THREADS_Y 8
#define CACHE_SIZE_X NUM_THREADS_X * 2
#define CACHE_SIZE_Y NUM_THREADS_Y * 2
#define GAUSS_BLUR_DEVIATION 1.5




float linear_eye_depth(float z, vec4 z_buffer_params)
{
    return 1.0 / (z_buffer_params.z * z + z_buffer_params.w);
}

float gaussian_weight(float offset, float deviation)
{
    float weight = 1.0 / sqrt(2.0 * PI * deviation * deviation);
    weight *= exp(-(offset * offset) / (2.0 * deviation * deviation));
    return weight;
}

float bilateral_blur(ivec2 coord)
{
    const float deviation = float(push_constants.radius) / GAUSS_BLUR_DEVIATION;
    
    float total_ao = texelFetch(ao_image, coord, 0).r;
    float total_weight = 1.0;

    float center_depth = linear_eye_depth(texelFetch(depth_buffer, coord, push_constants.gbuffer_mip).r, push_constants.z_buffer_params);
    float3 center_normal = octohedral_to_direction(texelFetch(gbufferB, coord, push_constants.gbuffer_mip).rg);

    int radius = push_constants.radius;

    for(int i = -radius; i <= radius; i++)
    {
        if (i == 0)
        {
            continue;
        }

        ivec2 sample_coord = coord + push_constants.direction * ivec2(i);
        float sample_depth = linear_eye_depth(texelFetch(depth_buffer, sample_coord, push_constants.gbuffer_mip).r, push_constants.z_buffer_params);
        float sample_ao = texelFetch(ao_image, sample_coord, 0).r;
        float3 sample_normal = octohedral_to_direction(texelFetch(gbufferB, sample_coord, push_constants.gbuffer_mip).rg);
    
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

void main()
{
    ivec2 size = textureSize(gbufferB, push_constants.gbuffer_mip);
    ivec2 coord = denoise_tile_buffer.coord[gl_WorkGroupID.x] + ivec2(gl_LocalInvocationID.xy);
    if(coord.x >= size.x || coord.y >= size.y)
    {
        return;
    }
    float depth = texelFetch(depth_buffer, coord, push_constants.gbuffer_mip).r;
    if(depth == 1.0)
    {
        imageStore(bilateral_image, coord, vec4(1.0));
        return;
    }
    float ao = bilateral_blur(coord);
    imageStore(bilateral_image, coord, vec4(ao));
}