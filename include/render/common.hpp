#pragma once

#include "context.hpp"

enum class RayTracedScale
{
	Full_Res,
	Half_Res,
	Quarter_Res,
};

enum BlueNoiseSpp
{
	BLUE_NOISE_1SPP,
	BLUE_NOISE_2SPP,
	BLUE_NOISE_4SPP,
	BLUE_NOISE_8SPP,
	BLUE_NOISE_16SPP,
	BLUE_NOISE_32SPP,
	BLUE_NOISE_64SPP,
	BLUE_NOISE_128SPP,
	BLUE_NOISE_256SPP,
};

struct BlueNoise
{
	Texture scrambling_ranking_images[9];
	Texture sobol_image;

	VkImageView scrambling_ranking_image_views[9] = {VK_NULL_HANDLE};
	VkImageView sobol_image_view                  = VK_NULL_HANDLE;

	BlueNoise(const Context &context);

	~BlueNoise();

  private:
	const Context *m_context = nullptr;
};