#include "application.hpp"
#include "core/log.hpp"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <vector>

inline glm::vec3 smooth_step(const glm::vec3 &v1, const glm::vec3 &v2, float t)
{
	t = glm::clamp(t, 0.f, 1.f);
	t = t * t * (3.f - 2.f * t);

	glm::vec3 v = glm::mix(v1, v2, t);

	return v;
}

bool is_key_pressed(GLFWwindow *window, uint32_t keycode)
{
	auto state = glfwGetKey(window, keycode);
	return state == GLFW_PRESS || state == GLFW_REPEAT;
}

Application::Application(const ApplicationConfig &config) :
    m_context(config.context_config),
    m_renderer{
        .ui{m_context},
        .gbuffer_pass{m_context},
        .raytraced_ao{m_context}},
    m_scene(config.scene_file, m_context),
    m_blue_noise(m_context)
{
	glfwSetWindowUserPointer(m_context.window, this);
	glfwSetScrollCallback(m_context.window, [](GLFWwindow *window, double xoffset, double yoffset) {
		Application *app = (Application *) glfwGetWindowUserPointer(window);
		app->m_camera.speed += static_cast<float>(yoffset) * 0.3f;
	});

	// Init command buffer
	{
		VkCommandBufferAllocateInfo allocate_info =
		    {
		        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		        .commandPool        = m_context.graphics_cmd_pool,
		        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		        .commandBufferCount = 3,
		    };
		vkAllocateCommandBuffers(m_context.vk_device, &allocate_info, m_cmd_buffers.data());
	}

	// Image transition
	{
		VkCommandBuffer cmd_buffer = VK_NULL_HANDLE;
		VkFence         fence      = VK_NULL_HANDLE;

		VkFenceCreateInfo create_info = {
		    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		    .flags = 0,
		};
		vkCreateFence(m_context.vk_device, &create_info, nullptr, &fence);

		VkCommandBufferAllocateInfo allocate_info =
		    {
		        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		        .commandPool        = m_context.graphics_cmd_pool,
		        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		        .commandBufferCount = 1,
		    };
		vkAllocateCommandBuffers(m_context.vk_device, &allocate_info, &cmd_buffer);

		VkCommandBufferBeginInfo begin_info = {
		    .sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		    .flags            = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		    .pInheritanceInfo = nullptr,
		};

		vkBeginCommandBuffer(cmd_buffer, &begin_info);
		m_renderer.gbuffer_pass.init(cmd_buffer);
		m_renderer.raytraced_ao.init(cmd_buffer);

		std::vector<VkImageMemoryBarrier> image_barriers;
		for (auto &image : m_context.swapchain_images)
		{
			image_barriers.push_back(VkImageMemoryBarrier{
			    .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			    .srcAccessMask       = 0,
			    .dstAccessMask       = VK_ACCESS_MEMORY_READ_BIT,
			    .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
			    .newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			    .image               = image,
			    .subresourceRange    = VkImageSubresourceRange{
			           .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
			           .baseMipLevel   = 0,
			           .levelCount     = 1,
			           .baseArrayLayer = 0,
			           .layerCount     = 1,
                },
			});
		}

		vkCmdPipelineBarrier(
		    cmd_buffer,
		    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		    0, 0, nullptr, 0, nullptr, 3, image_barriers.data());

		vkEndCommandBuffer(cmd_buffer);

		VkSubmitInfo submit_info = {
		    .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		    .waitSemaphoreCount   = 0,
		    .pWaitSemaphores      = nullptr,
		    .pWaitDstStageMask    = 0,
		    .commandBufferCount   = 1,
		    .pCommandBuffers      = &cmd_buffer,
		    .signalSemaphoreCount = 0,
		    .pSignalSemaphores    = nullptr,
		};

		vkQueueSubmit(m_context.graphics_queue, 1, &submit_info, fence);

		vkWaitForFences(m_context.vk_device, 1, &fence, VK_TRUE, UINT64_MAX);
		vkResetFences(m_context.vk_device, 1, &fence);

		vkDestroyFence(m_context.vk_device, fence, nullptr);
		vkFreeCommandBuffers(m_context.vk_device, m_context.graphics_cmd_pool, 1, &cmd_buffer);
	}

	update();
}

Application::~Application()
{
	vkDeviceWaitIdle(m_context.vk_device);
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

		auto cmd_buffer = m_cmd_buffers[m_current_frame];

		update();
		update_ui();

		begin_render();
		render(cmd_buffer);
		end_render();

		m_current_frame = (m_current_frame + 1) % 3;
		m_num_frames++;
	}
}

