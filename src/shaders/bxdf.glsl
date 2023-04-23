#ifndef BXDF_GLSL
#define BXDF_GLSL

#include "common.glsl"

mat3 make_rotation_matrix(vec3 z)
{
    const vec3 ref = abs(dot(z, vec3(0, 1, 0))) > 0.99 ? vec3(0, 0, 1) : vec3(0, 1, 0);

    const vec3 x = normalize(cross(ref, z));
    const vec3 y = cross(z, x);

    return mat3(x, y, z);
}

vec3 sample_cosine_lobe(vec3 n, vec2 rnd)
{
    vec2 rand_sample = max(vec2(0.00001), rnd);

    const float phi = 2.0f * PI * rand_sample.y;

    const float cos_theta = sqrt(rand_sample.x);
    const float sin_theta = sqrt(1 - rand_sample.x);

    vec3 t = vec3(sin_theta * cos(phi), sin_theta * sin(phi), cos_theta);

    return normalize(make_rotation_matrix(n) * t);
}

vec3 F_Schlick(vec3 f0, vec3 f90, float VdotH)
{
    return f0 + (f90 - f0) * pow(clamp(1.0 - VdotH, 0.0, 1.0), 5.0);
}

float F_Schlick(float f0, float f90, float VdotH)
{
    float x = clamp(1.0 - VdotH, 0.0, 1.0);
    float x2 = x * x;
    float x5 = x * x2 * x2;
    return f0 + (f90 - f0) * x5;
}

float F_Schlick(float f0, float VdotH)
{
    float f90 = 1.0;
    return F_Schlick(f0, f90, VdotH);
}

vec3 F_Schlick(vec3 f0, float f90, float VdotH)
{
    float x = clamp(1.0 - VdotH, 0.0, 1.0);
    float x2 = x * x;
    float x5 = x * x2 * x2;
    return f0 + (f90 - f0) * x5;
}

vec3 F_Schlick(vec3 f0, float VdotH)
{
    float f90 = 1.0;
    return F_Schlick(f0, f90, VdotH);
}

vec3 Schlick_to_F0(vec3 f, vec3 f90, float VdotH) {
    float x = clamp(1.0 - VdotH, 0.0, 1.0);
    float x2 = x * x;
    float x5 = clamp(x * x2 * x2, 0.0, 0.9999);

    return (f - f90 * x5) / (1.0 - x5);
}

float Schlick_to_F0(float f, float f90, float VdotH) {
    float x = clamp(1.0 - VdotH, 0.0, 1.0);
    float x2 = x * x;
    float x5 = clamp(x * x2 * x2, 0.0, 0.9999);

    return (f - f90 * x5) / (1.0 - x5);
}

vec3 Schlick_to_F0(vec3 f, float VdotH) {
    return Schlick_to_F0(f, vec3(1.0), VdotH);
}

float Schlick_to_F0(float f, float VdotH) {
    return Schlick_to_F0(f, 1.0, VdotH);
}

float V_GGX(float NdotL, float NdotV, float alpha)
{
    float alphaSq = alpha * alpha;

    float GGXV = NdotL * sqrt(NdotV * NdotV * (1.0 - alphaSq) + alphaSq);
    float GGXL = NdotV * sqrt(NdotL * NdotL * (1.0 - alphaSq) + alphaSq);

    float GGX = GGXV + GGXL;
    if (GGX > 0.0)
    {
        return 0.5 / GGX;
    }
    return 0.0;
}

float D_GGX(float NdotH, float alpha)
{
    float alphaSq = alpha * alpha;
    float f = (NdotH * NdotH) * (alphaSq - 1.0) + 1.0;
    return alphaSq / (PI * f * f);
}

vec3 BRDF_specularGGX(vec3 f0, vec3 f90, float alpha, float VdotH, float NdotL, float NdotV, float NdotH)
{
    vec3 F = F_Schlick(f0, f90, VdotH);
    float Vis = V_GGX(NdotL, NdotV, alpha);
    float D = D_GGX(NdotH, alpha);

    return F * Vis * D;
}

vec3 eval_lambertian(vec3 f0, vec3 f90, vec3 base_color, float VdotH)
{
    return (1.0 - F_Schlick(f0, f90, VdotH)) * (base_color / PI);
}

vec3 eval_clearcoat(vec3 N, vec3 V, vec3 L, vec3 H, float VdotH, vec3 f0, vec3 f90, vec3 clearcoat, float clearcoat_roughness)
{
    float NdotL = clamp(dot(N, L), 0.0, 1.0);
    float NdotV = clamp(dot(N, V), 0.0, 1.0);
    float NdotH = clamp(dot(N, H), 0.0, 1.0);
    return clearcoat * NdotL * BRDF_specularGGX(f0, f90, clearcoat_roughness * clearcoat_roughness, VdotH, NdotL, NdotV, NdotH);
}

vec3 eval_transmission(vec3 N, vec3 V, vec3 L, float alpha, vec3 f0, vec3 f90, vec3 base_color, float ior)
{
    float transmission_rougness = alpha * clamp(ior * 2.0 - 2.0, 0.0, 1.0);;

    vec3 R = normalize(L + 2.0 * N * dot(-L, N));
    vec3 H = normalize(R + L);

    float D = D_GGX(clamp(dot(N, H), 0.0, 1.0), transmission_rougness);
    vec3 F = F_Schlick(f0, f90, clamp(dot(V, H), 0.0, 1.0));
    float Vis = V_GGX(clamp(dot(N, R), 0.0, 1.0), clamp(dot(N, V), 0.0, 1.0), transmission_rougness);

    return (1.0 - F) * base_color * D * Vis;
}

vec3 eval_bsdf(vec3 N, vec3 V, vec3 L, vec3 base_color, float roughness, float metallic, vec3 clearcoat, float clearcoat_roughness)
{
    vec3 f0 = vec3(0.04);
    float ior = 1.5;
    roughness = clamp(roughness, 0.0, 1.0);
    metallic = clamp(metallic, 0.0, 1.0);
    float alpha = roughness * roughness;
    vec3 reflectance = f0;
    vec3 f90 = vec3(1.0);
    vec3 clearcoat_F0 = vec3(pow((ior - 1.0) / (ior + 1.0), 2.0));
    vec3 clearcoat_F90 = vec3(1.0);

    vec3 H = normalize(L + V);
    float NdotL = clamp(dot(N, L), 0.0, 1.0);
    float NdotV = clamp(dot(N, V), 0.0, 1.0);
    float NdotH = clamp(dot(N, H), 0.0, 1.0);
    float LdotH = clamp(dot(L, H), 0.0, 1.0);
    float VdotH = clamp(dot(V, H), 0.0, 1.0);

    vec3 val = vec3(0.0);
    
    if (NdotL > 0.0 || NdotV > 0.0)
    {
        val += NdotL * BRDF_specularGGX(f0, f90, alpha, VdotH, NdotL, NdotV, NdotH);
        val += eval_clearcoat(N, V, L, H, VdotH, clearcoat_F0, clearcoat_F90, clearcoat, clearcoat_roughness);
        val += eval_lambertian(f0, f90, base_color, VdotH);
    }

    return val;
}

#endif