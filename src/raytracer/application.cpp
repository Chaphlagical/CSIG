#include "application.hpp"
#include "core/log.hpp"

#include <GLFW/glfw3.h>
#include <imgui.h>

#include <vector>

Application::Application(const ApplicationConfig &config) :
    m_context(config.context_config),
    m_renderer{std::make_unique<UI>(m_context)},
    m_scene(config.scene_file, m_context)
{
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
}

Application::~Application()
{
}

void Application::run()
{
	while (!glfwWindowShouldClose(m_context.window))
	{
		glfwPollEvents();
		update();

		begin_render();
		m_renderer.ui->render(m_cmd_buffers[m_current_frame], m_current_frame);
		end_render();

		m_current_frame = (m_current_frame + 1) % 3;
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

void Application::update()
{
	m_renderer.ui->begin_frame();

	if (ImGui::IsKeyPressed(ImGuiKey_G, false))
	{
		m_enable_ui = !m_enable_ui;
	}

	if (m_enable_ui)
	{
		ImGui::Begin("UI", &m_enable_ui);
		ImGui::Text("CSIG 2023 RayTracer");
		ImGui::Text("FPS: %f", ImGui::GetIO().Framerate);

		ImGui::End();
	}
	m_renderer.ui->end_frame();
}
