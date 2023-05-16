#pragma once

#include "render/common.hpp"
#include "render/context.hpp"
#include "render/pipeline/gbuffer.hpp"
#include "render/pipeline/pathtracing.hpp"
// #include "render/pipeline/raytraced_ao.hpp"
#include "render/pipeline/raytraced_di.hpp"
#include "render/pipeline/tonemap.hpp"
#include "render/pipeline/ui/ui.hpp"
#include "render/scene.hpp"

#include <memory>

struct ApplicationConfig
{
	ContextConfig context_config;

	//std::string scene_file = "assets/scenes/Deferred/Deferred.gltf";
	// std::string scene_file = "assets/scenes/test.glb";
	 std::string scene_file = "assets/scenes/conell_box.glb";
	std::string hdr_file = "assets/textures/hdr/BasketballCourt_3k.hdr";
};

class Application
{
  public:
	Application(const ApplicationConfig &config);

	~Application();

	void run();

  private:
	void begin_render();
	void end_render();
	void update_ui();
	void update(VkCommandBuffer cmd_buffer);
	void render(VkCommandBuffer cmd_buffer);
	void present(VkCommandBuffer cmd_buffer, VkImage image);

  private:
	Context m_context;

	Scene m_scene;

	std::array<VkCommandBuffer, 3> m_cmd_buffers = {VK_NULL_HANDLE};

	struct
	{
		UI          ui;
		GBufferPass gbuffer_pass;
		PathTracing path_tracing;
		RayTracedDI raytraced_di;
		// RayTracedAO raytraced_ao;
		//  RayTracedGI raytraced_gi;
		Tonemap tonemap;
	} m_renderer;

	struct
	{
		glm::vec3 position = glm::vec3(0.f);
		float     yaw      = 0.f;
		float     pitch    = 0.f;
		float     sensity  = 0.1f;
		float     speed    = 1.f;
		glm::vec3 velocity = glm::vec3(0.f);

		glm::mat4 view          = glm::mat4(1.f);
		glm::mat4 proj          = glm::mat4(1.f);
		glm::mat4 view_proj     = glm::mat4(1.f);
		glm::mat4 view_proj_inv = glm::mat4(1.f);

		glm::mat4 prev_view          = glm::mat4(1.f);
		glm::mat4 prev_proj          = glm::mat4(1.f);
		glm::mat4 prev_view_proj     = glm::mat4(1.f);
		glm::mat4 prev_view_proj_inv = glm::mat4(1.f);
	} m_camera;

	BlueNoise m_blue_noise;

	uint32_t m_current_frame = 0;
	uint32_t m_num_frames    = 0;

	bool m_update    = true;
	bool m_enable_ui = true;

	std::vector<glm::vec2> m_jitter_samples;

	glm::vec2 m_current_jitter = glm::vec2(0.f);
	glm::vec2 m_prev_jitter    = glm::vec2(0.f);

	enum class RenderMode
	{
		Hybrid,
		PathTracing,
	} m_render_mode = RenderMode::Hybrid;
};