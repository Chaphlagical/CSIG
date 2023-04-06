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

#endif