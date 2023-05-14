#ifndef SHADE_STATE_GLSL
#define SHADE_STATE_GLSL

#include "common_data.hpp"

struct ShadeState
{
	vec3 normal;
	vec3 geom_normal;
	vec3 ffnormal;
	vec3 position;
	vec3 tangent;
	vec3 bitangent;
	float eta;
	bool primary;
	float depth;
	vec2 motion_vector;	// only for primary bounce
	Material mat;
};

#endif