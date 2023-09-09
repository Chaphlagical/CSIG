#include "common.slangh"

struct PushConstant
{
    float threshold;
};

[[vk::binding(0, 0)]] Texture2D<float4> BloomMaskInput;
[[vk::binding(0, 1)]] RWTexture2D<float4> BloomMaskOutput;
[[vk::push_constant]] ConstantBuffer<PushConstant> push_constant;

[numthreads(8, 8, 1)]
void main(CSParam param)
{
    uint2 extent;
    BloomMaskInput.GetDimensions(extent.x, extent.y);

    if (param.DispatchThreadID.x > extent.x || param.DispatchThreadID.y > extent.y)
    {
        return;
    }

    float3 color = BloomMaskInput.Load(uint3(param.DispatchThreadID.xy, 0)).rgb;
    float lum = luminance(color);

    BloomMaskOutput[param.DispatchThreadID.xy] = float4(clamp(lum - push_constant.threshold, 0.0, 1.0) * color, 1.0);
}