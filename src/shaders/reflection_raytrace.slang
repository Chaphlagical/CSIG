#include "common.slangh"
#include "gbuffer.slangh"
#include "scene.slangh"
#include "raytrace.slangh"

#define NUM_THREADS_X 8
#define NUM_THREADS_Y 8

#define MIRROR_REFLECTIONS_ROUGHNESS_THRESHOLD 0.05f
#define DDGI_REFLECTIONS_ROUGHNESS_THRESHOLD 0.75f

struct PushConstant
{
    int gbuffer_mip;
    float bias;
    float rough_ddgi_intensity;
    uint approximate_with_ddgi;
    float gi_intensity;
    uint sample_gi;
};

[[vk::binding(0, 2)]] RWTexture2D<float4> Output;
[[vk::push_constant]] ConstantBuffer<PushConstant> push_constant;

float4 importance_sample_ggx(float2 E, float3 N, float Roughness)
{
    float a = Roughness * Roughness;
    float m2 = a * a;

    float phi = 2.0f * PI * E.x;
    float cosTheta = sqrt((1.0f - E.y) / (1.0f + (m2 - 1.0f) * E.y));
    float sinTheta = sqrt(1.0f - cosTheta * cosTheta);

    float3 H;
    H.x = cos(phi) * sinTheta;
    H.y = sin(phi) * sinTheta;
    H.z = cosTheta;

    float d = (cosTheta * m2 - cosTheta) * cosTheta + 1;
    float D = m2 / (PI * d * d);

    float PDF = D * cosTheta;

    float3 up = abs(N.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 tangent = normalize(cross(up, N));
    float3 bitangent = cross(N, tangent);

    float3 sampleVec = tangent * H.x + bitangent * H.y + N * H.z;
    return float4(normalize(sampleVec), PDF);
}

float2 random_sample(int2 coord, uint num_frames)
{
    return float2(sample_blue_noise(coord, int(num_frames), 0, SobelSequence, ScramblingRankingTile[0]),
                  sample_blue_noise(coord, int(num_frames), 1, SobelSequence, ScramblingRankingTile[0]));
}

float3 fresnel_schlick_roughness(float cosTheta, float3 F0, float roughness)
{
    return F0 + (max(float3(1.0 - roughness), F0) - F0) * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

float3 indirect_lighting(float3 Wo, float3 N, float3 P, float3 F0, float3 diffuse_color, float roughness, float metallic)
{
    const float3 R = reflect(-Wo, N);

    float3 F = fresnel_schlick_roughness(max(dot(N, Wo), 0.0), F0, roughness);

    float3 kS = F;
    float3 kD = 1.0 - kS;
    kD *= 1.0 - metallic;

    const float MAX_REFLECTION_LOD = 4.0;

    float3 prefiltered_color = PrefilterMap.SampleLevel(Samplers[int(SamplerType::Linear)], R, roughness * MAX_REFLECTION_LOD).rgb;
    float2 brdf = GGXLut.Sample(Samplers[int(SamplerType::Linear)], float2(max(dot(N, Wo), 0.0), roughness)).rg;

    float3 specular = prefiltered_color * (F * brdf.x + brdf.y);

    // TODO: DDGI
    // float3 diffuse = push_constant.gi_intensity * diffuse_color * sample_irradiance(ddgi, P, N, Wo, s_Irradiance, s_Depth);
    float3 diffuse = 0;

    return kD * diffuse + specular;
}

void ray_trace(RayDesc ray, out float3 color, out float ray_length)
{
    if (closest_hit(ray.Origin, ray.Direction, RAY_FLAG_FORCE_OPAQUE))
    {
        ShadeState sstate = get_shade_state(ray, prd);

        const float3 albedo = sstate.mat.base_color.rgb;
        const float roughness = sstate.mat.roughness_factor;
        const float metallic = sstate.mat.metallic_factor;
        const float3 N = sstate.normal;
        const float3 wo = -ray.Direction;
        const float3 R = reflect(-wo, N);

        const float3 F0 = lerp(float3(0.04f), albedo, metallic);
        const float3 c_diffuse = lerp(albedo * (float3(1.0f) - F0), float3(0.0f), metallic);

        float3 Lo = float3(0.0f);

        // Lo += direct_lighting(ubo.light, wo, N, sstate.position.xyz, F0, c_diffuse, roughness);

        if (push_constant.sample_gi == 1)
        {
            Lo += indirect_lighting(wo, N, sstate.position.xyz, F0, c_diffuse, roughness, metallic);
        }

        color = Lo;
        ray_length = prd.hit_t;
    }
    else
    {
        color = EnvMap.SampleLevel(Samplers[int(SamplerType::Linear)], ray.Direction, 0).rgb;
        ray_length = -1.0;
    }
}

[shader("compute")]
[numthreads(NUM_THREADS_X, NUM_THREADS_Y, 1)]
void main(CSParam param)
{
    const int2 size = texture_size(GBufferA, push_constant.gbuffer_mip);
    const int2 current_coord = int2(param.DispatchThreadID.xy);
    const float2 pixel_center = float2(current_coord) + float2(0.5);
    const float2 tex_coord = pixel_center / float2(size);

    float depth = DepthBuffer.Load(int3(current_coord, push_constant.gbuffer_mip)).r;

    if (depth == 1.0f)
    {
        Output[current_coord] = float4(0.0f, 0.0f, 0.0f, -1.0f);
        return;
    }

    float roughness = GBufferC.Load(int3(current_coord, push_constant.gbuffer_mip)).r;
    float3 P = world_position_from_depth(tex_coord, depth, ViewBuffer.view_projection_inv);
    float3 N = octohedral_to_direction(GBufferB.Load(int3(current_coord, push_constant.gbuffer_mip)).rg);
    float3 Wo = normalize(ViewBuffer.cam_pos.xyz - P.xyz);

    float3 color = 0;
    float ray_length = -1.0;
    float3 ray_origin = P + N * push_constant.bias;

    RayDesc ray;
    ray.Origin = P + N * push_constant.bias;

    if (roughness < MIRROR_REFLECTIONS_ROUGHNESS_THRESHOLD)
    {
        ray.Direction = reflect(-Wo, N.xyz);
        ray_trace(ray, color, ray_length);
    }
    else if (roughness > DDGI_REFLECTIONS_ROUGHNESS_THRESHOLD && push_constant.approximate_with_ddgi == 1)
    {
        float3 R = reflect(-Wo, N.xyz);
        // color = push_constant.rough_ddgi_intensity * sample_irradiance(ddgi, P, R, Wo, s_Irradiance, s_Depth);
    }
    else
    {
        float2 Xi = random_sample(current_coord, int(ViewBuffer.cam_pos.w));
        float4 Wh_pdf = importance_sample_ggx(Xi, N, roughness);
        float pdf = Wh_pdf.w;
        ray.Direction = reflect(-Wo, Wh_pdf.xyz);
        ray_trace(ray, color, ray_length);
    }

    float3 clamped_color = min(color, float3(0.7f));
    Output[current_coord] = float4(clamped_color, ray_length);
}