#ifndef REPROJECT_GLSL
#define REPROJECT_GLSL

#include "common.glsl"

#define NORMAL_DISTANCE 0.1
#define PLANE_DISTANCE 5.0

bool plane_distance_disocclusion_check(vec3 current_pos, vec3 prev_pos, vec3 current_normal, vec3 prev_normal)
{
    vec3 to_current = current_pos - prev_pos;
    float dist_to_plane = abs(dot(to_current, current_normal));
    return dist_to_plane > PLANE_DISTANCE;
}

bool out_of_frame_disocclusion_check(ivec2 coord, ivec2 image_size)
{
    return any(lessThan(coord, ivec2(0, 0))) || any(greaterThan(coord, image_size - ivec2(1, 1)));
}

bool instance_id_disocclusion_check(uint instance_id, uint prev_instance_id)
{
    return instance_id != prev_instance_id;
}

bool normals_disocclusion_check(vec3 current_normal, vec3 prev_normal)
{
    return pow(abs(dot(current_normal, prev_normal)), 2) <= NORMAL_DISTANCE;
}

bool is_reprojection_valid(
    ivec2 coord, 
    vec3 current_pos, vec3 prev_pos, 
    vec3 current_normal, vec3 prev_normal,
    uint current_instance_id, uint prev_instance_id, 
    ivec2 image_size)
{
    if(out_of_frame_disocclusion_check(coord, image_size))
    {
        return false;
    }

    if(instance_id_disocclusion_check(current_instance_id, prev_instance_id))
    {
        return false;
    }

    if(plane_distance_disocclusion_check(current_pos, prev_pos, current_normal, prev_normal))
    {
        return false;
    }

    if(normals_disocclusion_check(current_normal, prev_normal))
    {
        return false;
    }

    return true;
}

vec2 surface_point_reprojection(ivec2 coord, vec2 motion_vector, ivec2 size)
{
    return vec2(coord) + motion_vector.xy * vec2(size);
}

vec2 virtual_point_reprojection(ivec2 current_coord, ivec2 size, float depth, float ray_length, vec3 cam_pos, mat4 view_proj_inverse, mat4 prev_view_proj)
{
    const vec2 tex_coord  = current_coord / vec2(size);
    vec3       ray_origin = world_position_from_depth(tex_coord, depth, view_proj_inverse);

    vec3 camera_ray = ray_origin - cam_pos.xyz;

    float camera_ray_length = length(camera_ray);
    float reflection_ray_length = ray_length;

    camera_ray = normalize(camera_ray);

    vec3 parallax_hit_point = cam_pos.xyz + camera_ray * (camera_ray_length + reflection_ray_length);

    vec4 reprojected_parallax_hit_point = prev_view_proj * vec4(parallax_hit_point, 1.0f);

    reprojected_parallax_hit_point.xy /= reprojected_parallax_hit_point.w;

    return (reprojected_parallax_hit_point.xy * 0.5 + 0.5) * vec2(size);
}

vec2 compute_history_coord(ivec2 current_coord, ivec2 size, float depth, vec2 motion, float curvature, float ray_length, vec3 cam_pos, mat4 view_proj_inverse, mat4 prev_view_proj)
{
    const vec2 surface_history_coord = surface_point_reprojection(current_coord, motion, size);

    vec2 history_coord = surface_history_coord;

    if (ray_length > 0.0 && curvature == 0.0)
    {
        history_coord = virtual_point_reprojection(current_coord, size, depth, ray_length, cam_pos, view_proj_inverse, prev_view_proj);
    }

    return history_coord;
}

