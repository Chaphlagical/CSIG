#pragma once

#include "render/context.hpp"
#include "render/pipeline/ui.hpp"
#include "render/scene.hpp"

#include <memory>

struct ApplicationConfig
{
	ContextConfig context_config;
	std::string   scene_file = "scene/PBR/PBR.gltf";
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
	void update();

  private:
	Context m_context;

	std::array<VkCommandBuffer, 3> m_cmd_buffers = {VK_NULL_HANDLE};

	struct
	{
		std::unique_ptr<UI> ui = nullptr;
	} m_renderer;
	Scene m_scene;

	uint32_t m_current_frame = 0;
	uint32_t m_image_index   = 0;

	bool m_enable_ui = true;
};