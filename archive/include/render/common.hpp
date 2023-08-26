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
	Texture     scrambling_ranking_images[9];
	VkImageView scrambling_ranking_image_views[9];

	Texture     sobol_image;
	VkImageView sobol_image_view = VK_NULL_HANDLE;

	BlueNoise(const Context &context);

	~BlueNoise();

  public:
	struct
	{
		VkDescriptorSetLayout layout = VK_NULL_HANDLE;
		VkDescriptorSet       set    = VK_NULL_HANDLE;
	} descriptor;

  private:
	const Context *m_context = nullptr;
};

struct LUT
{
	Texture     ggx_image;
	VkImageView ggx_view = VK_NULL_HANDLE;

	LUT(const Context &context);

	~LUT();

  public:
	struct
	{
		VkDescriptorSetLayout layout = VK_NULL_HANDLE;
		VkDescriptorSet       set    = VK_NULL_HANDLE;
	} descriptor;

  private:
	const Context *m_context = nullptr;
};

// create all kinds of buffer
Buffer create_vulkan_buffer(const Context &context, VkBufferUsageFlags usage, void *data, size_t size);

// copy to all kinds of vulkan buffer
void copy_to_vulkan_buffer(const Context& context, Buffer &target_buffer, void *data, size_t size);