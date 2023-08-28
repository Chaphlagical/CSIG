#pragma once

#include "context.hpp"
#include "pipeline/gbuffer.hpp"
#include "pipeline/raytrace_ao.hpp"
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
	void update_view();
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

	std::vector<glm::vec2> m_jitter_samples;

	glm::vec2 m_current_jitter = glm::vec2(0.f);
	glm::vec2 m_prev_jitter    = glm::vec2(0.f);

	bool m_update = true;
	bool m_enable_ui = true;

	struct
	{
		glm::vec3 position = glm::vec3(0.f);
		float     yaw      = 0.f;
		float     pitch    = 0.f;
		float     sensity  = 0.1f;
		float     speed    = 5.f;
		glm::vec3 velocity = glm::vec3(0.f);

		glm::mat4 view          = glm::mat4(1.f);
		glm::mat4 proj          = glm::mat4(1.f);
		glm::mat4 view_proj     = glm::mat4(1.f);
		glm::mat4 view_proj_inv = glm::mat4(1.f);

		glm::vec3 prev_position      = glm::vec3(0.f);
		glm::mat4 prev_view          = glm::mat4(1.f);
		glm::mat4 prev_proj          = glm::mat4(1.f);
		glm::mat4 prev_view_proj     = glm::mat4(1.f);
		glm::mat4 prev_view_proj_inv = glm::mat4(1.f);
	} m_camera;

	struct
	{
		UIPass      ui;
		GBufferPass gbuffer;
		RayTracedAO ao;
	} m_renderer;
};