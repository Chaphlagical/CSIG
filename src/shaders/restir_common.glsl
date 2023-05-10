#ifndef RESTIR_COMMON_GLSL
#define RESTIR_COMMON_GLSL

#include "random.glsl"

#define RESERVOIR_SIZE 1

struct EmitterSample
{
    vec3 position;
    uint emitter_id;
    vec3 normal;
    float weight;
};

struct Reservoir
{
    EmitterSample y;   // The output sample
    uint M; // The number of samples seen so far
    float w_sum;    // The sum of weights
    float W;
};

void update_reservoir(EmitterSample x, float w, uint seed, inout Reservoir reservoir)
{
    reservoir.w_sum += w;
    reservoir.M += 1;
    if(rand(seed) < w / reservoir.w_sum)
    {
        reservoir.y = x;
    }
}

#endif