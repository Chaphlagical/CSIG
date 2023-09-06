#include "pipeline/raytrace_gi.hpp"

RayTracedGI::RayTracedGI(const Context &context, const Scene &scene, const GBufferPass &gbuffer_pass, RayTracedScale scale)
{
}

RayTracedGI::~RayTracedGI()
{
}

void RayTracedGI::init()
{
}

void RayTracedGI::draw(CommandBufferRecorder &recorder, const Scene &scene, const GBufferPass &gbuffer_pass)
{
}

bool RayTracedGI::draw_ui()
{
	return false;
}
