#ifndef RAYTRACE_GLSL
#define RAYTRACE_GLSL

#include "common.glsl"

float query_visibility(accelerationStructureEXT tlas, vec3 world_pos, vec3 dir, float t_max, uint flags)
{
    float t_min = ShadowEpsilon;

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

float query_visibility(accelerationStructureEXT tlas, vec3 world_pos, vec3 dir, float t_max, uint flags, out uint instance_id)
{
	float t_min = ShadowEpsilon;

	rayQueryEXT ray_query;
	rayQueryInitializeEXT(ray_query,
	                      tlas,
	                      flags,
	                      0xFF,
	                      world_pos,
	                      t_min,
	                      dir,
	                      t_max);
	while (rayQueryProceedEXT(ray_query)){}
	if (rayQueryGetIntersectionTypeEXT(ray_query, true) != gl_RayQueryCommittedIntersectionNoneEXT)
	{
		instance_id = rayQueryGetIntersectionInstanceIdEXT(ray_query, true);
		return 0.0;
	}
	instance_id = ~0u;
	return 1.0;
}

bool scene_intersect(accelerationStructureEXT tlas, vec3 org, vec3 dir, uint flags, out uint instance_id, out uint primitive_id, out vec2 barycentric, out float t)
{
	rayQueryEXT ray_query;
	rayQueryInitializeEXT(ray_query,
	                      tlas,
	                      flags,
	                      0xFF,
	                      org,
	                      Epsilon,
	                      dir,
	                      Infinity);
	while (rayQueryProceedEXT(ray_query))
	{}
	if (rayQueryGetIntersectionTypeEXT(ray_query, true) != gl_RayQueryCommittedIntersectionNoneEXT)
	{
		instance_id = rayQueryGetIntersectionInstanceIdEXT(ray_query, true);
		primitive_id = rayQueryGetIntersectionPrimitiveIndexEXT(ray_query, true);
		barycentric = rayQueryGetIntersectionBarycentricsEXT(ray_query, true);
		t            = rayQueryGetIntersectionTEXT(ray_query, true); 
		return true;
	}
	instance_id = ~0;
	primitive_id = ~0;
	barycentric  = vec2(0.0);
	t            = 0; 
	return false;

}

#endif