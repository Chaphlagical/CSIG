#include "common.slangh"

[[vk::binding(0, 0)]] Texture2D<float4> BloomDownSamplingInput;
[[vk::binding(1, 0)]] RWTexture2D<float4> BloomDownSamplingOutput;
[[vk::binding(2, 0)]] SamplerState BloomDownSampleSampler;

[numthreads(8, 8, 1)]
void main(CSParam param)
{
    uint2 extent;
    BloomDownSamplingOutput.GetDimensions(extent.x, extent.y);

    if (param.DispatchThreadID.x >= extent.x || param.DispatchThreadID.y >= extent.y)
    {
        return;
    }

    float2 uv = (float2(param.DispatchThreadID.xy) + float2(0.5, 0.5)) / float2(extent);
    float3 avg_color = BloomDownSamplingInput.SampleLevel(BloomDownSampleSampler, uv, 0.0).rgb;
    BloomDownSamplingOutput[param.DispatchThreadID.xy] = float4(avg_color, 1.0);
}