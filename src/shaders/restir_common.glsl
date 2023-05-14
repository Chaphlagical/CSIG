#ifndef RESTIR_COMMON_GLSL
#define RESTIR_COMMON_GLSL

#include "random.glsl"
#include "shade_state.glsl"
#include "bxdf.glsl"
#include "scene.glsl"
#include "raytrace.glsl"

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
};

layout(buffer_reference, scalar) buffer RestirReservoirBuffer
{
    Reservoir reservoirs[];
};

Reservoir init_reservoir()
{
    Reservoir reservoir;

    reservoir.w_sum = 0;
    reservoir.W = 0;
    reservoir.M = 0;

    return reservoir;
}

void update_reservoir(inout Reservoir reservoir, RestirSample x, float w)
{
    reservoir.w_sum += w;
    reservoir.M += 1;
    if(rand(prd.seed) < w / reservoir.w_sum)
    {
        reservoir.y = x;
    }
}

vec3 compute_L(const Reservoir r, ShadeState sstate) 
{
    Ray ray;
    ray.origin = ubo.cam_pos.xyz;
    ray.direction = normalize(sstate.position - ubo.cam_pos.xyz);
    LightSample ls = sample_light_idx(sstate, r.y.light_id);
    float bsdf_pdf;
    vec3 f = eval_bsdf(sstate, -ray.direction, sstate.ffnormal, ls.dir, bsdf_pdf);
    return f * ls.le * abs(dot(ls.norm, -ls.dir)) * abs(dot(sstate.ffnormal, ls.dir)) / (ls.dist * ls.dist);
}

vec3 compute_L_with_visibility(const Reservoir r, ShadeState sstate) 
{
    Ray ray;
    ray.origin = ubo.cam_pos.xyz;
    ray.direction = normalize(sstate.position - ubo.cam_pos.xyz);
    LightSample ls = sample_light_idx(sstate, r.y.light_id);
    Ray shadow_ray;
    shadow_ray.origin = offset_ray(sstate.position, dot(ls.dir, sstate.ffnormal) > 0 ? sstate.ffnormal : -sstate.ffnormal);
    shadow_ray.direction = ls.dir;
    if(!any_hit(shadow_ray, length(shadow_ray.origin - ls.pos)))
    {
        float bsdf_pdf;
        vec3 f = eval_bsdf(sstate, -ray.direction, sstate.ffnormal, ls.dir, bsdf_pdf);
        return f * ls.le * abs(dot(ls.norm, -ls.dir)) * abs(dot(sstate.ffnormal, ls.dir)) / (ls.dist * ls.dist);
    }
    return vec3(0);
}

float compute_p_hat(const Reservoir r_new, ShadeState sstate) 
{ 
    return luminance(compute_L(r_new, sstate)); 
}

void combine_reservoir(inout Reservoir r1, const Reservoir r2, ShadeState sstate) {
    float fac = r2.W * r2.M;
    if (fac > 0) 
    {
        fac *= compute_p_hat(r2, sstate);
        update_reservoir(r1, r2.y, fac);
    }
}

Reservoir get_reservoir(uint64_t addr, uint id)
{
    return RestirReservoirBuffer(addr).reservoirs[id];
}

#endif