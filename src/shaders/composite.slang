#include "scene.slangh"
#include "common.slangh"

#ifdef VISUALIZE_AO
[[vk::binding(0, 1)]] Texture2D<float> AO_Image;
#endif

#ifdef VISUALIZE_REFLECTION
[[vk::binding(0, 1)]] Texture2D<float4> Reflection_Image;
#endif

#ifdef VISUALIZE_DI
[[vk::binding(0, 1)]] Texture2D<float4> DI_Image;
#endif

#ifdef VISUALIZE_GBUFFER
#include "gbuffer.slangh"
#endif

[[vk::binding(0, 2)]] RWTexture2D<float4> Output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(CSParam param)
{
    uint2 tex_size;
    Output.GetDimensions(tex_size.x, tex_size.y);
    const int2 coord = int2(param.DispatchThreadID.xy);
    const float2 pixel = float2(coord) + float2(0.5);
    const float2 uv = pixel / float2(tex_size);

    if (coord.x >= tex_size.x || coord.y >= tex_size.y)
    {
        return;
    }

#ifdef VISUALIZE_AO
    Output[coord] = float4(float3(AO_Image[coord].r), 1.0);
#endif

#ifdef VISUALIZE_DI
    Output[coord] = float4(pow(DI_Image[coord].rgb, 1.0 / 2.2), 1.0);
#endif

#ifdef VISUALIZE_REFLECTION
    Output[coord] = float4(pow(Reflection_Image[coord].rgb, 1.0 / 2.2), 1.0);
#endif

#ifdef VISUALIZE_ALBEDO
    Output[coord] = float4(GBufferA[coord].xyz, 1.0);
#endif

#ifdef VISUALIZE_NORMAL
    Output[coord] = float4(octohedral_to_direction(GBufferB[coord].rg), 1.0);
#endif

#ifdef VISUALIZE_METALLIC
    Output[coord] = float4(float3(GBufferA[coord].a), 1.0);
#endif

#ifdef VISUALIZE_ROUGHNESS
    Output[coord] = float4(float3(GBufferC[coord].r), 1.0);
#endif

#ifdef VISUALIZE_POSITION
    Output[coord] = float4(world_position_from_depth(float2(coord) / float2(tex_size), DepthBuffer[coord].r, ViewBuffer.view_projection_inv), 1.0);
#endif
}