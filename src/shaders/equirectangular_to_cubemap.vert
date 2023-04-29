#version 450

#extension GL_KHR_vulkan_glsl : enable
#extension GL_GOOGLE_include_directive : enable
#extension  GL_ARB_shader_viewport_layer_array : enable

#include "common.glsl"

layout(location = 0) out vec3 outFragPos;

const mat4 inv_view_projections[6] = 
{
    mat4(
        0, 0, -1.0202, -1,
        0, -1, 0, 0,
        1, 0, 0, 0,
        0, 0, -0.20202, 0
    ),
    mat4(
        0, 0, 1.0202, 1,
        0, -1, 0, 0,
        -1, 0, 0, 0,
        0, 0, -0.20202, 0
    ),
    mat4(
        1, 0, 0, 0,
        0, 0, 1.0202, 1,
        0, 1, 0, 0,
        0, 0, -0.20202, 0
    ),
    mat4(
        1, 0, 0, 0,
        0, 0, -1.0202, -1,
        0, -1, 0, 0,
        0, 0, -0.20202, 0
    ),
    mat4(
        1, 0, 0, 0,
        0, -1, 0, 0,
        0, 0, 1.0202, 1,
        0, 0, -0.20202, 0
    ),
    mat4(
        -1, 0, 0, 0,
        0, -1, 0, 0,
        0, 0, -1.0202, -1,
        0, 0, -0.20202, 0
    ),
};

void main()
{
    vec2 tex_coord = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(tex_coord * 2.0 - 1.0, 1.0, 1.0);
    outFragPos = (inv_view_projections[gl_InstanceIndex] * gl_Position).xyz;
	gl_Layer = gl_InstanceIndex;
}