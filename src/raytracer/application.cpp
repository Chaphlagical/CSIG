#include "application.hpp"

#include <GLFW/glfw3.h>

#include <glm/gtc/matrix_transform.hpp>

#include <imgui.h>

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <nfd.h>

#include <filesystem>

#define HALTON_SAMPLES 16
#define CAMERA_NEAR_PLANE 0.01f
#define CAMERA_FAR_PLANE 1000.f

inline bool is_key_pressed(GLFWwindow *window, uint32_t keycode)
{
	auto state = glfwGetKey(window, keycode);
	return state == GLFW_PRESS || state == GLFW_REPEAT;
}

inline float halton_sequence(int32_t base, int32_t index)
{
	float result = 0;
	float f      = 1;
	while (index > 0)
	{
		f /= base;
		result += f * (index % base);
		index = (int32_t) floor((float) index / (float) base);
	}

	return result;
}

inline glm::vec3 smooth_step(const glm::vec3 &v1, const glm::vec3 &v2, float t)
{
	t = glm::clamp(t, 0.f, 1.f);
	t = t * t * (3.f - 2.f * t);

	glm::vec3 v = glm::mix(v1, v2, t);

	return v;
}

Application::Application() :
    m_context{1920, 1080},
    m_scene{m_context},
    m_renderer{
        .ui{m_context},
        .gbuffer{m_context, m_scene},
        .path_tracing{m_context, m_scene, m_renderer.gbuffer},
        .ao{m_context, m_scene, m_renderer.gbuffer},
        .di{m_context, m_scene, m_renderer.gbuffer},
        .reflection{m_context, m_scene, m_renderer.gbuffer},
        .tonemap{m_context},
        .composite{m_context, m_scene, m_renderer.gbuffer, m_renderer.ao, m_renderer.di, m_renderer.reflection},
    }
{
	glfwSetWindowUserPointer(m_context.window, this);
	glfwSetScrollCallback(m_context.window, [](GLFWwindow *window, double xoffset, double yoffset) {
		Application *app = (Application *) glfwGetWindowUserPointer(window);
		app->m_camera.speed += static_cast<float>(yoffset) * 0.3f;
	});

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

	for (int32_t i = 1; i <= HALTON_SAMPLES; i++)
	{
		m_jitter_samples.push_back(glm::vec2((2.f * halton_sequence(2, i) - 1.f), (2.f * halton_sequence(3, i) - 1.f)));
	}

	// m_scene.load_scene(R"(D:\Workspace\CSIG\assets\scenes\default.glb)");
	m_scene.load_scene(R"(D:\Workspace\CSIG\assets\scenes\Deferred\Deferred.gltf)");
	m_scene.load_envmap(R"(D:\Workspace\CSIG\assets\textures\hdr\default.hdr)");
	m_scene.update();

	m_context.wait();
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
		recorder.begin_marker("Tick");
		update(recorder);
		render(recorder);
		recorder.end_marker();
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

void Application::update_view()
{
	static bool hide_cursor = false;
	if (ImGui::IsMouseDown(ImGuiMouseButton_Right) || m_num_frames == 0)
	{
		static double cursor_xpos, cursor_ypos;
		if (!hide_cursor)
		{
			hide_cursor = true;
			glfwGetCursorPos(m_context.window, &cursor_xpos, &cursor_ypos);
		}
		ImGui::SetMouseCursor(ImGuiMouseCursor_None);

		double current_xpos, current_ypos;
		glfwGetCursorPos(m_context.window, &current_xpos, &current_ypos);
		glm::vec2 delta_pos = {
		    static_cast<float>(current_xpos - cursor_xpos),
		    static_cast<float>(current_ypos - cursor_ypos),
		};
		glfwSetCursorPos(m_context.window, cursor_xpos, cursor_ypos);

		m_camera.yaw += delta_pos.x * m_camera.sensity;
		m_camera.pitch -= delta_pos.y * m_camera.sensity;
		m_camera.pitch = glm::clamp(m_camera.pitch, -88.f, 88.f);

		glm::vec3 front = glm::vec3(1.0f);

		front.x = cos(glm::radians(m_camera.pitch)) * cos(glm::radians(m_camera.yaw));
		front.y = sin(glm::radians(m_camera.pitch));
		front.z = cos(glm::radians(m_camera.pitch)) * sin(glm::radians(m_camera.yaw));
		front   = glm::normalize(front);

		glm::vec3 right = glm::normalize(glm::cross(front, glm::vec3(0.0f, 1.0f, 0.0f)));
		glm::vec3 up    = glm::normalize(glm::cross(right, front));

		glm::vec3 direction = glm::vec3(0.f);
		if (is_key_pressed(m_context.window, GLFW_KEY_W))
		{
			direction += front;
		}
		if (is_key_pressed(m_context.window, GLFW_KEY_S))
		{
			direction -= front;
		}
		if (is_key_pressed(m_context.window, GLFW_KEY_A))
		{
			direction -= right;
		}
		if (is_key_pressed(m_context.window, GLFW_KEY_D))
		{
			direction += right;
		}
		if (is_key_pressed(m_context.window, GLFW_KEY_Q))
		{
			direction += up;
		}
		if (is_key_pressed(m_context.window, GLFW_KEY_E))
		{
			direction -= up;
		}

		m_camera.speed += 0.1f * ImGui::GetIO().MouseWheel;
		m_camera.velocity = smooth_step(m_camera.velocity, direction * m_camera.speed, 0.6f);
		m_camera.position += ImGui::GetIO().DeltaTime * m_camera.velocity;

		m_camera.view = glm::lookAt(m_camera.position, m_camera.position + front, up);
		// Reversed Z
		m_camera.proj =
		    glm::mat4(1, 0, 0, 0,
		              0, 1, 0, 0,
		              0, 0, -1, 0,
		              0, 0, 1, 1) *
		    glm::perspective(glm::radians(60.f), static_cast<float>(m_context.render_extent.width) / static_cast<float>(m_context.render_extent.height), CAMERA_NEAR_PLANE, CAMERA_FAR_PLANE);
		m_renderer.path_tracing.reset_frames();
	}
	else
	{
		m_camera.velocity = glm::vec3(0.f);
		m_prev_jitter     = m_current_jitter;
		glm::vec2 halton  = m_jitter_samples[m_num_frames % m_jitter_samples.size()];
		m_current_jitter  = glm::vec2(halton.x / float(m_context.render_extent.width), halton.y / float(m_context.render_extent.height));

		hide_cursor = false;
	}

	{
		glm::mat4 jitter_proj  = glm::translate(glm::mat4(1.0f), glm::vec3(m_current_jitter, 0.0f)) * m_camera.proj;
		m_camera.view_proj     = jitter_proj * m_camera.view;
		m_camera.view_proj_inv = glm::inverse(m_camera.view_proj);

		m_scene.view_info = {
		    .view_inv                 = glm::inverse(m_camera.view),
		    .projection_inv           = glm::inverse(jitter_proj),
		    .view_projection_inv      = m_camera.view_proj_inv,
		    .view_projection          = m_camera.view_proj,
		    .prev_view                = m_camera.prev_view,
		    .prev_projection          = m_camera.prev_proj,
		    .prev_view_projection     = m_camera.prev_view_proj,
		    .prev_view_projection_inv = m_camera.prev_view_proj_inv,
		    .cam_pos                  = glm::vec4(m_camera.position, static_cast<float>(m_num_frames)),
		    .prev_cam_pos             = glm::vec4(m_camera.prev_position, 0.f),
		    .jitter                   = glm::vec4(m_current_jitter, m_prev_jitter),
		};

		m_camera.prev_view_proj     = m_camera.view_proj;
		m_camera.prev_view_proj_inv = m_camera.view_proj_inv;
		m_camera.prev_view          = m_camera.view;
		m_camera.prev_proj          = m_camera.proj;
	}
}

void Application::update(CommandBufferRecorder &recorder)
{
	if (m_update)
	{
		m_context.wait();
	}

	update_view();
	m_scene.update_view(recorder);
}

void Application::render(CommandBufferRecorder &recorder)
{
	m_renderer.gbuffer.draw(recorder, m_scene);

	switch (m_render_mode)
	{
		case RenderMode::Normal:
			m_renderer.composite.draw(recorder, m_scene, m_renderer.gbuffer, CompositePass::GBufferOption::Normal);
			break;
		case RenderMode::Albedo:
			m_renderer.composite.draw(recorder, m_scene, m_renderer.gbuffer, CompositePass::GBufferOption::Albedo);
			break;
		case RenderMode::Roughness:
			m_renderer.composite.draw(recorder, m_scene, m_renderer.gbuffer, CompositePass::GBufferOption::Roughness);
			break;
		case RenderMode::Metallic:
			m_renderer.composite.draw(recorder, m_scene, m_renderer.gbuffer, CompositePass::GBufferOption::Metallic);
			break;
		case RenderMode::Position:
			m_renderer.composite.draw(recorder, m_scene, m_renderer.gbuffer, CompositePass::GBufferOption::Position);
			break;
		default:
			break;
	}

	if (m_render_mode == RenderMode::PathTracing)
	{
		m_renderer.path_tracing.draw(recorder, m_scene, m_renderer.gbuffer);
		m_renderer.tonemap.draw(recorder, m_renderer.path_tracing);
		m_renderer.composite.draw(recorder, m_scene, m_renderer.tonemap);
	}
	else
	{
		m_renderer.ao.draw(recorder, m_scene, m_renderer.gbuffer);
		m_renderer.di.draw(recorder, m_scene, m_renderer.gbuffer);
		m_renderer.reflection.draw(recorder, m_scene, m_renderer.gbuffer);
		if (m_render_mode == RenderMode::AO)
		{
			m_renderer.composite.draw(recorder, m_scene, m_renderer.ao);
		}
		else if (m_render_mode == RenderMode::Reflection)
		{
			m_renderer.composite.draw(recorder, m_scene, m_renderer.reflection);
		}
		else if (m_render_mode == RenderMode::DI)
		{
			m_renderer.composite.draw(recorder, m_scene, m_renderer.di);
		}
	}
	m_renderer.ui.render(recorder, m_current_frame);
}

void Application::update_ui()
{
	m_renderer.ui.begin_frame();

	if (ImGui::IsKeyPressed(ImGuiKey_G, false))
	{
		m_enable_ui = !m_enable_ui;
	}

	if (m_enable_ui)
	{
		ImGui::Begin("UI", &m_enable_ui);
		ImGui::Text("CSIG 2023 RayTracer");
		ImGui::Text("FPS: %.f", ImGui::GetIO().Framerate);
		ImGui::Text("Frames: %.d", m_num_frames);

		if (ImGui::Button("Open Scene"))
		{
			char *path = nullptr;
			if (NFD_OpenDialog("gltf,glb", PROJECT_DIR, &path) == NFD_OKAY)
			{
				m_scene.load_scene(path);
				m_scene.update();
				m_update = true;
			}
		}

		ImGui::SameLine();

		if (ImGui::Button("Open HDRI"))
		{
			char *path = nullptr;
			if (NFD_OpenDialog("hdr", PROJECT_DIR, &path) == NFD_OKAY)
			{
				m_scene.load_envmap(path);
				m_scene.update();
				m_update = true;
			}
		}
		const char *const render_modes[] = {"Path Tracing", "Hybrid", "Normal", "Albedo", "Roughness", "Metallic", "Position", "AO", "Reflection", "DI", "GI"};
		if (ImGui::Combo("Render Mode", reinterpret_cast<int *>(&m_render_mode), render_modes, 11))
		{
			m_context.ping_pong = false;
			m_context.wait();
			m_renderer.gbuffer.init();
			m_renderer.path_tracing.init();
			m_renderer.ao.init();
			m_renderer.reflection.init();
		}

		bool update = false;
		if (m_render_mode == RenderMode::PathTracing)
		{
			update |= m_renderer.path_tracing.draw_ui();
			if (update)
			{
				m_renderer.path_tracing.reset_frames();
			}
		}
		if (m_render_mode == RenderMode::Hybrid || m_render_mode == RenderMode::AO)
		{
			update |= m_renderer.ao.draw_ui();
		}
		if (m_render_mode == RenderMode::Hybrid || m_render_mode == RenderMode::Reflection)
		{
			update |= m_renderer.reflection.draw_ui();
		}
		if (m_render_mode == RenderMode::Hybrid || m_render_mode == RenderMode::PathTracing)
		{
			update |= m_renderer.tonemap.draw_ui();
		}

		ImGui::End();
	}

	m_renderer.ui.end_frame();
}
