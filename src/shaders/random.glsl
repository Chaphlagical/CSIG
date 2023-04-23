#ifndef RANDOM_GLSL
#define RANDOM_GLSL

float sample_blue_noise(ivec2 coord, int sample_index, int sample_dimension, sampler2D sobol_sequence_tex, sampler2D scrambling_ranking_tex)
{
	coord.x = coord.x % 128;
	coord.y = coord.y % 128;

	sample_index = sample_index % 256;
	sample_dimension = sample_dimension % 4;

	int ranked_sample_index = sample_index ^ int(clamp(texelFetch(scrambling_ranking_tex, ivec2(coord.x, coord.y), 0).b * 256.0, 0.0, 255.0));
	int value = int(clamp(texelFetch(sobol_sequence_tex, ivec2(ranked_sample_index, 0), 0)[sample_dimension] * 256.0, 0.0, 255.0));

	value = value ^ int(clamp(texelFetch(scrambling_ranking_tex, ivec2(coord.x, coord.y), 0)[sample_dimension % 2] * 256.0, 0.0, 255.0));

	float v = (0.5 + value) / 256.0;
	return v;
}

uint rng_rotl(uint x, uint k)
{
	return (x << k) | (x >> (32 - k));
}

uint rng_next(inout uvec2 rng)
{
	uint result = rng.x * 0x9e3779bb;
	rng.y ^= rng.x;
	rng.x = rng_rotl(rng.x, 26) ^ rng.y ^ (rng.y << 9);
	rng.y = rng_rotl(rng.y, 13);
	return result;
}

uint rng_hash(uint seed)
{
	seed = (seed ^ 61) ^ (seed >> 16);
	seed *= 9;
	seed = seed ^ (seed >> 4);
	seed *= 0x27d4eb2d;
	seed = seed ^ (seed >> 15);
	return seed;
}

uvec2 rng_init(uvec2 id, uint frameIndex)
{
	uint s0 = (id.x << 16) | id.y;
	uint s1 = frameIndex;

	uvec2 rng;
	rng.x = rng_hash(s0);
	rng.y = rng_hash(s1);
	rng_next(rng);
	return rng;
}

float next_float(inout uvec2 rng)
{
	uint u = 0x3f800000 | (rng_next(rng) >> 9);
	return uintBitsToFloat(u) - 1.0;
}

uint next_uint(inout uvec2 rng, uint nmax)
{
	float f = next_float(rng);
	return uint(floor(f * nmax));
}

vec2 next_vec2(inout uvec2 rng)
{
	return vec2(next_float(rng), next_float(rng));
}

vec3 next_vec3(inout uvec2 rng)
{
	return vec3(next_float(rng), next_float(rng), next_float(rng));
}

#endif