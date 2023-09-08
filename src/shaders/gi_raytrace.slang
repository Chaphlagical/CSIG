#include "common.slangh"
#include "ddgi.slangh"
#include "gbuffer.slangh"
#include "random.slangh"
#include "raytrace.slangh"
#include "scene.slangh"

#define SAMPLE_ENVMMAP
#include "lighting.slangh"

#define NUM_THREADS_X 8
#define NUM_THREADS_Y 8

struct PushConstant
{
    float4x4 random_orientation;
    uint num_frames;
    uint infinite_bounces;
    float gi_intensity;
};

[[vk::binding(0, 2)]] ConstantBuffer<DDGIUniforms> DDGIBuffer;
[[vk::binding(1, 2)]] RWTexture2D<float4> Radiance;
[[vk::binding(2, 2)]] RWTexture2D<float4> DirectionDistance;
[[vk::binding(3, 2)]] Texture2D<float4> ProbeGridIrradiance;
[[vk::binding(4, 2)]] Texture2D<float4> ProbeGridDepth;
[[vk::push_constant]] ConstantBuffer<PushConstant> push_constant;

float3 spherical_fibonacci(float i, float n)
{
    const float PHI = sqrt(5) * 0.5 + 0.5;
#define madfrac(A, B) ((A) * (B)-floor((A) * (B)))
    float phi = 2.0 * PI * madfrac(i, PHI - 1);
    float cos_theta = 1.0 - (2.0 * i + 1.0) * (1.0 / n);
    float sin_theta = sqrt(clamp(1.0 - cos_theta * cos_theta, 0.0f, 1.0f));

    return float3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);

#undef madfrac
}

float3 fresnel_schlick_roughness(float cosTheta, float3 F0, float roughness)
{
    return F0 + (max(float3(1.0 - roughness), F0) - F0) * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

float3 indirect_lighting(float3 Wo, float3 N, float3 P, float3 F0, float3 diffuse_color, float roughness, float metallic)
{
    float3 F = fresnel_schlick_roughness(max(dot(N, Wo), 0.0), F0, roughness);

    float3 kS = F;
    float3 kD = 1.0 - kS;
    kD *= 1.0 - metallic;
    return push_constant.gi_intensity * kD * diffuse_color * sample_irradiance(DDGIBuffer, P, N, Wo, ProbeGridIrradiance, ProbeGridDepth);
}

[shader("compute")]
[numthreads(NUM_THREADS_X, NUM_THREADS_Y, 1)]
void main(CSParam param)
{
    const int2 size = texture_size(GBufferA, 0);
    const int2 coord = int2(param.DispatchThreadID.xy);
    const int probe_id = coord.y;
    const int ray_id = coord.x;

    uint seed = tea(size.x * param.DispatchThreadID.y + param.DispatchThreadID.x, uint(ViewBuffer.cam_pos.w));

    float3 ray_origin = probe_location(DDGIBuffer, probe_id);
    float3 direction = normalize(mul(float3x3(push_constant.random_orientation), spherical_fibonacci(ray_id, DDGIBuffer.rays_per_probe)));

    float3 Lo = 0.0;
    float3 T = 1.0;
    float hit_distance = Infinity;

    if (closest_hit(ray_origin, direction, RAY_FLAG_FORCE_OPAQUE))
    {
        RayDesc ray;
        ray.Origin = ray_origin;
        ray.Direction = direction;
        ShadeState sstate = get_shade_state(ray, prd);
        Lo += direct_lighting(sstate, -direction, T, seed);
        if (push_constant.infinite_bounces == 1)
        {
            const float3 F0 = lerp(float3(0.04f), sstate.mat.base_color.rgb, sstate.mat.metallic_factor);
            const float3 c_diffuse = lerp(sstate.mat.base_color.rgb * (float3(1.0f) - F0), float3(0.0f), sstate.mat.metallic_factor);
            Lo += indirect_lighting(-direction, sstate.normal, sstate.position, F0, c_diffuse, sstate.mat.roughness_factor, sstate.mat.metallic_factor);
        }
    }
    else
    {
        Lo = EnvMap.SampleLevel(Samplers[int(SamplerType::Linear)], direction, 0.0).rgb;
    }

    Radiance[coord] = float4(Lo, 0.0);
    DirectionDistance[coord] = float4(direction, hit_distance);
}
