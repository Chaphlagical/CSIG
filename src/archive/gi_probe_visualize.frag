#version 450

#extension GL_KHR_vulkan_glsl : enable
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_scalar_block_layout : enable
#extension GL_EXT_shader_explicit_arithmetic_types : enable

#include "ddgi.glsl"

layout(location = 0) in vec3 inFragPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in flat int inInstanceID;

layout(location = 0) out vec4 outFragColor;

layout(binding = 2) uniform sampler2D probe_irradiance;
layout(binding = 3) uniform sampler2D probe_depth;

layout(binding = 1, scalar) uniform DDGIUBO
{
    DDGIUniform ddgi_buffer;
};

void main()
{
    vec2 probe_coord = texture_coord_from_direction(normalize(inNormal),
                                                    inInstanceID,
                                                    ddgi_buffer.irradiance_texture_width,
                                                    ddgi_buffer.irradiance_texture_height,
                                                    ddgi_buffer.irradiance_probe_side_length);
    outFragColor = vec4(textureLod(probe_irradiance, probe_coord, 0.0).rgb, 1.0);
}