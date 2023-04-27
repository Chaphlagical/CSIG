#version 450

#extension GL_KHR_vulkan_glsl : enable
#extension GL_GOOGLE_include_directive : enable

#include "common.glsl"

layout(binding = 0) uniform sampler2D input_texture;

layout(location = 0) in vec3 inFragPos;
layout(location = 0) out vec4 outColor;

vec2 sample_spherical_map(vec3 v)
{
    vec2 uv = vec2(atan(v.x, v.z), asin(v.y));
    uv.x /= 2 * PI;
    uv.y /= PI;
    uv += 0.5;
    uv.y = 1.0 - uv.y;
    return uv;
}

void main()
{
	vec2 uv = sample_spherical_map(normalize(inFragPos));
    outColor = vec4(texture(input_texture, uv).rgb, 1.0);
}