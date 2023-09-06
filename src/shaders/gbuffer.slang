#include "scene.slangh"
#include "common.slangh"

struct VSInput
{
    float4 position: POSITION0; // xyz - position, w - texcoord u
	float4 normal: NORMAL0; // xyz - normal, w - texcoord v
    uint instance_id: SV_InstanceID;
};

struct VSOutput
{
    float4 position: SV_Position;
    float3 world_position: POSITION0;
    float4 clip_pos: POSITION1;
    float4 prev_clip_pos: POSITION2;
    float3 normal: NORMAL0;
    float2 texcoord: TEXCOORD0;
    uint instance_id: NORMAL1;
};

struct FSOutput
{
    float4 GBufferA: SV_Target0; // RGB: Albedo, A: Metallic
    float4 GBufferB: SV_Target1; // RG: Normal, BA: Motion Vector
    float4 GBufferC: SV_Target2; // R: Roughness, G: Curvature, B: Mesh ID, A: Linear Z
};

float2 direction_to_octohedral(float3 normal)
{
    float2 p = normal.xy * (1.0 / dot(abs(normal), 1.0));
    return normal.z > 0.0 ? p : (1.0 - abs(p.yx)) * (step(0.0, p) * 2.0 - 1.0);
}

float2 compute_motion_vector(float4 prev_pos, float4 current_pos)
{
	float2 current = (current_pos.xy / current_pos.w);
    float2 prev    = (prev_pos.xy / prev_pos.w);

	current = current * 0.5 + 0.5;
    prev    = prev * 0.5 + 0.5;

	return (prev - current);
}

float4 fetch_base_color(in Material material, in float2 uv)
{
	if (material.base_color_texture == ~0)
	{
		return material.base_color;
	}
	else
	{
		float4 base_color = Textures[material.base_color_texture].Sample(Samplers[int(SamplerType::Linear)], uv);
		base_color.rgb = pow(base_color.rgb, float3(2.2));
		return base_color * material.base_color;
	}
}

float2 fetch_roughness_metallic(in Material material, in float2 uv)
{
	if (material.metallic_roughness_texture == ~0)
	{
		return float2(material.roughness_factor, material.metallic_factor);
	}
	else
	{
		float2 roughness_metallic = Textures[material.metallic_roughness_texture].Sample(Samplers[int(SamplerType::Linear)], uv).gb;
		return roughness_metallic * float2(material.roughness_factor, material.metallic_factor);
	}
}

float3 fetch_normal(in Material material, in float3 in_normal, in float2 uv)
{
    if (material.normal_texture == ~0)
	{
		return normalize(in_normal);
	}
	else
	{
		float3 bitangent, tangent;
		coordinate_system(in_normal, tangent, bitangent);
		float3x3 TBN = float3x3(tangent, bitangent, in_normal);
        float3 normal = normalize(Textures[material.normal_texture].Sample(Samplers[int(SamplerType::Linear)], uv).rgb * 2.0 - 1.0);
        return normalize(mul(transpose(TBN), normal));
	}
}

float compute_curvature(float3 normal)
{
    float3 dx = ddx(normal);
    float3 dy = ddy(normal);

    float x = dot(dx, dx);
    float y = dot(dy, dy);

    return pow(max(x, y), 0.5f);
}

[shader("vertex")]
VSOutput vs_main(VSInput input)
{
    Instance instance = InstanceBuffer[input.instance_id];

    float4 world_pos = mul(instance.transform, float4(input.position.xyz, 1.0));
    float4 prev_world_pos = world_pos;

    VSOutput output;
    output.position = mul(ViewBuffer.view_projection, world_pos);
    output.world_position = world_pos.xyz;
    output.clip_pos = output.position;
    output.prev_clip_pos = mul(ViewBuffer.prev_view_projection, prev_world_pos);
    output.texcoord = float2(input.position.w, input.normal.w);
    output.normal = normalize(mul(transpose(float3x3(instance.transform)), input.normal.xyz));
    output.instance_id = input.instance_id;

    return output;
}

[shader("fragment")]
FSOutput fs_main(VSOutput input)
{
    Instance instance = InstanceBuffer[input.instance_id];
    Material material = MaterialBuffer[instance.material];

    float4 base_color = fetch_base_color(material, input.texcoord);
	float2 roughness_metallic = fetch_roughness_metallic(material, input.texcoord);
	float2 motion_vector = compute_motion_vector(input.prev_clip_pos, input.clip_pos);
	float2 normal = direction_to_octohedral(fetch_normal(material, input.normal, input.texcoord));
	float linear_z  = input.position.z / input.position.w;
	float curvature = compute_curvature(linear_z);
	float instance_id = float(input.instance_id);

    FSOutput output;
    if(material.alpha_mode == 1 &&
		base_color.a < material.cutoff)
	{
		discard;
	}
	
	output.GBufferA = float4(base_color.rgb, roughness_metallic.g);
	output.GBufferB = float4(normal, motion_vector);
	output.GBufferC = float4(roughness_metallic.r, curvature, instance_id, linear_z);

    return output;
}