#version 450

#extension GL_KHR_vulkan_glsl : enable
#extension GL_GOOGLE_include_directive : enable

#include "common.glsl"

layout(location = 0) in vec4 inPosition;
layout(location = 1) in vec4 inNormal;

layout(location = 0) out vec3 outFragPos;
layout(location = 1) out vec2 outTexcoord;
layout(location = 2) out vec3 outNormal;
layout(location = 3) out vec4 outClipPos;
layout(location = 4) out vec4 outPrevClipPos;
layout(location = 5) out uint outInstanceID;

layout(binding = 0) uniform UBO
{
	mat4 view_inv;
	mat4 projection_inv;
	mat4 view_projection_inv;
	mat4 view_projection;
	mat4 prev_view_projection;
	vec4 cam_pos;
	vec4 jitter;
} ubo;

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

	mat3 normal_mat = transpose(inverse(mat3(instance.transform)));

	outNormal = normalize(normal_mat * inNormal.xyz);

	outInstanceID = gl_InstanceIndex;
}