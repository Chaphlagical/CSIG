#pragma once

#include "context.hpp"

#include <functional>
#include <vector>

class UIPass
{
  public:
	UIPass(const Context &context);

	~UIPass();

	void render(CommandBufferRecorder &recorder, uint32_t frame_idx);

	void resize();

	void begin_frame();

	void end_frame();

	void set_style();

  private:
	void create_render_pass();

	void create_frame_buffer();

  private:
	const Context *m_context = nullptr;

	VkRenderPass m_render_pass = VK_NULL_HANDLE;

	std::array<VkFramebuffer, 3> m_frame_buffers = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
};