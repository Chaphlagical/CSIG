#define CACHE_SIZE 64

#ifdef UPDATE_DEPTH
    #define NUM_THREADS_X 16
    #define NUM_THREADS_Y 16
    #define TEXTURE_WIDTH ddgi_buffer.depth_texture_width
    #define TEXTURE_HEIGHT ddgi_buffer.depth_texture_height
    #define PROBE_SIDE_LENGTH ddgi_buffer.depth_probe_side_length
#else
    #define NUM_THREADS_X 8
    #define NUM_THREADS_Y 8
    #define TEXTURE_WIDTH ddgi_buffer.irradiance_texture_width
    #define TEXTURE_HEIGHT ddgi_buffer.irradiance_texture_height
    #define PROBE_SIDE_LENGTH ddgi_buffer.irradiance_probe_side_length
#endif

layout(local_size_x = NUM_THREADS_X, local_size_y = NUM_THREADS_Y, local_size_z = 1) in;

layout(binding = 0, rgba16f) uniform image2D output_irradiance;
layout(binding = 1, rg16f) uniform image2D output_depth;

layout(binding = 2) uniform sampler2D input_irradiance;
layout(binding = 3) uniform sampler2D input_depth;

layout(binding = 4) uniform sampler2D input_radiance;
layout(binding = 5) uniform sampler2D input_direction_depth;

layout(binding = 6) uniform DDGIBuffer
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
} ddgi_buffer;

layout(push_constant) uniform PushConstants
{
    uint frame_count;
}push_constants;

const float FLT_EPS = 0.00000001;

shared vec4 shared_ray_direction_depth[CACHE_SIZE];
#ifndef UPDATE_DEPTH
shared vec3 shared_ray_hit_radiance[CACHE_SIZE];
#endif

float sign_not_zero(in float k)
{
    return (k >= 0.0) ? 1.0 : -1.0;
}

vec2 sign_not_zero(in vec2 v)
{
    return vec2(sign_not_zero(v.x), sign_not_zero(v.y));
}

uint probe_id(vec2 texel_xy)
{
    uint probe_with_border_side = PROBE_SIDE_LENGTH + 2;
    uint probes_per_side = (TEXTURE_WIDTH - 2) / probe_with_border_side;
    return uint(texel_xy.x / probe_with_border_side) + probes_per_side * int(texel_xy.y / probe_with_border_side);
}

vec2 normalized_oct_coord(ivec2 frag_coord)
{
    int probe_with_border_side = int(PROBE_SIDE_LENGTH + 2);
    vec2 oct_frag_coord = ivec2((frag_coord.x - 2) % probe_with_border_side, (frag_coord.y - 2) % probe_with_border_side);
    return (vec2(oct_frag_coord) + vec2(0.5f)) * (2.0f / float(PROBE_SIDE_LENGTH)) - vec2(1.0f, 1.0f);
}

vec3 oct_decode(vec2 o)
{
    vec3 v = vec3(o.x, o.y, 1.0 - abs(o.x) - abs(o.y));
    if (v.z < 0.0)
    {
        v.xy = (1.0 - abs(v.yx)) * sign_not_zero(v.xy);
    }
    return normalize(v);
}

void populate_cache(uint relative_probe_id, uint offset, uint num_rays)
{
    if (gl_LocalInvocationIndex < num_rays)
    {
        ivec2 C = ivec2(offset + uint(gl_LocalInvocationIndex), int(relative_probe_id));

        shared_ray_direction_depth[gl_LocalInvocationIndex] = texelFetch(input_direction_depth, C, 0);
#ifndef UPDATE_DEPTH
        shared_ray_hit_radiance[gl_LocalInvocationIndex] = texelFetch(input_radiance, C, 0).xyz;
#endif
    }
}

void gather_rays(ivec2 current_coord, uint num_rays, inout vec3 result, inout float total_weight)
{
    const float energy_conservation = 0.95;

    for (int r = 0; r < num_rays; ++r)
    {
        vec4 ray_direction_depth = shared_ray_direction_depth[r];
        vec3 ray_direction = ray_direction_depth.xyz;
#ifdef UPDATE_DEPTH
        float ray_probe_distance = min(ddgi_buffer.max_distance, ray_direction_depth.w - 0.01);
        if (ray_probe_distance == -1.0)
        {
            ray_probe_distance = ddgi_buffer.max_distance;
        }
#else
        vec3 ray_hit_radiance = shared_ray_hit_radiance[r] * energy_conservation;
#endif
        vec3 texel_direction = oct_decode(normalized_oct_coord(current_coord));
#ifdef UPDATE_DEPTH
        float weight = pow(max(0.0, dot(texel_direction, ray_direction)), ddgi_buffer.depth_sharpness);
#else
        float weight = max(0.0, dot(texel_direction, ray_direction));
#endif
        if (weight >= FLT_EPS)
        {
#ifdef UPDATE_DEPTH
            result += vec3(ray_probe_distance * weight, pow(ray_probe_distance, 2) * weight, 0.0);
#else
            result += vec3(ray_hit_radiance * weight);
#endif
            total_weight += weight;
        }
    }
}

void main()
{
    const ivec2 current_coord = ivec2(gl_GlobalInvocationID.xy) + (ivec2(gl_WorkGroupID.xy) * ivec2(2)) + ivec2(2);
    const uint relative_probe_id = probe_id(current_coord);
    
    vec3 result = vec3(0.0);
    float total_weight = 0.0;

    uint remaining_rays = ddgi_buffer.rays_per_probe;
    uint offset = 0;

    while (remaining_rays > 0)
    {
        uint num_rays = min(CACHE_SIZE, remaining_rays);
        populate_cache(relative_probe_id, offset, num_rays);
        barrier();
        gather_rays(current_coord, num_rays, result, total_weight);
        barrier();
        remaining_rays -= num_rays;
        offset += num_rays;
    }

    if (total_weight > FLT_EPS)
    {
        result /= total_weight;
    }

    // Temporal Accumulation
    vec3 prev_result;

#ifdef UPDATE_DEPTH
    prev_result = texelFetch(input_depth, current_coord, 0).rgb;
#else
    prev_result = texelFetch(input_irradiance, current_coord, 0).rgb;
#endif

    if(push_constants.frame_count != 0)
    {
        result = mix(result, prev_result, ddgi_buffer.hysteresis);
    }
    
#ifdef UPDATE_DEPTH
    imageStore(output_depth, current_coord, vec4(result, 1.0));
#else
    imageStore(output_irradiance, current_coord, vec4(result, 1.0));
#endif 
}