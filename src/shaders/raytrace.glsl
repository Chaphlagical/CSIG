#ifndef RAYTRACE_GLSL
#define RAYTRACE_GLSL

float query_visibility(accelerationStructureEXT tlas, vec3 world_pos, vec3 dir, float t_max, uint flags)
{
    float t_min = 0.01;
    rayQueryEXT ray_query;
    rayQueryInitializeEXT(ray_query,
                          tlas,
                          flags,
                          0xFF,
                          world_pos,
                          t_min,
                          dir,
                          t_max);
    while (rayQueryProceedEXT(ray_query)) {}
    if (rayQueryGetIntersectionTypeEXT(ray_query, true) != gl_RayQueryCommittedIntersectionNoneEXT)
    {
        return 0.0;
    }

    return 1.0;
}

#endif