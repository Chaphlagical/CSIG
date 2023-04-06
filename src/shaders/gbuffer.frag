#version 450

#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_nonuniform_qualifier : enable

#include "common.glsl"

layout(location = 0) in vec3 inFragPos;
layout(location = 1) in vec2 inTexcoord;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec3 inTangent;
layout(location = 4) in vec4 inClipPos;
layout(location = 5) in vec4 inPrevClipPos;
layout(location = 6) in flat uint inInstanceID;

layout(location = 0) out vec4 GBufferA; // RGB: Albedo, A: Metallic
layout(location = 1) out vec4 GBufferB; // RG: Normal, BA: Motion Vector
layout(location = 2) out vec4 GBufferC; // R: Roughness, G: Curvature, B: Mesh ID, A: Linear Z

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

layout(binding = 1) uniform sampler2D textures[];

layout(std430, binding = 2) buffer InstanceBuffer {
	Instance instances[];
};

layout(std430, binding = 3) buffer MaterialBuffer {
	Material materials[];
};

vec2 compute_motion_vector(vec4 prev_pos, vec4 current_pos)
{
	vec2 current = (current_pos.xy / current_pos.w);
    vec2 prev    = (prev_pos.xy / prev_pos.w);

	current = current * 0.5 + 0.5;
    prev    = prev * 0.5 + 0.5;

	return (prev - current);
}

vec4 fetch_base_color(in Material material, in vec2 uv)
{
	if (material.base_color_texture == ~0)
	{
		return material.base_color;
	}
	else
	{
		return texture(textures[material.base_color_texture], uv) * material.base_color;
	}
}

vec2 fetch_roughness_metallic(in Material material, in vec2 uv)
{
	if (material.metallic_roughness_texture == ~0)
	{
		return vec2(material.roughness_factor, material.metallic_factor);
	}
	else
	{
		vec2 roughness_metallic = texture(textures[material.metallic_roughness_texture], uv).gb;
		return roughness_metallic * vec2(material.roughness_factor, material.metallic_factor);
	}
}

vec3 fetch_normal(in Material material, in vec2 uv)
{
	
	if (material.normal_texture == ~0)
	{
		return normalize(inNormal);
	}
	else
	{
		mat3 TBN = mat3(normalize(inTangent), normalize(cross(inNormal, inTangent)), normalize(inNormal));
		vec3 normal = normalize(texture(textures[material.normal_texture], uv).rgb * 2.0-1.0);
		return normalize(TBN * normal);
	}
}

float compute_curvature(float depth)
{
    vec3 dx = dFdx(inNormal);
    vec3 dy = dFdy(inNormal);

    float x = dot(dx, dx);
    float y = dot(dy, dy);

    return pow(max(x, y), 0.5f);
}

void main()
{
	Instance instance = instances[inInstanceID];
	Material material = materials[instance.material];

	vec4 base_color = fetch_base_color(material, inTexcoord);
	vec2 roughness_metallic = fetch_roughness_metallic(material, inTexcoord);
	vec2 motion_vector = compute_motion_vector(inPrevClipPos, inClipPos);
	vec2 normal = direction_to_octohedral(fetch_normal(material, inTexcoord));
	float linear_z  = gl_FragCoord.z / gl_FragCoord.w;
	float curvature = compute_curvature(linear_z);
	float instance_id = float(inInstanceID);

	if(material.alpha_mode == 1 &&
		base_color.a < material.cutoff)
	{
		discard;
	}

	GBufferA = vec4(fetch_normal(material, inTexcoord), roughness_metallic.g);
	GBufferB = vec4(normal, motion_vector);
	GBufferC = vec4(roughness_metallic.r, curvature, instance_id, linear_z);
}