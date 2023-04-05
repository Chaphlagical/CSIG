#include "render/pipeline/ui.hpp"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

static VkInstance g_instance = nullptr;

inline PFN_vkVoidFunction load_vulkan_function(char const *function, void *)
{
	return vkGetInstanceProcAddr(g_instance, function);
}

UI::UI(const Context &context) :
    m_context(&context)
{
	create_render_pass();
	create_frame_buffer();

	ImGui::CreateContext();

	ImGuiIO &io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;        // Enable Keyboard Controls
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;         // Enable Gamepad Controls

	g_instance = context.vk_instance;

	ImGui_ImplVulkan_LoadFunctions(load_vulkan_function);
	ImGui_ImplGlfw_InitForVulkan(m_context->window, true);

	ImGui_ImplVulkan_InitInfo init_info = {
	    .Instance       = m_context->vk_instance,
	    .PhysicalDevice = m_context->vk_physical_device,
	    .Device         = m_context->vk_device,
	    .QueueFamily    = m_context->graphics_family.value(),
	    .Queue          = m_context->graphics_queue,
	    .PipelineCache  = m_context->vk_pipeline_cache,
	    .DescriptorPool = m_context->vk_descriptor_pool,
	    .MinImageCount  = 3,
	    .ImageCount     = 3,
	    .MSAASamples    = VK_SAMPLE_COUNT_1_BIT,
	    .Allocator      = nullptr,
	};

	ImGui_ImplVulkan_Init(&init_info, m_render_pass);

	{
		VkCommandBuffer cmd_buffer = VK_NULL_HANDLE;
		VkFence         fence      = VK_NULL_HANDLE;

		VkFenceCreateInfo create_info = {
		    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		    .flags = 0,
		};
		vkCreateFence(m_context->vk_device, &create_info, nullptr, &fence);

		VkCommandBufferAllocateInfo allocate_info =
		    {
		        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		        .commandPool        = m_context->graphics_cmd_pool,
		        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		        .commandBufferCount = 1,
		    };
		vkAllocateCommandBuffers(m_context->vk_device, &allocate_info, &cmd_buffer);

		VkCommandBufferBeginInfo begin_info = {
		    .sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		    .flags            = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		    .pInheritanceInfo = nullptr,
		};

		vkBeginCommandBuffer(cmd_buffer, &begin_info);
		ImGui_ImplVulkan_CreateFontsTexture(cmd_buffer);
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

		vkQueueSubmit(m_context->graphics_queue, 1, &submit_info, fence);

		vkWaitForFences(m_context->vk_device, 1, &fence, VK_TRUE, UINT64_MAX);
		vkResetFences(m_context->vk_device, 1, &fence);

		vkDestroyFence(m_context->vk_device, fence, nullptr);
		vkFreeCommandBuffers(m_context->vk_device, m_context->graphics_cmd_pool, 1, &cmd_buffer);

		ImGui_ImplVulkan_DestroyFontUploadObjects();
	}
}

UI::~UI()
{
	vkDeviceWaitIdle(m_context->vk_device);

	ImGui_ImplVulkan_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();

	for (auto &frame_buffer : m_frame_buffers)
	{
		vkDestroyFramebuffer(m_context->vk_device, frame_buffer, nullptr);
	}

	vkDestroyRenderPass(m_context->vk_device, m_render_pass, nullptr);
}

void UI::render(VkCommandBuffer cmd_buffer, uint32_t frame_idx)
{
	VkRect2D area            = {};
	area.extent.width        = m_context->extent.width;
	area.extent.height       = m_context->extent.height;
	VkClearValue clear_value = {};

	VkRenderPassBeginInfo begin_info = {};
	begin_info.sType                 = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	begin_info.renderPass            = m_render_pass;
	begin_info.renderArea            = area;
	begin_info.framebuffer           = m_frame_buffers[frame_idx];
	begin_info.clearValueCount       = 1;
	begin_info.pClearValues          = &clear_value;

	vkCmdBeginRenderPass(cmd_buffer, &begin_info, VK_SUBPASS_CONTENTS_INLINE);
	ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd_buffer);
	vkCmdEndRenderPass(cmd_buffer);
}

void UI::begin_frame()
{
	ImGui_ImplVulkan_NewFrame();
	ImGui_ImplGlfw_NewFrame();
	ImGui::NewFrame();
}

void UI::end_frame()
{
	ImGui::EndFrame();
	ImGui::Render();
}

void UI::create_render_pass()
{
	std::array<VkAttachmentDescription, 1> attachments = {};
	// Color attachment
	attachments[0].format         = m_context->vk_format;
	attachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
	attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[0].finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	VkAttachmentReference colorReference = {};
	colorReference.attachment            = 0;
	colorReference.layout                = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpassDescription    = {};
	subpassDescription.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpassDescription.colorAttachmentCount    = 1;
	subpassDescription.pColorAttachments       = &colorReference;
	subpassDescription.pDepthStencilAttachment = nullptr;
	subpassDescription.inputAttachmentCount    = 0;
	subpassDescription.pInputAttachments       = nullptr;
	subpassDescription.preserveAttachmentCount = 0;
	subpassDescription.pPreserveAttachments    = nullptr;
	subpassDescription.pResolveAttachments     = nullptr;

	// Subpass dependencies for layout transitions
	std::array<VkSubpassDependency, 2> dependencies;

	dependencies[0].srcSubpass      = VK_SUBPASS_EXTERNAL;
	dependencies[0].dstSubpass      = 0;
	dependencies[0].srcStageMask    = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
	dependencies[0].dstStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[0].srcAccessMask   = VK_ACCESS_MEMORY_READ_BIT;
	dependencies[0].dstAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

	dependencies[1].srcSubpass      = 0;
	dependencies[1].dstSubpass      = VK_SUBPASS_EXTERNAL;
	dependencies[1].srcStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[1].dstStageMask    = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
	dependencies[1].srcAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependencies[1].dstAccessMask   = VK_ACCESS_MEMORY_READ_BIT;
	dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

	VkRenderPassCreateInfo renderPassInfo = {};
	renderPassInfo.sType                  = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	renderPassInfo.attachmentCount        = static_cast<uint32_t>(attachments.size());
	renderPassInfo.pAttachments           = attachments.data();
	renderPassInfo.subpassCount           = 1;
	renderPassInfo.pSubpasses             = &subpassDescription;
	renderPassInfo.dependencyCount        = static_cast<uint32_t>(dependencies.size());
	renderPassInfo.pDependencies          = dependencies.data();

	vkCreateRenderPass(m_context->vk_device, &renderPassInfo, nullptr, &m_render_pass);
}

void UI::create_frame_buffer()
{
	VkFramebufferCreateInfo create_info = {};
	create_info.sType                   = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	create_info.pNext                   = NULL;
	create_info.renderPass              = m_render_pass;
	create_info.attachmentCount         = 1;

	// Create frame buffers for every swap chain image
	for (uint32_t i = 0; i < m_frame_buffers.size(); i++)
	{
		VkImageView view         = m_context->swapchain_image_views[i];
		create_info.pAttachments = &view;
		create_info.width        = m_context->extent.width;
		create_info.height       = m_context->extent.height;
		create_info.layers       = 1;
		vkCreateFramebuffer(m_context->vk_device, &create_info, nullptr, &m_frame_buffers[i]);
	}
}
