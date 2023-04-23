#pragma once

#include "render/common.hpp"
#include "render/context.hpp"
#include "render/pipeline/gbuffer.hpp"
#include "render/pipeline/pathtracing.hpp"
#include "render/pipeline/raytraced_ao.hpp"
#include "render/pipeline/ui/ui.hpp"
#include "render/scene.hpp"

#include <memory>

struct ApplicationConfig
{
	ContextConfig context_config;
	SceneConfig   scene_config = {.light_config = SceneConfig::LightLoadingConfig::AsPointLight};
	std::string   scene_file   = "assets/scenes/GI/GI.gltf";
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
	void update();
	void render(VkCommandBuffer cmd_buffer);
	void present(VkCommandBuffer cmd_buffer, VkImage image);

  private:
	Context m_context;

	std::array<VkCommandBuffer, 3> m_cmd_buffers = {VK_NULL_HANDLE};

	struct
	{
		UI          ui;
		PathTracing path_tracing;
		GBufferPass gbuffer_pass;
		RayTracedAO raytraced_ao;
	} m_renderer;

	struct
	{
		glm::vec3 position = glm::vec3(0.f);
		float     yaw      = 0.f;
		float     pitch    = 0.f;
		float     sensity  = 0.1f;
		float     speed    = 1.f;
		glm::vec3 velocity = glm::vec3(0.f);

		glm::mat4 view = glm::mat4(1.f);
		glm::mat4 proj = glm::mat4(1.f);
	} m_camera;

	Scene     m_scene;
	BlueNoise m_blue_noise;

	uint32_t m_current_frame = 0;
	uint32_t m_num_frames    = 0;

	bool m_update      = true;
	bool m_enable_ui   = true;
	bool m_pathtracing = true;
};