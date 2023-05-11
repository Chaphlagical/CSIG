#ifndef RESTIR_COMMON_GLSL
#define RESTIR_COMMON_GLSL

#include "random.glsl"

#define RESERVOIR_SIZE 1

struct RestirSample
{
    uint light_id;
};

struct Reservoir
{
    RestirSample y;   // The output sample
    uint M; // The number of samples seen so far
    float w_sum;    // The sum of weights
    float W;
    float pdf;
    float p_hat
};

void init_reservoir(inout Reservoir reservoir)
{
    reservoir.w_sum = 0;
    reservoir.W = 0;
    reservoir.M = 0;
}

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