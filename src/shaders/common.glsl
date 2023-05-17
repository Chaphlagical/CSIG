#ifndef COMMON_GLSL
#define COMMON_GLSL

#include "common_data.hpp"

// void coordinate_system(vec3 N, out vec3 Nt, out vec3 Nb)
// {
// 	Nt = normalize(((abs(N.z) > 0.99999f) ? vec3(-N.x * N.y, 1.0f - N.y * N.y, -N.y * N.z) :
//                                           vec3(-N.x * N.z, -N.y * N.z, 1.0f - N.z * N.z)));
// 	Nb = normalize(cross(Nt, N));
// }

// vec2 direction_to_octohedral(vec3 normal)
// {
//     vec2 p = normal.xy * (1.0 / dot(abs(normal), vec3(1.0)));
//     return normal.z > 0.0 ? p : (1.0 - abs(p.yx)) * (step(0.0, p) * 2.0 - vec2(1.0));
// }

// vec3 octohedral_to_direction(vec2 e)
// {
//     vec3 v = vec3(e, 1.0 - abs(e.x) - abs(e.y));

//     if (v.z < 0.0)
// 	{
//         v.xy = (1.0 - abs(v.yx)) * (step(0.0, v.xy) * 2.0 - vec2(1.0));
// 	}

//     return normalize(v);
// }

// vec3 world_position_from_depth(vec2 uv, float ndc_depth, mat4 view_proj_inv)
// {
//     vec2 screen_pos = uv * 2.0 - 1.0;
//     vec4 ndc_pos = vec4(screen_pos, ndc_depth, 1.0);
// 	vec4 world_pos  = view_proj_inv * ndc_pos;
//     world_pos = world_pos / world_pos.w;
//     return world_pos.xyz;
// }

vec3 local_to_world(vec3 n, vec3 v)
{
	const vec3 ref = abs(dot(n, vec3(0, 1, 0))) > 0.99 ? vec3(0, 0, 1) : vec3(0, 1, 0);
    const vec3 x = normalize(cross(ref, n));
    const vec3 y = cross(n, x);
	return normalize(mat3(x, y, n) * v);
}

// vec3 offset_ray(vec3 p, vec3 n)
// {
//   const float intScale   = 256.0f;
//   const float floatScale = 1.0f / 65536.0f;
//   const float origin     = 1.0f / 32.0f;

//   ivec3 of_i = ivec3(intScale * n.x, intScale * n.y, intScale * n.z);

//   vec3 p_i = vec3(intBitsToFloat(floatBitsToInt(p.x) + ((p.x < 0) ? -of_i.x : of_i.x)),
//                   intBitsToFloat(floatBitsToInt(p.y) + ((p.y < 0) ? -of_i.y : of_i.y)),
//                   intBitsToFloat(floatBitsToInt(p.z) + ((p.z < 0) ? -of_i.z : of_i.z)));

//   return vec3(abs(p.x) < origin ? p.x + floatScale * n.x : p_i.x,  //
//               abs(p.y) < origin ? p.y + floatScale * n.y : p_i.y,  //
//               abs(p.z) < origin ? p.z + floatScale * n.z : p_i.z);
// }

float gaussian_weight(float offset, float deviation)
{
    float weight = 1.0 / sqrt(2.0 * PI * deviation * deviation);
    weight *= exp(-(offset * offset) / (2.0 * deviation * deviation));
    return weight;
}

float area_integration(float x, float y)
{
    return atan(sqrt(x * x + y * y + 1), x * y);
}

float calculate_solid_angle(uint x, uint y, uint width, uint height)
{
    float u = 2.0 * (float(x) + 0.5) / float(width) - 1.0;
    float v = 2.0 * (float(y) + 0.5) / float(height) - 1.0;

    float x0 = u - 1.0 / float(width);
    float x1 = u + 1.0 / float(width);
    float y0 = v - 1.0 / float(height);
    float y1 = v + 1.0 / float(height);

    return area_integration(x0, y0) - area_integration(x0, y1) - area_integration(x1, y0) + area_integration(x1, y1);
}

vec3 calculate_cubemap_direction(uint face_idx, uint face_x, uint face_y, uint width, uint height)
{
    float u = 2.0 * (float(face_x) + 0.5) / float(width) - 1.0;
    float v = 2.0 * (float(face_y) + 0.5) / float(height) - 1.0;
    float x, y, z;

    // POSITIVE_X 0
    // NEGATIVE_X 1
    // POSITIVE_Y 2
    // NEGATIVE_Y 3
    // POSITIVE_Z 4
    // NEGATIVE_Z 5
    
    switch (face_idx)
    {
        case 0:
            x = 1;
            y = -v;
            z = -u;
            break;
        case 1:
            x = -1;
            y = -v;
            z = u;
            break;
        case 2:
            x = u;
            y = 1;
            z = v;
            break;
        case 3:
            x = u;
            y = -1;
            z = -v;
            break;
        case 4:
            x = u;
            y = -v;
            z = 1;
            break;
        case 5:
            x = -u;
            y = -v;
            z = -1;
            break;
    }

    return normalize(vec3(x, y, z));
}

#endif        // !COMMON_GLSL
