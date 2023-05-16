#ifndef RESTIR_COMMON_GLSL
#define RESTIR_COMMON_GLSL

#include "random.glsl"
#include "shade_state.glsl"
#include "bxdf.glsl"
#include "scene.glsl"
#include "raytrace.glsl"

#define RESERVOIR_SIZE 1

layout(buffer_reference, scalar, buffer_reference_align = 4) buffer RestirReservoirBuffer
{
    Reservoir reservoirs[];
};

Reservoir init_reservoir()
{
    Reservoir reservoir;

    reservoir.light_id = -1;
    reservoir.p_hat = 0;
    reservoir.sum_weights = 0;
    reservoir.w = 0;
    reservoir.num_samples = 0;

    return reservoir;
}

vec3 eval_L(uint light_id, ShadeState sstate)
{
    LightSample ls = sample_light_idx(sstate, light_id);
    vec3 wo = normalize(ubo.cam_pos.xyz - sstate.position);
    float bsdf_pdf;
    vec3 f = eval_bsdf(sstate, wo, sstate.ffnormal, ls.dir, bsdf_pdf);
    float geo_term = abs(dot(ls.norm, -ls.dir)) * abs(dot(sstate.ffnormal, ls.dir)) / (ls.dist * ls.dist);
    return ls.le * f * abs(dot(sstate.ffnormal, ls.dir));
}

float eval_phat(uint light_id, ShadeState sstate)
{
    return luminance(eval_L(light_id, sstate));
}

void update_reservoir(inout Reservoir res, int light_id, float weight, float p_hat, float w) 
{
    res.sum_weights += weight;
    float prob = weight / res.sum_weights;
    if(rand(prd.seed) < prob)
    {
        res.light_id = light_id;
        res.p_hat = p_hat;
        res.w = w;
    }
}

void add_sample_to_reservoir(inout Reservoir res, int light_id, float light_pdf, float p_hat)
{
    float weight = p_hat / light_pdf;
    res.num_samples += 1;
    float w = (res.sum_weights + weight) / (res.num_samples * p_hat);
    update_reservoir(res, light_id, weight, p_hat, w);
}

void combine_reservoir(inout Reservoir self, Reservoir other, float p_hat)
{
    self.num_samples += other.num_samples;
    float weight = p_hat * other.w * other.num_samples;
    if(weight > 0)
    {
        update_reservoir(self, other.light_id, weight, p_hat, other.w);
    }
    if(self.w > 0)
    {
        self.w = self.sum_weights / (self.num_samples * self.p_hat);
    }
}

Reservoir get_reservoir(uint64_t addr, uint id)
{
    return RestirReservoirBuffer(addr).reservoirs[id];
}

#endif