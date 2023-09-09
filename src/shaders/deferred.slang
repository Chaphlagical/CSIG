#include "common.slangh"
#include "gbuffer.slangh"
#include "raytrace.slangh"
#include "scene.slangh"


#define NUM_THREADS_X 8
#define NUM_THREADS_Y 8

struct PushConstant
{ 
    uint enable_ao;
    uint enable_reflection;
    uint enable_gi;
    float indirect_specular_strength;
};

[[vk::binding(0, 2)]] Texture2D<float> AO_Image;
[[vk::binding(0, 3)]] Texture2D<float4> DI_Image;
[[vk::binding(0, 4)]] Texture2D<float4> GI_Image;
[[vk::binding(0, 5)]] Texture2D<float4> Reflection_Image;
[[vk::binding(0, 6)]] RWTexture2D<float4> Output_Image;
[[vk::push_constant]] ConstantBuffer<PushConstant> push_constant;

float3 fresnel_schlick_roughness(float cosTheta, float3 F0, float roughness)
{
    return F0 + (max(float3(1.0 - roughness), F0) - F0) * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

float3 indirect_lighting(ShadeState sstate, float2 uv, float3 N, float3 diffuse_color, float roughness, float metallic, float ao, float3 wo, float3 F0)
{
    const float3 R = reflect(-wo, N);

    float3 F = fresnel_schlick_roughness(max(dot(N, wo), 0.0), F0, roughness);

    float3 kS = F;
    float3 kD = 1.0 - kS;
    kD *= 1.0 - metallic;

    float3 irradiance = push_constant.enable_gi == 1 ? GI_Image.SampleLevel(Samplers[int(SamplerType::Linear)], uv, 0).rgb : evaluate_sh_irradiance(N, IrradianceSH);
    float3 diffuse = irradiance * diffuse_color;

    const float MAX_REFLECTION_LOD = 4.0;
    float3 prefilteredColor = push_constant.enable_reflection == 1 ? 
        Reflection_Image.SampleLevel(Samplers[int(SamplerType::Linear)], uv, 0).rgb : 
        PrefilterMap.SampleLevel(Samplers[int(SamplerType::Linear)], R, roughness * MAX_REFLECTION_LOD).rgb * float(!any_hit(sstate.position, R, 1e32));
    float2 brdf = GGXLut.SampleLevel(Samplers[int(SamplerType::Linear)], float2(max(dot(N, wo), 0.0), roughness), 0).rg;
    float3 specular = prefilteredColor * (F * brdf.x + brdf.y) * push_constant.indirect_specular_strength;

    return (kD * diffuse + specular) * ao;
}

[shader("compute")]
[numthreads(NUM_THREADS_X, NUM_THREADS_Y, 1)]
void main(CSParam param)
{
    const int2 size = texture_size(GBufferA, 0);
	const int2 coord = int2(param.DispatchThreadID.xy);
    const float2 uv = (float2(coord) + 0.5) / float2(size);

    ShadeState sstate;
    bool is_hit = get_primary_state(float2(coord) + 0.5, 0, sstate);
    const float3 wo = normalize(ViewBuffer.cam_pos.xyz - sstate.position);
    float3 Lo = 0.0;
    if (!is_hit)
    {
        Lo = EnvMap.SampleLevel(Samplers[int(SamplerType::Linear)], -wo, 0).rgb;
    }
    else
    {
        const float3 F0 = lerp(float3(0.04), sstate.mat.base_color.rgb, sstate.mat.metallic_factor);
        const float3 c_diff = lerp(sstate.mat.base_color.rgb * (1.0 - F0), 0.0, sstate.mat.metallic_factor);

        const float3 N = sstate.normal;

        const float roughness = sstate.mat.roughness_factor;
        const float metallic = sstate.mat.metallic_factor;

        float ao = push_constant.enable_ao == 1 ? AO_Image.SampleLevel(Samplers[int(SamplerType::Linear)], uv, 0).r : 1.0;
        float3 direct_light = DI_Image.SampleLevel(Samplers[int(SamplerType::Linear)], uv, 0).rgb;
        float3 indirect_light = indirect_lighting(sstate, uv, N, c_diff, roughness, metallic, ao, wo, F0);

        Lo = direct_light + indirect_light;
    }
    
    Output_Image[coord] = float4(Lo, 1.0);
}