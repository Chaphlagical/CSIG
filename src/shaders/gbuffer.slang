#include "common.slangh"

struct VSInput
{
    float4 position: POSITION0; // xyz - position, w - texcoord u
	float4 normal: NORMAL0; // xyz - normal, w - texcoord v
};

struct VSOutput
{
    float4 position: SV_Position;
    float3 world_position: POSITION0;
    float3 normal: NORMAL0;
    float2 texcoord: TEXCOORD0;
};

struct FSOutput
{

};

[shader("vertex")]
VSOutput vs_main(VSInput vs_input)
{
    SceneBuffer scene_buffer;
    // scene_buffer.instance.Load;

    VSOutput output;
    output.position = float4(vs_input.position.xyz, 1.0);
    output.normal = vs_input.normal.xyz;
    output.texcoord = float2(vs_input.position.w, vs_input.normal.w);
    return output;
}

[shader("fragment")]
FSOutput fs_main(VSOutput fs_input)
{

}