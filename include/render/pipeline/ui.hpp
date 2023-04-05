#pragma once

#include "render/context.hpp"

#include <functional>
#include <vector>

class UI
{
  public:
	UI(const Context &context);

	~UI();

	void render(VkCommandBuffer cmd_buffer, uint32_t frame_idx);
	void begin_frame();
	void end_frame();

  private:
	void create_render_pass();
	void create_frame_buffer();

  private:
	const Context *m_context = nullptr;

	VkRenderPass m_render_pass = VK_NULL_HANDLE;

	std::array<VkFramebuffer, 3> m_frame_buffers = {VK_NULL_HANDLE};
};