#pragma once

#include "context.hpp"

#define CAMERA_NEAR_PLANE 0.01f
#define CAMERA_FAR_PLANE 1000.f

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

	VkImageView scrambling_ranking_image_views[9];
	VkImageView sobol_image_view = VK_NULL_HANDLE;

	BlueNoise(const Context &context);

	~BlueNoise();

  public:
	struct
	{
		VkDescriptorSetLayout layout = VK_NULL_HANDLE;
		VkDescriptorSet       set    = VK_NULL_HANDLE;
	}descriptor;

  private:
	const Context *m_context = nullptr;
};