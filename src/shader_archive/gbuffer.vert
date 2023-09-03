#version 450

#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_scalar_block_layout : enable
#extension GL_EXT_shader_explicit_arithmetic_types : enable

#include "common.glsl"

layout(location = 0) in vec4 inPosition;
layout(location = 1) in vec4 inNormal;

layout(location = 0) out vec3 outFragPos;
layout(location = 1) out vec2 outTexcoord;
layout(location = 2) out vec3 outNormal;
layout(location = 3) out vec4 outClipPos;
layout(location = 4) out vec4 outPrevClipPos;
layout(location = 5) out uint outInstanceID;

layout(binding = 0, scalar) uniform UBO
{
    GlobalData ubo;
};

layout(std430, binding = 2) buffer InstanceBuffer {
	Instance instances[];
};

void main()
{
	Instance instance = instances[gl_InstanceIndex];

	vec4 world_pos = instance.transform * vec4(inPosition.xyz, 1.0);
	vec4 prev_world_pos = instance.transform * vec4(inPosition.xyz, 1.0);

	outFragPos = world_pos.xyz;
	gl_Position = ubo.view_projection * world_pos;
	outClipPos = gl_Position;
	outPrevClipPos = ubo.prev_view_projection * prev_world_pos;
	outTexcoord = vec2(inPosition.w, inNormal.w);

	outNormal = normalize(transpose(mat3(instance.transform_inv)) * inNormal.xyz);

	outInstanceID = gl_InstanceIndex;
}