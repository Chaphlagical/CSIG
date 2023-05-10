#ifndef DDGI_GLSL
#define DDGI_GLSL

#include "common.glsl"

struct DDGIUniform
{
    vec3 grid_start;
    float max_distance;
    vec3 grid_step;
    float depth_sharpness;
    ivec3 probe_count;
    float hysteresis;
    float normal_bias;
    float energy_preservation;
    uint rays_per_probe;
    uint visibility_test;
    uint irradiance_probe_side_length;
    uint irradiance_texture_width;
    uint irradiance_texture_height;
    uint depth_probe_side_length;
    uint depth_texture_width;
    uint depth_texture_height;
};

ivec3 base_grid_coord(vec3 X, DDGIUniform ddgi) 
{
    return clamp(ivec3((X - ddgi.grid_start) / ddgi.grid_step), ivec3(0, 0, 0), ivec3(ddgi.probe_count) - ivec3(1, 1, 1));
}

vec3 grid_coord_to_position(ivec3 c, DDGIUniform ddgi)
{
    return ddgi.grid_step * vec3(c) + ddgi.grid_start;
}

int grid_coord_to_probe_index(ivec3 probe_coord, DDGIUniform ddgi) 
{
    return int(probe_coord.x + probe_coord.y * ddgi.probe_count.x + probe_coord.z * ddgi.probe_count.x * ddgi.probe_count.y);
}

vec3 grid_coord_to_offset(ivec3 c, DDGIUniform ddgi, sampler2D probe_data)
{
    return texelFetch(probe_data, ivec2(grid_coord_to_probe_index(c, ddgi), 0), 0).xyz;
}

int grid_coord_to_state(ivec3 c, DDGIUniform ddgi, sampler2D probe_data)
{
    return int(texelFetch(probe_data, ivec2(grid_coord_to_probe_index(c, ddgi), 0), 0).w);
}

ivec3 probe_index_to_grid_coord(int index, DDGIUniform ddgi)
{
    ivec3 pos;

    pos.x = index % ddgi.probe_count.x;
    pos.y = (index % (ddgi.probe_count.x * ddgi.probe_count.y)) / ddgi.probe_count.x;
    pos.z = index / (ddgi.probe_count.x * ddgi.probe_count.y);

    return pos;
}

float sign_not_zero(in float k)
{
    return (k >= 0.0) ? 1.0 : -1.0;
}

vec2 sign_not_zero(in vec2 v)
{
    return vec2(sign_not_zero(v.x), sign_not_zero(v.y));
}

vec2 oct_encode(in vec3 v) 
{
    float l1norm = abs(v.x) + abs(v.y) + abs(v.z);
    vec2 result = v.xy * (1.0 / l1norm);
    if (v.z < 0.0)
    {
        result = (1.0 - abs(result.yx)) * sign_not_zero(result.xy);
    }
    return result;
}

vec2 texture_coord_from_direction(vec3 dir, int probe_index, uint width, uint height, uint probe_side_length) 
{
    vec2 normalized_oct_coord = oct_encode(normalize(dir));
    vec2 normalized_oct_coord_zero_one = (normalized_oct_coord + vec2(1.0)) * 0.5;

    float probe_with_border_side = float(probe_side_length) + 2.0;

    vec2 oct_coord_normalized_to_texture_dimensions = (normalized_oct_coord_zero_one * float(probe_side_length)) / vec2(float(width), float(height));

    int probes_per_row = (int(width) - 2) / int(probe_with_border_side);

    vec2 probe_top_left_position = vec2(mod(probe_index, probes_per_row) * probe_with_border_side,
        (probe_index / probes_per_row) * probe_with_border_side) + vec2(2.0, 2.0);

    vec2 normalized_probe_top_left_position = vec2(probe_top_left_position) / vec2(float(width), float(height));

    return vec2(normalized_probe_top_left_position + oct_coord_normalized_to_texture_dimensions);
}

vec3 sample_irradiance(vec3 P, vec3 N, vec3 V, DDGIUniform ddgi, sampler2D probe_depth, sampler2D probe_irradiance)
{
    ivec3 base_grid_coord = base_grid_coord(P, ddgi);
    vec3 base_probe_pos = grid_coord_to_position(base_grid_coord, ddgi);

    vec3 sum_irradiance = vec3(0.0);
    float sum_weight = 0.0;

    vec3 alpha = clamp((P - base_probe_pos) / ddgi.grid_step, vec3(0.0), vec3(1.0));

    for (int i = 0; i < 8; ++i) 
    {
        ivec3 offset = ivec3(i, i >> 1, i >> 2) & ivec3(1);
        ivec3 probe_grid_coord = clamp(base_grid_coord + offset, ivec3(0), ddgi.probe_count - ivec3(1));
        int p = grid_coord_to_probe_index(probe_grid_coord, ddgi);
        vec3 probe_pos = grid_coord_to_position(probe_grid_coord, ddgi);
        vec3 probe_to_point = P - probe_pos + (N + 3.0 * V) * ddgi.normal_bias;
        vec3 dir = normalize(-probe_to_point);
        vec3 trilinear = mix(1.0 - alpha, alpha, offset);
        float weight = 1.0;
        vec3 true_direction_to_probe = normalize(probe_pos - P);
        weight *= pow(max(0.0001, (dot(true_direction_to_probe, N) + 1.0) * 0.5), 2.0) + 0.2;
        if (ddgi.visibility_test == 1)
        {
            vec2 tex_coord = texture_coord_from_direction(-dir, p, ddgi.depth_texture_width, ddgi.depth_texture_height, ddgi.depth_probe_side_length);

            float dist_to_probe = length(probe_to_point);

            vec2 temp = textureLod(probe_depth, tex_coord, 0.0f).rg;
            float mean = temp.x;
            float variance = abs(pow(temp.x, 2.0) - temp.y);

            float chebyshev_weight = variance / (variance + pow(max(dist_to_probe - mean, 0.0), 2.0));

            chebyshev_weight = max(pow(chebyshev_weight, 3.0), 0.0);

            weight *= (dist_to_probe <= mean) ? 1.0 : chebyshev_weight;
        }

        weight = max(0.000001, weight);
                 
        vec3 irradiance_dir = dot(N, V) > 0 ? N : -N;

        vec2 tex_coord = texture_coord_from_direction(normalize(irradiance_dir), p, ddgi.irradiance_texture_width, ddgi.irradiance_texture_height, ddgi.irradiance_probe_side_length);

        vec3 probe_irradiance = textureLod(probe_irradiance, tex_coord, 0.0).rgb;

        const float crush_threshold = 0.2;
        if (weight < crush_threshold)
        {
            weight *= weight * weight * (1.0 / pow(crush_threshold, 2.0)); 
        }

        weight *= trilinear.x * trilinear.y * trilinear.z;

        sum_irradiance += weight * probe_irradiance;
        sum_weight += weight;
    }

    vec3 net_irradiance = sum_irradiance / sum_weight;
    
    net_irradiance.x = isnan(net_irradiance.x) ? 0.5 : net_irradiance.x;
    net_irradiance.y = isnan(net_irradiance.y) ? 0.5 : net_irradiance.y;
    net_irradiance.z = isnan(net_irradiance.z) ? 0.5 : net_irradiance.z;

    net_irradiance *= ddgi.energy_preservation;

    return 0.5 * PI * net_irradiance;
}

#endif