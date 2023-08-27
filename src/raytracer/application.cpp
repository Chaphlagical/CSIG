#include "application.hpp"

#include <GLFW/glfw3.h>

#include <imgui.h>
#include <spdlog/fmt/fmt.h>

Application::Application() :
    m_renderer{
        .ui_pass{m_context},
    },
    m_scene{m_context}
{
	for (uint32_t i = 0; i < 3; i++)
	{
		m_recorders.push_back(m_context.record_command());
	}

	m_render_complete  = m_context.create_semaphore("Render Complete Semaphore");
	m_present_complete = m_context.create_semaphore("Present Complete Semaphore");

	for (uint32_t i = 0; i < 3; i++)
	{
		m_fences.push_back(m_context.create_fence(fmt::format("Fence #{}", i)));
	}

	m_scene.load_scene(R"(D:\Workspace\CSIG\assets\scenes\default.glb)");
	// m_scene.load_scene(R"(D:\Workspace\CSIG\assets\scenes\Deferred\Deferred.gltf)");
}

Application::~Application()
{
	m_context.wait();
	m_context
	    .destroy(m_render_complete)
	    .destroy(m_present_complete)
	    .destroy(m_fences);
}

void Application::run()
{
	while (!glfwWindowShouldClose(m_context.window))
	{
		glfwPollEvents();

		int32_t width, height;
		glfwGetWindowSize(m_context.window, &width, &height);
		if (width == 0 || height == 0)
		{
			continue;
		}

		auto &recorder = m_recorders[m_current_frame];

		update_ui();

		begin_render();
		update(recorder);
		render(recorder);
		end_render();

		m_current_frame     = (m_current_frame + 1) % 3;
		m_context.ping_pong = !m_context.ping_pong;
		m_num_frames++;
	}
}

void Application::begin_render()
{
	m_context.acquire_next_image(m_present_complete);
	m_context.wait(m_fences[m_current_frame]);
	m_recorders[m_current_frame].begin();
}

void Application::end_render()
{
	m_recorders[m_current_frame]
	    .end()
	    .submit({m_render_complete}, {m_present_complete}, {VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT}, m_fences[m_current_frame])
	    .present({m_render_complete});
}

void Application::update(CommandBufferRecorder &recorder)
{
}

void Application::render(CommandBufferRecorder &recorder)
{
	m_renderer.ui_pass.render(recorder, m_current_frame);
}

void Application::update_ui()
{
	m_renderer.ui_pass.begin_frame();
	ImGui::ShowDemoWindow();
	m_renderer.ui_pass.end_frame();
}
