#ifndef BXDF_GLSL
#define BXDF_GLSL

#include "shade_state.glsl"
#include "random.glsl"

struct BSDFSample
{
	vec3 L;
	vec3 f;
	float pdf;
};

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
	float a = rgh;

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
	return 1.0 / (NdotV + max(sqrt(a + b - a * b), 0.0001));
}

float dielectric_fresnel(float cos_theta_i, float eta)
{
	float sinThetaTSq = eta * eta * (1.0 - cos_theta_i * cos_theta_i);

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
	float a = max(0.0001, pow(sstate.mat.roughness_factor, 2.0));
	float D = GTR2(dot(N, H), a);
	pdf     = D * dot(N, H) / (4.0 * dot(V, H));

	float FH = schlick_fresnel(dot(L, H));
	vec3  F  = mix(Cspec0, vec3(1.0), FH);
	float G  = SmithG_GGX(dot(N, L), a) * SmithG_GGX(dot(N, V), a);
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
	float a = max(0.0001, pow(sstate.mat.roughness_factor, 2.0));
	float F = dielectric_fresnel(dot(V, H), sstate.eta);
	float D = GTR2(dot(N, H), a);

	pdf = D * dot(N, H) * F / (4.0 * dot(V, H));

	float G = SmithG_GGX(abs(dot(N, L)), a) * SmithG_GGX(dot(N, V), a);
	return sstate.mat.base_color.rgb * F * D * G;
}

vec3 eval_dielectric_refraction(ShadeState sstate, vec3 V, vec3 N, vec3 L, vec3 H, inout float pdf)
{
	float a = max(0.0001, pow(sstate.mat.roughness_factor, 2.0));
	float F = dielectric_fresnel(abs(dot(V, H)), sstate.eta);
	float D = GTR2(dot(N, H), a);

	float denomSqrt = dot(L, H) * sstate.eta + dot(V, H);
	pdf             = D * dot(N, H) * (1.0 - F) * abs(dot(L, H)) / (denomSqrt * denomSqrt);

	float G = SmithG_GGX(abs(dot(N, L)), a) * SmithG_GGX(dot(N, V), a);
	return sstate.mat.base_color.rgb * (1.0 - F) * D * G * abs(dot(V, H)) * abs(dot(L, H)) * 4.0 * sstate.eta * sstate.eta / (denomSqrt * denomSqrt);
}

