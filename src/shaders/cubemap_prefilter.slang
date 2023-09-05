#include "common.slangh"
#include "bsdf.slangh"

#define LOCAL_SIZE 8
#define CUBEMAP_SIZE 1024
#define PREFILTER_MAP_SIZE 256
#define PREFILTER_MIP_LEVELS 5
#define SAMPLE_COUNT 1024

struct PushConstant
{
	int mip;
};

[[vk::binding(0, 0)]] TextureCube<float4> EnvMap;
[[vk::binding(0, 0)]] SamplerState Sampler;
[[vk::binding(1, 0)]] RWTexture2DArray<float4> PrefilterMap;
[[vk::push_constant]] ConstantBuffer<PushConstant> push_constant;

float GGX(float3 N, float3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NoH = max(dot(N, H), 0.f);
    float NoH2 = NoH * NoH;
    float nom = a2;
    float denom = (NoH2 * (a2 - 1.f) + 1.f);
    denom = PI * denom * denom;
    return nom / denom;
}

float3 GGXImportanceSampling(float2 Xi, float3 N, float roughness)
{
    float a = roughness * roughness;

    float phi = 2.0 * PI * Xi.x;
    float cos_theta = sqrt((1.f - Xi.y) / (1.f + (a * a - 1.f) * Xi.y));
    float sin_theta = sqrt(1.f - cos_theta * cos_theta);

    float3 H = float3(
        cos(phi) * sin_theta,
        sin(phi) * sin_theta,
        cos_theta);

    float3 Up = N.z > 0.999 ? float3(0.0, 1.0, 0.0) : float3(0.0, 0.0, 1.0);
    float3 T = normalize(cross(N, Up));
    float3 B = normalize(cross(N, T));

    return T * H.x + B * H.y + N * H.z;
}

float RadicalInverse_VdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10; // / 0x100000000
}

float2 Hammersley(uint i, uint N)
{
    return float2(float(i) / float(N), RadicalInverse_VdC(i));
}

[shader("compute")]
[numthreads(LOCAL_SIZE, LOCAL_SIZE, 1)]
void main(CSParam param)
{
    uint2 extent;
    uint layers;
    PrefilterMap.GetDimensions(extent.x, extent.y, layers);
    
    float3 colors[5] = {
        float3(0, 0, 0),
        float3(1, 0, 0),
        float3(0, 1, 0),
        float3(0, 0, 1),
        float3(1, 1, 1),
    };
    
    uint level = uint(log2(PREFILTER_MAP_SIZE / extent.x));
    float roughness = float(level) / float(PREFILTER_MIP_LEVELS - 1);
    
    if (param.DispatchThreadID.x >= extent.x ||
        param.DispatchThreadID.y >= extent.y)
    {
        return;
    }
    
    uint2 size;
    EnvMap.GetDimensions(size.x, size.y);
    uint resolution = size.x;
    
    float3 N = calculate_cubemap_direction(param.DispatchThreadID.z, param.DispatchThreadID.x, param.DispatchThreadID.y, extent.x, extent.y);
    float3 R = N;
    float3 V = R;

    float3 prefilter_color = float3(0.0, 0.0, 0.0);
    float total_weight = 0.0;

    for (uint i = 0; i < SAMPLE_COUNT; i++)
    {
        float2 Xi = Hammersley(i, SAMPLE_COUNT);
        float3 H = GGXImportanceSampling(Xi, N, roughness);
        float3 L = normalize(2.0 * dot(V, H) * H - V);

        float NoL = max(dot(N, L), 0.0);
        if (NoL > 0.0)
        {
            float D = GGX(N, H, roughness);
            float NoH = max(dot(N, H), 0.f);
            float HoV = max(dot(H, V), 0.f);
            float pdf = D * NoH / (4 * HoV) + 0.0001;
            
            float texel_ = 4.f * PI / (6.f * resolution * resolution);
            float sample_ = 1.f / (float(SAMPLE_COUNT) * pdf + 0.0001f);
            
            float mip_level = (roughness == 0.0 ? 0.0 : 0.5f * log2(sample_ / texel_));
            
            prefilter_color += EnvMap.SampleLevel(Sampler, L, mip_level).rgb * NoL;
            total_weight += NoL;
        }
    }

    prefilter_color /= total_weight;
    PrefilterMap[int3(param.DispatchThreadID.xyz)] = float4(prefilter_color, 1.0);
    //PrefilterMap[int3(param.DispatchThreadID.xyz)] = float4(colors[level], 1.0);
}