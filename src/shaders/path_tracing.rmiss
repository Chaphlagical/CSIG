#version 460
#extension GL_KHR_vulkan_glsl : enable
#extension GL_EXT_ray_tracing : enable

layout(location = 0) rayPayloadInEXT vec3 hitValue;

void main()
{
    hitValue = vec3(0.0, 0.0, 0.2);
}