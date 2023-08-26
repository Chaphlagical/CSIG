#ifndef BLUENOISE_GLSL
#define BLUENOISE_GLSL

#include "random.glsl"

layout(set = 2, binding = 0) uniform sampler2D sobol_sequence;

layout(set = 2, binding = 1) uniform sampler2D scrambling_ranking_tile[];

vec2 next_sample(ivec2 coord, uint num_frames, uint rank)
{
    return vec2(sample_blue_noise(coord, int(num_frames), 0, sobol_sequence, scrambling_ranking_tile[rank]),
                sample_blue_noise(coord, int(num_frames), 1, sobol_sequence, scrambling_ranking_tile[rank]));
}

#endif