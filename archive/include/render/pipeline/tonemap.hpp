#pragma once

#include "render/common.hpp"
#include "render/context.hpp"
#include "render/scene.hpp"

#include <glm/glm.hpp>

struct Tonemap
{
  public:
	Tonemap(const Context &context);

	~Tonemap();

	void init(VkCommandBuffer cmd_buffer);

	void update(const Scene &scene, VkImageView pt_result[2], VkImageView hybrid_result[2]);

	void draw(VkCommandBuffer cmd_buffer);

	bool draw_ui();

	void set_pathtracing(bool enable);

  public:
	Texture     tonemapped_image;
	VkImageView tonemapped_image_view = VK_NULL_HANDLE;

  private:
	const Context *m_context = nullptr;

	struct
	{
		float     brightness      = 1.f;
		float     contrast        = 1.f;
		float     saturation      = 1.f;
		float     vignette        = 0.f;
		float     avg_lum         = 1.f;
		float     y_white         = 0.5f;
		float     key             = 0.5f;
	} m_push_constants;

	bool m_is_pathtracing = false;

	VkPipelineLayout      m_pipeline_layout       = VK_NULL_HANDLE;
	VkPipeline            m_pipeline              = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_descriptor_set_layout = VK_NULL_HANDLE;
	VkDescriptorSet       m_pt_descriptor_sets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	VkDescriptorSet       m_hybrid_descriptor_sets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
};