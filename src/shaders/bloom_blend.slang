#include "common.slangh"

struct PushConstant
{
    float intensity;
};

[[vk::binding(0, 0)]] Texture2D BloomBlendInput;
[[vk::binding(0, 1)]] Texture2D BloomBlendBloom;
[[vk::binding(1, 1)]] RWTexture2D<float4> BloomBlendOutput;
[[vk::push_constant]] ConstantBuffer<PushConstant> push_constant;

[numthreads(8, 8, 1)]
void main(CSParam param)
{
    uint2 extent;
    BloomBlendOutput.GetDimensions(extent.x, extent.y);

    if (param.DispatchThreadID.x >= extent.x || param.DispatchThreadID.y >= extent.y)
    {
        return;
    }

    float2 uv = (float2(param.DispatchThreadID.xy) + float2(0.5, 0.5)) / float2(extent);

    BloomBlendOutput[param.DispatchThreadID.xy] = float4(BloomBlendInput.Load(uint3(param.DispatchThreadID.xy, 0), 0).rgb + push_constant.intensity * BloomBlendBloom.Load(uint3(param.DispatchThreadID.xy / 2, 0), 0).rgb, 1.0);
}