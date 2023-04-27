#pragma once

#include "render/common.hpp"
#include "render/context.hpp"
#include "render/pipeline/gbuffer.hpp"
#include "render/scene.hpp"

struct RayTracedReflection
{
  public:
	RayTracedReflection(const Context &context, RayTracedScale scale = RayTracedScale::Half_Res);

	~RayTracedReflection();

	void init(VkCommandBuffer cmd_buffer);

	void update(const Scene &scene, const BlueNoise &blue_noise, const GBufferPass &gbuffer_pass);

	void draw(VkCommandBuffer cmd_buffer);

	bool draw_ui();

  private:

};