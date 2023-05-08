#ifndef BXDF_GLSL
#define BXDF_GLSL

#include "common.glsl"

vec3 cosine_sample_hemisphere(float r1, float r2)
{
	vec3  dir;
	float r   = sqrt(r1);
	float phi = 2.0 * PI * r2;
	dir.x     = r * cos(phi);
	dir.y     = r * sin(phi);
	dir.z     = sqrt(max(0.0, 1.0 - dir.x * dir.x - dir.y * dir.y));

	return dir;
}

vec3 importance_sample_GTR2(float rgh, float r1, float r2)
{
	float a = max(0.001, rgh);

	float phi = r1 * 2.0 * PI;

	float cosTheta = sqrt((1.0 - r2) / (1.0 + (a * a - 1.0) * r2));
	float sinTheta = clamp(sqrt(1.0 - (cosTheta * cosTheta)), 0.0, 1.0);
	float sinPhi   = sin(phi);
	float cosPhi   = cos(phi);

	return vec3(sinTheta * cosPhi, sinTheta * sinPhi, cosTheta);
}

float GTR1(float NdotH, float a)
{
	if (a >= 1.0)
	{
		return 1.0 / PI;
	}
	float a2 = a * a;
	float t  = 1.0 + (a2 - 1.0) * NdotH * NdotH;
	return (a2 - 1.0) / (PI * log(a2) * t);
}

float GTR2(float NdotH, float a)
{
	float a2 = a * a;
	float t  = 1.0 + (a2 - 1.0) * NdotH * NdotH;
	return a2 / (PI * t * t);
}

float schlick_fresnel(float u)
{
	float m  = clamp(1.0 - u, 0.0, 1.0);
	float m2 = m * m;
	return m2 * m2 * m;        // pow(m,5)
}

float SmithG_GGX(float NdotV, float alphaG)
{
	float a = alphaG * alphaG;
	float b = NdotV * NdotV;
	return 1.0 / (NdotV + sqrt(a + b - a * b));
}

float dielectric_fresnel(float cos_theta_i, float eta)
{
	float sinThetaTSq = eta * eta * (1.0f - cos_theta_i * cos_theta_i);

	// Total internal reflection
	if (sinThetaTSq > 1.0)
	{
		return 1.0;
	}

	float cos_theta_t = sqrt(max(1.0 - sinThetaTSq, 0.0));

	float rs = (eta * cos_theta_t - cos_theta_i) / (eta * cos_theta_t + cos_theta_i);
	float rp = (eta * cos_theta_i - cos_theta_t) / (eta * cos_theta_i + cos_theta_t);

	return 0.5 * (rs * rs + rp * rp);
}

vec3 eval_diffuse(ShadeState sstate, vec3 V, vec3 N, vec3 L, vec3 H, inout float pdf)
{
	if (dot(N, L) < 0.0)
	{
		return vec3(0.0);
	}

	pdf = dot(N, L) * (1.0 / PI);

	float FL   = schlick_fresnel(dot(N, L));
	float FV   = schlick_fresnel(dot(N, V));
	float FH   = schlick_fresnel(dot(L, H));
	float Fd90 = 0.5 + 2.0 * dot(L, H) * dot(L, H) * sstate.mat.roughness_factor;
	float Fd   = mix(1.0, Fd90, FL) * mix(1.0, Fd90, FV);

	return ((1.0 / PI) * Fd * sstate.mat.base_color.rgb) * (1.0 - sstate.mat.metallic_factor);
}

vec3 eval_specular(ShadeState sstate, vec3 Cspec0, vec3 V, vec3 N, vec3 L, vec3 H, inout float pdf)
{
	if (dot(N, L) < 0.0)
	{
		return vec3(0.0);
	}

	float D = GTR2(dot(N, H), sstate.mat.roughness_factor);
	pdf     = D * dot(N, H) / (4.0 * dot(V, H));

	float FH = schlick_fresnel(dot(L, H));
	vec3  F  = mix(Cspec0, vec3(1.0), FH);
	float G  = SmithG_GGX(dot(N, L), sstate.mat.roughness_factor) * SmithG_GGX(dot(N, V), sstate.mat.roughness_factor);
	return F * D * G;
}

vec3 eval_clearcoat(ShadeState sstate, vec3 V, vec3 N, vec3 L, vec3 H, inout float pdf)
{
	if (dot(N, L) < 0.0)
	{
		return vec3(0.0);
	}

	float D = GTR1(dot(N, H), sstate.mat.clearcoat_roughness_factor);
	pdf     = D * dot(N, H) / (4.0 * dot(V, H));

	float FH = schlick_fresnel(dot(L, H));
	float F  = mix(0.04, 1.0, FH);
	float G  = SmithG_GGX(dot(N, L), 0.25) * SmithG_GGX(dot(N, V), 0.25);
	return vec3(0.25 * sstate.mat.clearcoat_factor * F * D * G);
}

vec3 eval_dielectric_reflection(ShadeState sstate, vec3 V, vec3 N, vec3 L, vec3 H, inout float pdf)
{
	if (dot(N, L) < 0.0)
	{
		return vec3(0.0);
	}

	float F = dielectric_fresnel(dot(V, H), sstate.eta);
	float D = GTR2(dot(N, H), sstate.mat.roughness_factor);

	pdf = D * dot(N, H) * F / (4.0 * dot(V, H));

	float G = SmithG_GGX(abs(dot(N, L)), sstate.mat.roughness_factor) * SmithG_GGX(dot(N, V), sstate.mat.roughness_factor);
	return sstate.mat.base_color.rgb * F * D * G;
}

vec3 eval_dielectric_refraction(ShadeState sstate, vec3 V, vec3 N, vec3 L, vec3 H, inout float pdf)
{
	float F = dielectric_fresnel(abs(dot(V, H)), sstate.eta);
	float D = GTR2(dot(N, H), sstate.mat.roughness_factor);

	float denomSqrt = dot(L, H) * sstate.eta + dot(V, H);
	pdf             = D * dot(N, H) * (1.0 - F) * abs(dot(L, H)) / (denomSqrt * denomSqrt);

	float G = SmithG_GGX(abs(dot(N, L)), sstate.mat.roughness_factor) * SmithG_GGX(dot(N, V), sstate.mat.roughness_factor);
	return sstate.mat.base_color.rgb * (1.0 - F) * D * G * abs(dot(V, H)) * abs(dot(L, H)) * 4.0 * sstate.eta * sstate.eta / (denomSqrt * denomSqrt);
}

#endif