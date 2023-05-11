#include "render/pipeline/raytraced_di.hpp"

RayTracedDI::RayTracedDI(const Context &context)
{
}

RayTracedDI::~RayTracedDI()
{
}

void RayTracedDI::init(VkCommandBuffer cmd_buffer)
{
}

void RayTracedDI::update(const Scene &scene, const BlueNoise &blue_noise, const GBufferPass &gbuffer_pass)
{
}

void RayTracedDI::draw(VkCommandBuffer cmd_buffer)
{
}

bool RayTracedDI::draw_ui()
{
	return false;
}
