#ifndef EDGE_STOPPING_GLSL
#define EDGE_STOPPING_GLSL

float normal_edge_stopping_weight(vec3 center_normal, vec3 sample_normal, float power)
{
    return pow(clamp(dot(center_normal, sample_normal), 0.0, 1.0), power);
}

float depth_edge_stopping_weight(float center_depth, float sample_depth, float phi)
{
    return exp(-abs(center_depth - sample_depth) / phi);
}

float luma_edge_stopping_weight(float center_luma, float sample_luma, float phi)
{
    return abs(center_luma - sample_luma) / phi;
}

float compute_edge_stopping_weight(
    float center_depth,
    float sample_depth,
    float phi_z
#ifdef USE_EDGE_STOPPING_NORMAL_WEIGHT
    ,vec3  center_normal,
    vec3  sample_normal,
    float phi_normal
#endif
#ifdef USE_EDGE_STOPPING_LUMA_WEIGHT
    ,float center_luma,
    float sample_luma,
    float phi_luma
#endif
)
{
    const float w_z = depth_edge_stopping_weight(center_depth, sample_depth, phi_z);

#ifdef USE_EDGE_STOPPING_NORMAL_WEIGHT
    const float w_normal = normal_edge_stopping_weight(center_normal, sample_normal, phi_normal);
#else
    const float w_normal = 1.0f;
#endif

#ifdef USE_EDGE_STOPPING_LUMA_WEIGHT
    const float w_l = luma_edge_stopping_weight(center_luma, sample_luma, phi_luma);
#else 
    const float w_l = 1.0f;
#endif

    const float w = exp(0.0 - max(w_l, 0.0) - max(w_z, 0.0)) * w_normal;

    return w;
}

#endif