void Application::begin_render()
{
	m_image_index = 0;
	vkAcquireNextImageKHR(m_context.vk_device, m_context.vk_swapchain, UINT64_MAX, m_context.present_complete, nullptr, &m_image_index);

	vkWaitForFences(m_context.vk_device, 1, &m_context.fences[m_current_frame], VK_TRUE, UINT64_MAX);
	vkResetFences(m_context.vk_device, 1, &m_context.fences[m_current_frame]);

	VkCommandBufferBeginInfo begin_info = {
	    .sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	    .flags            = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	    .pInheritanceInfo = nullptr,
	};

	vkBeginCommandBuffer(m_cmd_buffers[m_current_frame], &begin_info);
}

void Application::end_render()
{
	vkEndCommandBuffer(m_cmd_buffers[m_current_frame]);

	VkPipelineStageFlags pipeline_stage_flags[] = {VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT};

	VkSubmitInfo submit_info = {
	    .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .waitSemaphoreCount   = 1,
	    .pWaitSemaphores      = &m_context.present_complete,
	    .pWaitDstStageMask    = pipeline_stage_flags,
	    .commandBufferCount   = 1,
	    .pCommandBuffers      = &m_cmd_buffers[m_current_frame],
	    .signalSemaphoreCount = 1,
	    .pSignalSemaphores    = &m_context.render_complete,
	};

	vkQueueSubmit(m_context.graphics_queue, 1, &submit_info, m_context.fences[m_current_frame]);

	VkPresentInfoKHR present_info   = {};
	present_info.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present_info.pNext              = NULL;
	present_info.swapchainCount     = 1;
	present_info.pSwapchains        = &m_context.vk_swapchain;
	present_info.pImageIndices      = &m_image_index;
	present_info.pWaitSemaphores    = &m_context.render_complete;
	present_info.waitSemaphoreCount = 1;

	vkQueuePresentKHR(m_context.present_queue, &present_info);
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
		m_renderer.raytraced_ao.draw_ui();
		ImGui::End();
	}
	m_renderer.ui.end_frame();
}

void Application::update()
{
	static bool hide_cursor = false;
	if (ImGui::IsMouseDown(ImGuiMouseButton_Right) || m_update)
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
		m_camera.pitch = glm::clamp(m_camera.pitch, -98.f, 98.f);

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
		m_camera.proj = glm::perspective(glm::radians(60.f), static_cast<float>(m_context.extent.width) / static_cast<float>(m_context.extent.height), 0.01f, 1000.f);
	}
	else
	{
		m_camera.velocity = glm::vec3(0.f);
		hide_cursor       = false;
	}

	if (m_update)
	{
		m_renderer.gbuffer_pass.update(m_scene);
		m_renderer.raytraced_ao.update(m_scene, m_blue_noise, m_renderer.gbuffer_pass.gbufferB_view, m_renderer.gbuffer_pass.depth_stencil_view);
	}

	// Copy to device
	{
		GlobalBuffer *mapped_data = nullptr;
		vmaMapMemory(m_context.vma_allocator, m_scene.global_buffer.vma_allocation, reinterpret_cast<void **>(&mapped_data));

		GlobalBuffer global_buffer = {
		    .view_inv             = glm::inverse(m_camera.view),
		    .projection_inv       = glm::inverse(m_camera.proj),
		    .view_projection_inv  = glm::inverse(m_camera.proj * m_camera.view),
		    .view_projection      = m_camera.proj * m_camera.view,
		    .prev_view_projection = mapped_data->view_projection,
		    .cam_pos              = glm::vec4(m_camera.position, static_cast<float>(m_num_frames)),
		    .jitter               = glm::vec4(0.f),
		};

		std::memcpy(mapped_data, &global_buffer, sizeof(GlobalBuffer));
		vmaUnmapMemory(m_context.vma_allocator, m_scene.global_buffer.vma_allocation);
		vmaFlushAllocation(m_context.vma_allocator, m_scene.global_buffer.vma_allocation, 0, sizeof(GlobalBuffer));
		mapped_data = nullptr;
	}

	m_update = false;
}

