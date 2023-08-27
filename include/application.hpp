#pragma once

#include "context.hpp"
#include "pipeline/ui.hpp"
#include "scene.hpp"

class Application
{
  public:
	Application();

	~Application();

	void run();

  private:
	void begin_render();
	void end_render();
	void update(CommandBufferRecorder &recorder);
	void render(CommandBufferRecorder &recorder);
	void update_ui();

  private:
	Context m_context;
	Scene   m_scene;

	std::vector<CommandBufferRecorder> m_recorders;

	uint32_t m_current_frame = 0;
	uint32_t m_num_frames    = 0;

	VkSemaphore m_render_complete  = VK_NULL_HANDLE;
	VkSemaphore m_present_complete = VK_NULL_HANDLE;

	std::vector<VkFence> m_fences;

	struct
	{
		UIPass ui_pass;
	} m_renderer;
};