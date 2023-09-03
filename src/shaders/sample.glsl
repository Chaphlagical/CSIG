#ifndef SAMPLE_GLSL
#define SAMPLE_GLSL

#include "common.glsl"
#include "common_data.hpp"

vec2 uniform_sample_disk(vec2 u)
{
    float r = sqrt(u.x);
    float theta = 2* PI * u.y;
    return r * vec2(cos(theta), sin(theta));
}

vec2 sample_concentric_disk(vec2 u)
{
    vec2 offset = 2.0 * u - 1.0;
    if(offset.x == 0 && offset.y == 0)
    {
        return vec2(0.0);
    }
    float theta, r;
    if (abs(offset.x) > abs(offset.y))
    {
        r = offset.x;
        theta = PIOver4 * (offset.y / offset.x);
    }
    else
    {
        r = offset.y;
        theta = PIOver2 - PIOver4 * (offset.x / offset.y);
    }

    return r * vec2(cos(theta), sin(theta));
}

vec3 uniform_sample_hemisphere(vec2 u)
{
    float z = u.x;
    float r = sqrt(max(0.0, 1.0 - z * z));
    float phi = 2 * PI * u.y;
    return vec3(r * cos(phi), r * sin(phi), z);
}

vec3 uniform_sample_sphere(vec2 u)
{
    float z = 1.0 - 2.0 * u.x;
    float r = sqrt(max(0.0, 1.0 - z * z));
    float phi = 2 * PI * u.y;
    return vec3(r * cos(phi), r * sin(phi), z);
}

vec3 sample_cosine_hemisphere(vec2 u)
{
    vec2 d = sample_concentric_disk(u);
    float z = sqrt(max(0, 1 - d.x * d.x - d.y * d.y));
    return vec3(d.x, d.y, z);
}

#endif