#include "ddgi.slangh"
#include "scene.slangh"

struct VSInput
{
    float3 position : POSITION0;
    float3 normal : NORMAL0;
    uint instance_id : SV_InstanceID;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 normal : NORMAL0;
    uint instance_id : NORMAL1;
};

struct PushConstant
{
    float scale;
};

[[vk::binding(0, 1)]] ConstantBuffer<DDGIUniforms> DDGIBuffer;
[[vk::binding(1, 1)]] Texture2D<float4> ProbeGridIrradiance;
[[vk::push_constant]] ConstantBuffer<PushConstant> push_constant;

[shader("vertex")]
VSOutput vs_main(VSInput input)
{
    VSOutput output;
    int3 grid_coord = probe_index_to_grid_coord(DDGIBuffer, input.instance_id);
    float3 probe_position = grid_coord_to_position(DDGIBuffer, grid_coord);
    output.position =  mul(ViewBuffer.view_projection, float4((input.position * push_constant.scale) + probe_position, 1.0f));
    output.normal = input.normal;
    output.instance_id = input.instance_id;
    return output;
}

[shader("fragment")]
float4 fs_main(VSOutput input): SV_Target0
{
    float2 probe_coord = texture_coord_from_direction(normalize(input.normal),
                                                      input.instance_id,
                                                      DDGIBuffer.irradiance_texture_width,
                                                      DDGIBuffer.irradiance_texture_height,
                                                      DDGIBuffer.irradiance_probe_side_length);
    return float4(ProbeGridIrradiance.SampleLevel(Samplers[int(SamplerType::Linear)], probe_coord, 0).rgb, 1.0f);
}