BSDFSample sample_bsdf(ShadeState sstate, vec3 V, inout uint seed)
{
	BSDFSample bs;
	bs.pdf = 0.0;

	float r1 = rand(seed);
	float r2 = rand(seed);

	float diffuse_ratio  = 0.5 * (1.0 - sstate.mat.metallic_factor);
  	float trans_weight   = (1.0 - sstate.mat.metallic_factor) * sstate.mat.transmission_factor;

	vec3 Cdlin = sstate.mat.base_color.rgb;
	vec3 Cspec0 = mix(vec3(0.0), Cdlin, sstate.mat.metallic_factor);

	if(rand(seed) < trans_weight)
	{
		// BSDF
		vec3 H = importance_sample_GTR2(sstate.mat.roughness_factor, r1, r2);
		H = normalize(sstate.tangent * H.x + sstate.bitangent * H.y + sstate.ffnormal * H.z);

		vec3  R = reflect(-V, H);
    	float F = dielectric_fresnel(abs(dot(R, H)), sstate.eta);

		// Reflection
		if(rand(seed) < F)   
		{
			bs.L = normalize(R); 
			bs.f = eval_dielectric_reflection(sstate, V, sstate.ffnormal, bs.L, H, bs.pdf);
		}
		else  // Transmission
		{
			bs.L = normalize(refract(-V, H, sstate.eta));
			bs.f = eval_dielectric_refraction(sstate, V, sstate.ffnormal, bs.L, H, bs.pdf);
		}

		bs.f *= trans_weight;
    	bs.pdf *= trans_weight;
	}
	else
	{
		// BRDF
		if(rand(seed) < diffuse_ratio)
		{
			// Diffuse
			vec3 L = cosine_sample_hemisphere(r1, r2);
			bs.L = sstate.tangent * L.x + sstate.bitangent * L.y + sstate.ffnormal * L.z;
			vec3 H = normalize(bs.L + V);
			bs.f = eval_diffuse(sstate, V, sstate.ffnormal, bs.L, H, bs.pdf);
			bs.pdf *= diffuse_ratio;
		}
		else
		{
			// Specular
			float primary_spec_ratio = 1.0 / (1.0 + sstate.mat.clearcoat_factor);
			// Sample primary specular lobe
			if(rand(seed) < primary_spec_ratio)
			{
				vec3 H = importance_sample_GTR2(sstate.mat.roughness_factor, r1, r2);
				H = sstate.tangent * H.x + sstate.bitangent * H.y + sstate.ffnormal * H.z;
				bs.L = normalize(reflect(-V, H));
				bs.f = eval_specular(sstate, Cspec0, V, sstate.normal, bs.L, H, bs.pdf);
				bs.pdf *= primary_spec_ratio * (1.0 - diffuse_ratio);
			}
			else  // Sample clearcoat lobe
			{
				vec3 H = importance_sample_GTR2(sstate.mat.clearcoat_roughness_factor, r1, r2);
				H = sstate.tangent * H.x + sstate.bitangent * H.y + sstate.ffnormal * H.z;
				bs.L = normalize(reflect(-V, H));
				bs.f = eval_clearcoat(sstate, V, sstate.normal, bs.L, H, bs.pdf);
				bs.pdf *= (1.0 - primary_spec_ratio) * (1.0 - diffuse_ratio);
			}
		}

		bs.f *= (1.0 - trans_weight);
    	bs.pdf *= (1.0 - trans_weight);
	}

	return bs;
}

vec3 eval_bsdf(ShadeState sstate, vec3 V, vec3 N, vec3 L, out float pdf)
{
	vec3 H;

	if(dot(N, L) < 0.0)
	{
		H = normalize(L * (1.0 / sstate.eta) + V);
	}
	else
	{
		H = normalize(L + V);
	}

	float diffuse_ratio     = 0.5 * (1.0 - sstate.mat.metallic_factor);
	float primary_spec_ratio = 1.0 / (1.0 + sstate.mat.clearcoat_factor);
	float trans_weight      = (1.0 - sstate.mat.metallic_factor) * sstate.mat.transmission_factor;

	vec3  brdf = vec3(0.0);
	vec3  bsdf = vec3(0.0);

	float brdf_pdf = 0.0;
	float bsdf_pdf = 0.0;

	// BSDF
	if(trans_weight > 0.0)
	{
		// Transmission
		if(dot(N, L) < 0.0)
		{
			bsdf = eval_dielectric_refraction(sstate, V, N, L, H, bsdf_pdf);
		}
		else  // Reflection
		{
			bsdf = eval_dielectric_reflection(sstate, V, N, L, H, bsdf_pdf);
		}
	}

	float m_pdf;

	if(trans_weight < 1.0)
	{
		vec3  Cdlin = sstate.mat.base_color.rgb;
		vec3 Cspec0 = mix(vec3(0.0), Cdlin, sstate.mat.metallic_factor);

		// Diffuse
		brdf += eval_diffuse(sstate, V, N, L, H, m_pdf);
		brdf_pdf += m_pdf * diffuse_ratio;

		// Specular
		brdf += eval_specular(sstate, Cspec0, V, N, L, H, m_pdf);
		brdf_pdf += m_pdf * primary_spec_ratio * (1.0 - diffuse_ratio);

		// Clearcoat
		brdf += eval_clearcoat(sstate, V, N, L, H, m_pdf);
		brdf_pdf += m_pdf * (1.0 - primary_spec_ratio) * (1.0 - diffuse_ratio);
	}

	pdf = mix(brdf_pdf, bsdf_pdf, trans_weight);
  	return mix(brdf, bsdf, trans_weight);
}

#endif