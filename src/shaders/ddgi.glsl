#ifndef DDGI_GLSL
#define DDGI_GLSL

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

ivec3 probe_index_to_grid_coord(int index, DDGIUniform ddgi_buffer)
{
    ivec3 pos;

    pos.x = index % ddgi_buffer.probe_count.x;
    pos.y = (index % (ddgi_buffer.probe_count.x * ddgi_buffer.probe_count.y)) / ddgi_buffer.probe_count.x;
    pos.z = index / (ddgi_buffer.probe_count.x * ddgi_buffer.probe_count.y);

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

#endif