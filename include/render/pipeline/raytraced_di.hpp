#pragma once

#include "render/common.hpp"
#include "render/context.hpp"
#include "render/pipeline/gbuffer.hpp"
#include "render/scene.hpp"

struct RayTracedDI
{
  public:
	RayTracedDI(const Context &context, const Scene &scene, const GBufferPass &gbuffer_pass);

	~RayTracedDI();

	void init(VkCommandBuffer cmd_buffer);

	void update(const Scene &scene, const BlueNoise &blue_noise, const GBufferPass &gbuffer_pass);

	void draw(VkCommandBuffer cmd_buffer, const Scene &scene, const GBufferPass &gbuffer_pass);

	bool draw_ui();

  public:
	// Temporal reservoir buffer
	Buffer temporal_reservoir_buffer;

	// Passthrough reservoir buffer
	Buffer passthrough_reservoir_buffer;

	// Spatial reservoir buffer
	Buffer spatial_reservoir_buffer;

	// Output image
	Texture     output_image;
	VkImageView output_view = VK_NULL_HANDLE;

  private:
	const Context *m_context = nullptr;

	float m_normal_bias = 0.0001f;
	bool  m_spatial_reuse = false;
	bool  m_temporal_reuse = false;

	struct
	{
		struct
		{
			uint64_t temporal_reservoir_addr    = 0;
			uint64_t passthrough_reservoir_addr = 0;
			float    normal_bias                = 0.0001f;
			uint32_t temporal_reuse             = 0;
		} push_constants;

		VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
		VkPipeline       pipeline        = VK_NULL_HANDLE;
	} m_temporal_pass;

	struct
	{
		struct
		{
			uint64_t passthrough_reservoir_addr = 0;
			uint64_t spatial_reservoir_addr     = 0;
			uint32_t spatial_reuse             = 0;
		} push_constants;

		VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
		VkPipeline       pipeline        = VK_NULL_HANDLE;
	} m_spatial_pass;

	struct
	{
		struct
		{
			uint64_t passthrough_reservoir_addr = 0;
			uint64_t temporal_reservoir_addr    = 0;
			uint64_t spatial_reservoir_addr     = 0;
			float    normal_bias                = 0.0001f;
		} push_constants;

		VkPipelineLayout      pipeline_layout       = VK_NULL_HANDLE;
		VkPipeline            pipeline              = VK_NULL_HANDLE;
		VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
		VkDescriptorSet       descriptor_set        = VK_NULL_HANDLE;
	} m_composite_pass;
};