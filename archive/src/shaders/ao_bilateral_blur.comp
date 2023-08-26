#version 460

#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_ray_query : enable
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_scalar_block_layout : enable
#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_buffer_reference : enable
#extension GL_EXT_buffer_reference2 : enable

#define USE_EDGE_STOPPING_NORMAL_WEIGHT
#include "gbuffer_data.glsl"
#include "edge_stopping.glsl"

#define NUM_THREADS_X 8
#define NUM_THREADS_Y 8
#define CACHE_SIZE_X NUM_THREADS_X * 2
#define CACHE_SIZE_Y NUM_THREADS_Y * 2
#define GAUSS_BLUR_DEVIATION 1.5

layout(local_size_x = NUM_THREADS_X, local_size_y = NUM_THREADS_Y, local_size_z = 1) in;

layout(set = 2, binding = 0, r16f) uniform image2D bilateral_image;
layout(set = 2, binding = 1) uniform sampler2D ao_image;
layout(set = 2, binding = 2) uniform sampler2D history_length;
layout(set = 2, binding = 3, std430) buffer DenoiseTileBuffer
{
    ivec2 coord[];
}denoise_tile_buffer;

layout(push_constant) uniform PushConstants
{
    vec4 z_buffer_params;
    ivec2 direction;
    int radius;
    int gbuffer_mip;
}push_constants;

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
    vec3 center_normal = octohedral_to_direction(texelFetch(gbufferB, coord, push_constants.gbuffer_mip).rg);

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
        vec3 sample_normal = octohedral_to_direction(texelFetch(gbufferB, sample_coord, push_constants.gbuffer_mip).rg);
    
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