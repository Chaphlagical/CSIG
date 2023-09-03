#ifndef RANDOM_GLSL
#define RANDOM_GLSL

uint tea(in uint val0, in uint val1)
{
  uint v0 = val0;
  uint v1 = val1;
  uint s0 = 0;

  for(uint n = 0; n < 16; n++)
  {
    s0 += 0x9e3779b9;
    v0 += ((v1 << 4) + 0xa341316c) ^ (v1 + s0) ^ ((v1 >> 5) + 0xc8013ea4);
    v1 += ((v0 << 4) + 0xad90777d) ^ (v0 + s0) ^ ((v0 >> 5) + 0x7e95761e);
  }

  return v0;
}

uint init_random(in uvec2 resolution, in uvec2 screen_coord, in uint frame)
{
  return tea(screen_coord.y * resolution.x + screen_coord.x, frame);
}

uint pcg(inout uint state)
{
  uint prev = state * 747796405u + 2891336453u;
  uint word = ((prev >> ((prev >> 28u) + 4u)) ^ prev) * 277803737u;
  state     = prev;
  return (word >> 22u) ^ word;
}

uvec2 pcg2d(uvec2 v)
{
  v = v * 1664525u + 1013904223u;
  v.x += v.y * 1664525u;
  v.y += v.x * 1664525u;
  v = v ^ (v >> 16u);
  v.x += v.y * 1664525u;
  v.y += v.x * 1664525u;
  v = v ^ (v >> 16u);
  return v;
}

uvec3 pcg3d(uvec3 v)
{
  v = v * 1664525u + uvec3(1013904223u);
  v.x += v.y * v.z;
  v.y += v.z * v.x;
  v.z += v.x * v.y;
  v ^= v >> uvec3(16u);
  v.x += v.y * v.z;
  v.y += v.z * v.x;
  v.z += v.x * v.y;
  return v;
}

float rand(inout uint seed)
{
  uint r = pcg(seed);
  return uintBitsToFloat(0x3f800000 | (r >> 9)) - 1.0f;
}

vec2 rand2(inout uint prev)
{
  return vec2(rand(prev), rand(prev));
}

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

#endif