bool reprojection(
    in ivec2 frag_coord,
    in float depth,
    in int g_buffer_mip,
#ifdef REPROJECTION_REFLECTIONS
    in vec3 cam_pos,
#endif
    in mat4 view_projection_inv,
#ifdef REPROJECTION_REFLECTIONS
    in mat4 prev_view_proj,
    in float ray_length,
#endif
    in sampler2D sampler_gbufferB,
    in sampler2D sampler_gbufferC,
    in sampler2D sampler_prev_gbufferB,
    in sampler2D sampler_prev_gbufferC,
    in sampler2D sampler_prev_depth,
    in sampler2D sampler_history_output,
#ifdef REPROJECTION_MOMENTS
    in sampler2D sampler_history_moments_length,
#else 
    in sampler2D sampler_history_length,
#endif
#ifdef REPROJECTION_SINGLE_COLOR_CHANNEL
    out float history_color,
#else
    out vec3  history_color, 
#endif
#ifdef REPROJECTION_MOMENTS
    out vec2 history_moments, 
#endif
    out float history_length)
{
    const vec2 image_size = vec2(textureSize(sampler_history_output, 0));
    const vec2 pixel_center = vec2(frag_coord) + vec2(0.5);
    const vec2 tex_coord = pixel_center / image_size;

    const vec4 gbufferB_data = texelFetch(sampler_gbufferB, frag_coord, g_buffer_mip);
    const vec4 gbufferC_data = texelFetch(sampler_gbufferC, frag_coord, g_buffer_mip);

    const vec2 current_motion  = gbufferB_data.zw;
    const vec3 current_normal = octohedral_to_direction(gbufferB_data.xy);
    const uint current_instance_id = uint(gbufferC_data.z);
    const vec3 current_pos = world_position_from_depth(tex_coord, depth, view_projection_inv);

#ifdef REPROJECTION_REFLECTION
    const float curvature = gbufferC_data.g;
    const vec2 vec2 history_tex_coord = tex_coord + current_motion;
    const vec2 reprojected_coord = compute_history_coord(frag_coord, 
                                                         ivec2(image_dim), 
                                                         depth, 
                                                         current_motion, 
                                                         curvature, 
                                                         ray_length, 
                                                         cam_pos, 
                                                         view_proj_inverse, 
                                                         prev_view_proj);
    const ivec2 history_coord = ivec2(reprojected_coord);                                                      
    const vec2  history_coord_floor = reprojected_coord;             
// TODO
#else
    const ivec2 history_coord = ivec2(vec2(frag_coord) + current_motion.xy * image_size + vec2(0.5));
    const vec2 history_coord_floor = floor(vec2(frag_coord.xy)) + current_motion.xy * image_size;
    const vec2 history_tex_coord = tex_coord + current_motion.xy;
#endif

#ifdef REPROJECTION_SINGLE_COLOR_CHANNEL
    history_color = 0.0;
#else
    history_color = vec3(0.0);
#endif
#ifdef REPROJECTION_MOMENTS
    history_moments = vec2(0.0f);
#endif

    bool v[4];
    const ivec2 offset[4] = { ivec2(0, 0), ivec2(1, 0), ivec2(0, 1), ivec2(1, 1) };

    // check for all 4 taps of the bilinear filter for validity
    bool valid = false;
    for(int sample_idx = 0; sample_idx < 4; sample_idx++)
    {
        ivec2 loc = ivec2(history_coord_floor) + offset[sample_idx];

        vec4 prev_gbufferB_data = texelFetch(sampler_prev_gbufferB, loc, g_buffer_mip);
        vec4 prev_gbufferC_data = texelFetch(sampler_prev_gbufferC, loc, g_buffer_mip);
        float prev_depth = texelFetch(sampler_prev_depth, loc, g_buffer_mip).r;

        vec3 prev_normal = octohedral_to_direction(gbufferB_data.xy);
        uint prev_instance_id = uint(gbufferC_data.z);
        vec3 prev_pos = world_position_from_depth(history_tex_coord, prev_depth, view_projection_inv);

        v[sample_idx] = is_reprojection_valid(history_coord, current_pos, prev_pos, current_normal, prev_normal, current_instance_id, prev_instance_id, ivec2(image_size));

        valid = valid || v[sample_idx];
    }

    if(valid)
    {
        float sum_w = 0.0;
        float x = fract(history_coord_floor.x);
        float y = fract(history_coord_floor.y);

        float w[4] = { (1 - x) * (1 - y), x * (1 - y), (1 - x) * y, x * y };

#ifdef REPROJECTION_SINGLE_COLOR_CHANNEL
        history_color = 0.0;
#else
        history_color = vec3(0.0);
#endif
#ifdef REPROJECTION_MOMENTS
        history_moments = vec2(0.0);
#endif

        for(int sample_idx = 0; sample_idx < 4; sample_idx++)
        {
            ivec2 loc = ivec2(history_coord_floor) + offset[sample_idx];
            if (v[sample_idx])
            {
#ifdef REPROJECTION_SINGLE_COLOR_CHANNEL
                history_color += w[sample_idx] * texelFetch(sampler_history_output, loc, 0).r;
#else
                history_color += w[sample_idx] * texelFetch(sampler_history_output, loc, 0).rgb;
#endif
#ifdef REPROJECTION_MOMENTS
                history_moments += w[sample_idx] * texelFetch(sampler_history_moments_length, loc, 0).rg;
#endif
                sum_w += w[sample_idx];
            }
        }

        valid = (sum_w >= 0.01);
#ifdef REPROJECTION_SINGLE_COLOR_CHANNEL
        history_color = valid ? history_color / sum_w : 0.0;
#else
        history_color = valid ? history_color / sum_w : vec3(0.0);
#endif
#ifdef REPROJECTION_MOMENTS
        history_moments = valid ? history_moments / sum_w : vec2(0.0);
#endif
    }

    if(!valid)
    {
        float cnt = 0.0;
        const int radius = 1;
        for (int yy = -radius; yy <= radius; yy++)
        {
            for (int xx = -radius; xx <= radius; xx++)
            {
                ivec2 p = ivec2(history_coord) + ivec2(xx, yy);

                vec4 prev_gbufferB_data = texelFetch(sampler_prev_gbufferB, p, g_buffer_mip);
                vec4 prev_gbufferC_data = texelFetch(sampler_prev_gbufferC, p, g_buffer_mip);
                float prev_depth = texelFetch(sampler_prev_depth, p, g_buffer_mip).r;

                vec3 prev_normal = octohedral_to_direction(gbufferB_data.xy);
                uint prev_instance_id = uint(gbufferC_data.z);
                vec3 prev_pos = world_position_from_depth(history_tex_coord, prev_depth, view_projection_inv);

                if (is_reprojection_valid(history_coord, current_pos, prev_pos, current_normal, prev_normal, current_instance_id, prev_instance_id, ivec2(image_size)))
                {
#ifdef REPROJECTION_SINGLE_COLOR_CHANNEL
                    history_color += texelFetch(sampler_history_output, p, 0).r;
#else
                    history_color += texelFetch(sampler_history_output, p, 0).rgb;
#endif
#ifdef REPROJECTION_MOMENTS
                    history_moments += texelFetch(sampler_history_moments_length, p, 0).rg;
#endif
                    cnt += 1.0;
                }
            }
        }
        if(cnt > 0)
        {
            valid = true;
            history_color /= cnt;
#ifdef REPROJECTION_MOMENTS
            history_moments /= cnt;
#endif
        }
    }

    if(valid)
    {
#ifdef REPROJECTION_MOMENTS
        history_length = texelFetch(sampler_history_moments_length, history_coord, 0).b;
#else
        history_length = texelFetch(sampler_history_length, history_coord, 0).r;
#endif
    }
    else
    {
#ifdef REPROJECTION_SINGLE_COLOR_CHANNEL
        history_color = 0.0;
#else        
        history_color = vec3(0.0);
#endif
#ifdef REPROJECTION_MOMENTS          
        history_moments = vec2(0.0);
#endif
        history_length  = 0.0;
    }

    return valid;
}

#endif