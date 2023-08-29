#include "common.slangh"

[[vk::binding(0, 0)]] Texture2D Texture;
[[vk::binding(1, 0)]] SamplerState Sampler;

static const float4x4 inv_view_projections[6] = 
{
    float4x4(
        0, 0, 0, 1,
        0, -1, 0, -0,
        -1, 0, -0, 0,
        0, 0, -4.95, 5.05
    ),
    float4x4(
        0, 0, 0, -1,
        0, -1, 0, -0,
        1, 0, 0, 0,
        0, -0, -4.95, 5.05
    ),
    float4x4(
        1, 0, -0, 0,
        0, -0, 0, 1,
        -0, 1, -0, 0,
        0, 0, -4.95, 5.05
    ),
    float4x4(
        1, 0, -0, 0,
        0, -0, 0, -1,
        0, -1, -0, 0,
        0, 0, -4.95, 5.05
    ),
    float4x4(
        1, 0, -0, 0,
        0, -1, 0, -0,
        -0, 0, -0, 1,
        0, -0, -4.95, 5.05
    ),
    float4x4(
        -1, 0, -0, 0,
        0, -1, -0, -0,
        -0, 0, -0, -1,
        0, -0, -4.95, 5.05
    ),
};

float2 sample_spherical_map(float3 v)
{
    float2 uv = float2(atan2(v.x, v.z), asin(v.y));
    uv.x /= 2 * PI;
    uv.y /= PI;
    uv += 0.5;
    uv.y = 1.0 - uv.y;
    return uv;
}

[shader("vertex")]
void vs_main(uint vertex_id: SV_VertexID, uint instance_id: SV_InstanceID, out float4 position: SV_Position, out float3 frag_pos: POSITION0, out uint layer_id: SV_RenderTargetArrayIndex)
{
    float2 tex_coord = float2((vertex_id << 1) & 2, vertex_id & 2);
    position = float4(tex_coord * 2.0 - 1.0, 1.0, 1.0);
    frag_pos = mul(inv_view_projections[instance_id], position).xyz;
	layer_id = instance_id;
}

[shader("fragment")]
float4 fs_main(float3 frag_pos: POSITION0): SV_Target
{
	float2 uv = sample_spherical_map(normalize(frag_pos));
    return float4(Texture.Sample(Sampler, uv).rgb, 1.0);
}