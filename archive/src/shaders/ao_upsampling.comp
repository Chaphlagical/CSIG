#version 460

#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_ray_query : enable
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_scalar_block_layout : enable
#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_buffer_reference : enable
#extension GL_EXT_buffer_reference2 : enable

#include "gbuffer_data.glsl"
#include "scene.glsl"
#define USE_EDGE_STOPPING_NORMAL_WEIGHT
#include "edge_stopping.glsl"

#define NUM_THREADS_X 8
#define NUM_THREADS_Y 8

layout(local_size_x = NUM_THREADS_X, local_size_y = NUM_THREADS_Y, local_size_z = 1) in;

layout(set = 2, binding = 0, r16f) uniform image2D upsample_image;
layout(set = 2, binding = 1) uniform sampler2D bilateral_image;

layout(push_constant) uniform PushConstants
{
    int gbuffer_mip;
    float power;
    uint debug;
}push_constants;

const float FLT_EPS = 0.00000001;
const vec2 kernal[4] = {
    vec2(0.0, 1.0),
    vec2(1.0, 0.0),
    vec2(-1.0, 0.0),
    vec2(0.0, -1.0),
};

void main()
{
    const ivec2 size = textureSize(gbufferB, 0);
    const vec2 texel_size = vec2(1.0) / vec2(textureSize(gbufferB, push_constants.gbuffer_mip));
    const ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    const vec2 pixel_center = vec2(coord) + vec2(0.5);
    const vec2 tex_coord = pixel_center / vec2(size);

    if(coord.x >= size.x || coord.y >= size.y)
    {
        return;
    }

    float full_depth = texelFetch(gbufferC, coord, 0).a;

    if(push_constants.debug == 1)
    {
        imageStore(upsample_image, coord, texelFetch(bilateral_image, coord/2, 0));
        return;
    }


    if(full_depth == -1.0)
    {
        imageStore(upsample_image, coord, vec4(1.0));
        return;
    }

    vec3 full_normal = octohedral_to_direction(texelFetch(gbufferB, coord, 0).rg);

    float upsampled = 0.0;
    float total_w = 0.0;

    for(int i = 0; i < 4; i++)
    {
        vec2 coarse_tex_coord = tex_coord + kernal[i] * texel_size;
        float coarse_depth = textureLod(gbufferC, coarse_tex_coord, push_constants.gbuffer_mip).a;
        if(coarse_depth == -1.0)
        {
            continue;
        }
        vec3 coarse_normal = octohedral_to_direction(textureLod(gbufferB, coarse_tex_coord, push_constants.gbuffer_mip).rg);
        float w = compute_edge_stopping_weight(full_depth,
                                               coarse_depth,
                                               1.0,
                                               full_normal,
                                               coarse_normal,
                                               32.0);
        upsampled += textureLod(bilateral_image, coarse_tex_coord, 0).r * w;
        total_w += w;
    }

    upsampled = upsampled / max(total_w, FLT_EPS);
    upsampled = pow(upsampled, push_constants.power);

    imageStore(upsample_image, coord, vec4(upsampled));
}