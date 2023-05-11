#version 450

#extension GL_KHR_vulkan_glsl : enable
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_scalar_block_layout : enable
#extension GL_EXT_shader_explicit_arithmetic_types : enable

#include "ddgi.glsl"

layout(location = 0) in vec4 inPosition;
layout(location = 1) in vec4 inNormal;

layout(location = 0) out vec3 outFragPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out flat int outInstanceID;

layout(binding = 0, scalar) uniform UBO
{
    GlobalData ubo;
};

layout(binding = 1, scalar) uniform DDGIUBO
{
    DDGIUniform ddgi_buffer;
};

void main()
{
    ivec3 grid_coord = probe_index_to_grid_coord(gl_InstanceIndex, ddgi_buffer);
    vec3 probe_position = grid_coord_to_position(grid_coord, ddgi_buffer);
    gl_Position = ubo.view_projection * vec4(inPosition.xyz + probe_position, 1.0);
    outNormal = inNormal.xyz;
    outFragPos = (ubo.view_projection * vec4(inPosition.xyz + probe_position, 1.0)).xyz;
    outInstanceID = gl_InstanceIndex;
}