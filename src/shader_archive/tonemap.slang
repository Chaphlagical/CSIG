#include "common.slangh"
#include "random.slangh"

#define NUM_THREADS_X 8
#define NUM_THREADS_Y 8

struct PushConstant
{
	float brightness;
	float contrast;
	float saturation;
	float vignette;
	float avg_lum;
	float y_white;
	float key;
};

[[vk::binding(0, 0)]] Texture2D<float4> InputImage;
[[vk::binding(0, 1)]] RWTexture2D<float4> OutputImage;
[[vk::push_constant]] ConstantBuffer<PushConstant> push_constant;

static const float GAMMA     = 2.2;
static const float INV_GAMMA = 1.0 / GAMMA;

float3 linear_to_srgb(float3 color)
{
    return pow(color, float3(INV_GAMMA));
}

float3 srgb_to_linear(float3 srgb)
{
    return float3(pow(srgb.xyz, float3(GAMMA)));
}

float4 srgb_to_linear(float4 srgb)
{
    return float4(srgb_to_linear(srgb.xyz), srgb.w);
}

float3 uncharted2(float3 color)
{
    const float A = 0.15;
    const float B = 0.50;
    const float C = 0.10;
    const float D = 0.20;
    const float E = 0.02;
    const float F = 0.30;
    return ((color * (A * color + C * B) + D * E) / (color * (A * color + B) + D * F)) - E / F;
}

float3 tone_map_uncharted(float3 color)
{
    const float W = 11.2;
    color = uncharted2(color * 2.0);
    float3 white_scale = 1.0 / uncharted2(float3(W));
    return linear_to_srgb(color * white_scale);
}

float3 tonemap(float3 color, float exposure)
{
    color *= exposure;
    return tone_map_uncharted(color);
}

float3 dither(float3 linear_color, float3 noise, float quant)
{
	float3 c0 = floor(linear_to_srgb(linear_color) / quant) * quant;
	float3 c1 = c0 + quant;
	float3 discr = lerp(srgb_to_linear(c0), srgb_to_linear(c1), noise);
	return lerp(c0, c1, discr < linear_color);
}

[shader("compute")]
[numthreads(NUM_THREADS_X, NUM_THREADS_Y, 1)]
void main(CSParam param)
{
	const int2 size = texture_size(InputImage, 0);
	const int2 pos = int2(param.DispatchThreadID.xy);
	const float2 pixel = float2(pos) + float2(0.5);
	float2 uv = pixel / float2(size);
	float4 hdr = InputImage.Load(int3(pos, 0)).rgba;
	float3 color = tonemap(hdr.rgb, push_constant.avg_lum);
	uint3 r = pcg3d(uint3(pos.xy, 0));
	float3 noise = asfloat(0x3f800000 | (r >> 9)) - 1.0;

	color = dither(srgb_to_linear(color), noise, 1.0 / 255.0);
	color = clamp(lerp(float3(0.5), color, push_constant.contrast), 0, 1);
	color = pow(color, float3(1.0 / push_constant.brightness));
	color = lerp(float3(dot(color, float3(0.299, 0.587, 0.114))), color, push_constant.saturation);

	uv = (uv - 0.5) * 2.0;
	color *= 1.0 - dot(uv, uv) * push_constant.vignette;

    OutputImage[param.DispatchThreadID.xy] = float4(color, hdr.a);
}