void Application::render(VkCommandBuffer cmd_buffer)
{
	m_renderer.gbuffer_pass.draw(cmd_buffer, m_scene);

	{
		VkImageMemoryBarrier image_barriers[] = {
		    {
		        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
		        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image               = m_renderer.gbuffer_pass.gbufferB.vk_image,
		        .subresourceRange    = VkImageSubresourceRange{
		               .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		               .baseMipLevel   = 0,
		               .levelCount     = m_renderer.gbuffer_pass.mip_level,
		               .baseArrayLayer = 0,
		               .layerCount     = 1,
                },
		    },
		    {
		        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
		        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
		        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image               = m_renderer.gbuffer_pass.depth_buffer.vk_image,
		        .subresourceRange    = VkImageSubresourceRange{
		               .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
		               .baseMipLevel   = 0,
		               .levelCount     = m_renderer.gbuffer_pass.mip_level,
		               .baseArrayLayer = 0,
		               .layerCount     = 1,
                },
		    },
		    {
		        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask       = VK_ACCESS_MEMORY_READ_BIT,
		        .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
		        .oldLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image               = m_context.swapchain_images[m_image_index],
		        .subresourceRange    = VkImageSubresourceRange{
		               .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		               .baseMipLevel   = 0,
		               .levelCount     = 1,
		               .baseArrayLayer = 0,
		               .layerCount     = 1,
                },
		    },
		};
		vkCmdPipelineBarrier(
		    cmd_buffer,
		    VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		    VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		    0, 0, nullptr, 0, nullptr, 3, image_barriers);
	}
	present(cmd_buffer, m_renderer.gbuffer_pass.gbufferA.vk_image);

	m_renderer.raytraced_ao.draw(cmd_buffer);

	{
		VkImageMemoryBarrier image_barriers[] = {
		    {
		        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
		        .dstAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		        .newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image               = m_renderer.gbuffer_pass.gbufferA.vk_image,
		        .subresourceRange    = VkImageSubresourceRange{
		               .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		               .baseMipLevel   = 0,
		               .levelCount     = m_renderer.gbuffer_pass.mip_level,
		               .baseArrayLayer = 0,
		               .layerCount     = 1,
                },
		    },
		    {
		        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask       = VK_ACCESS_SHADER_READ_BIT,
		        .dstAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		        .oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		        .newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image               = m_renderer.gbuffer_pass.gbufferB.vk_image,
		        .subresourceRange    = VkImageSubresourceRange{
		               .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		               .baseMipLevel   = 0,
		               .levelCount     = m_renderer.gbuffer_pass.mip_level,
		               .baseArrayLayer = 0,
		               .layerCount     = 1,
                },
		    },
		    {
		        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
		        .dstAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		        .newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image               = m_renderer.gbuffer_pass.gbufferC.vk_image,
		        .subresourceRange    = VkImageSubresourceRange{
		               .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		               .baseMipLevel   = 0,
		               .levelCount     = m_renderer.gbuffer_pass.mip_level,
		               .baseArrayLayer = 0,
		               .layerCount     = 1,
                },
		    },
		    {
		        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask       = VK_ACCESS_SHADER_READ_BIT,
		        .dstAccessMask       = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
		        .oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		        .newLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image               = m_renderer.gbuffer_pass.depth_buffer.vk_image,
		        .subresourceRange    = VkImageSubresourceRange{
		               .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
		               .baseMipLevel   = 0,
		               .levelCount     = m_renderer.gbuffer_pass.mip_level,
		               .baseArrayLayer = 0,
		               .layerCount     = 1,
                },
		    },
		    {
		        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
		        .dstAccessMask       = VK_ACCESS_MEMORY_READ_BIT,
		        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		        .newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image               = m_context.swapchain_images[m_image_index],
		        .subresourceRange    = VkImageSubresourceRange{
		               .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		               .baseMipLevel   = 0,
		               .levelCount     = 1,
		               .baseArrayLayer = 0,
		               .layerCount     = 1,
                },
		    },

		};
		vkCmdPipelineBarrier(
		    cmd_buffer,
		    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
		    VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
		    0, 0, nullptr, 0, nullptr, 5, image_barriers);
	}

	// Draw UI
	m_renderer.ui.render(cmd_buffer, m_current_frame);
}

void Application::present(VkCommandBuffer cmd_buffer, VkImage image)
{
	VkImageBlit image_blit = {
	    .srcSubresource = {
	        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	        .mipLevel       = 0,
	        .baseArrayLayer = 0,
	        .layerCount     = 1,
	    },
	    .srcOffsets     = {{0, static_cast<int32_t>(m_context.extent.height), 0}, {static_cast<int32_t>(m_context.extent.width), 0, 1}},
	    .dstSubresource = {
	        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	        .mipLevel       = 0,
	        .baseArrayLayer = 0,
	        .layerCount     = 1,
	    },
	    .dstOffsets{{0, 0, 0}, {static_cast<int32_t>(m_context.extent.width), static_cast<int32_t>(m_context.extent.height), 1}},
	};

	vkCmdBlitImage(cmd_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_context.swapchain_images[m_image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &image_blit, VK_FILTER_LINEAR);
}
