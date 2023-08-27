[[vk::binding(0, 0)]] Texture2D<float4> reference_image;
[[vk::binding(1, 0)]] Texture2D<float4> target_image;
[[vk::binding(2, 0)]] RWTexture2D<float4> gradient_image;

[Differentiable]
float loss(float3 a, no_diff float3 x)
{
    float3 rs = log(a * exp(a) + a) - x;
    rs *= rs;
    return length(rs);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatch_thread: SV_DispatchThreadID)
{
    uint2 screen_size;
    target_image.GetDimensions(screen_size.x, screen_size.y);

    if (dispatch_thread.x >= screen_size.x ||
        dispatch_thread.y >= screen_size.y)
    {
        return;
    }

    float3 color = target_image.Load(uint3(dispatch_thread.xy, 0)).rgb;
    float3 ref_color = (reference_image.Load(uint3(dispatch_thread.xy, 0))).rgb;

    var x = diffPair(color);

    let result = bwd_diff(loss)(x, ref_color, 1.0);

    gradient_image[int2(dispatch_thread.xy)] = float4(x.d, 1.0);
}
