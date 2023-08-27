#include "pipeline//ui.hpp"
#include "ui/imgui_impl_vulkan.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>

static VkInstance g_instance = nullptr;

inline PFN_vkVoidFunction load_vulkan_function(char const *function, void *)
{
	return vkGetInstanceProcAddr(g_instance, function);
}

UIPass::UIPass(const Context &context) :
    m_context(&context)
{
	create_render_pass();
	create_frame_buffer();

	ImGui::CreateContext();

	set_style();

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

	m_context->record_command()
	    .begin()
	    .execute(ImGui_ImplVulkan_CreateFontsTexture)
	    .end()
	    .flush();

	ImGui_ImplVulkan_DestroyFontUploadObjects();

	io.Fonts->AddFontFromFileTTF("assets/fonts/ArialUnicodeMS.ttf", 20.0f, NULL, io.Fonts->GetGlyphRangesChineseFull());
}

UIPass::~UIPass()
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

void UIPass::render(CommandBufferRecorder &recorder, uint32_t frame_idx)
{
	recorder
	    .begin_marker("ImGui")
	    .begin_render_pass(m_context->extent.width, m_context->extent.height, m_render_pass, m_frame_buffers[frame_idx])
	    .execute([&](VkCommandBuffer cmd_buffer) { ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd_buffer); })
	    .end_render_pass()
	    .end_marker();
}

void UIPass::begin_frame()
{
	ImGui_ImplVulkan_NewFrame();
	ImGui_ImplGlfw_NewFrame();
	ImGui::NewFrame();
}

void UIPass::end_frame()
{
	ImGui::EndFrame();
	ImGui::Render();
}

void UIPass::create_render_pass()
{
	std::array<VkAttachmentDescription, 1> attachments = {};
	// Color attachment
	attachments[0].format         = m_context->vk_format;
	attachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
	attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
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

void UIPass::create_frame_buffer()
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

void UIPass::set_style()
{
	ImGuiIO &io = ImGui::GetIO();
	(void) io;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

	// Set fonts
	ImFontConfig config;
	config.MergeMode        = true;
	config.GlyphMinAdvanceX = 13.0f;
	io.Fonts->AddFontFromFileTTF("./Asset/Font/ArialUnicodeMS.ttf", 20.0f, NULL, io.Fonts->GetGlyphRangesChineseFull());

	// When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to regular ones.
	ImGuiStyle &style  = ImGui::GetStyle();
	auto        colors = style.Colors;

	colors[ImGuiCol_Text]                  = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
	colors[ImGuiCol_TextDisabled]          = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
	colors[ImGuiCol_WindowBg]              = ImVec4(0.06f, 0.06f, 0.06f, 0.94f);
	colors[ImGuiCol_ChildBg]               = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
	colors[ImGuiCol_PopupBg]               = ImVec4(0.08f, 0.08f, 0.08f, 0.94f);
	colors[ImGuiCol_Border]                = ImVec4(0.43f, 0.43f, 0.50f, 0.50f);
	colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
	colors[ImGuiCol_FrameBg]               = ImVec4(0.44f, 0.44f, 0.44f, 0.60f);
	colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.57f, 0.57f, 0.57f, 0.70f);
	colors[ImGuiCol_FrameBgActive]         = ImVec4(0.76f, 0.76f, 0.76f, 0.80f);
	colors[ImGuiCol_TitleBg]               = ImVec4(0.04f, 0.04f, 0.04f, 1.00f);
	colors[ImGuiCol_TitleBgActive]         = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
	colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.00f, 0.00f, 0.00f, 0.60f);
	colors[ImGuiCol_MenuBarBg]             = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
	colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.02f, 0.02f, 0.02f, 0.53f);
	colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.31f, 0.31f, 0.31f, 1.00f);
	colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.41f, 0.41f, 0.41f, 1.00f);
	colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.51f, 0.51f, 0.51f, 1.00f);
	colors[ImGuiCol_CheckMark]             = ImVec4(0.13f, 0.75f, 0.55f, 0.80f);
	colors[ImGuiCol_SliderGrab]            = ImVec4(0.13f, 0.75f, 0.75f, 0.80f);
	colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.13f, 0.75f, 1.00f, 0.80f);
	colors[ImGuiCol_Button]                = ImVec4(0.13f, 0.75f, 0.55f, 0.40f);
	colors[ImGuiCol_ButtonHovered]         = ImVec4(0.13f, 0.75f, 0.75f, 0.60f);
	colors[ImGuiCol_ButtonActive]          = ImVec4(0.13f, 0.75f, 1.00f, 0.80f);
	colors[ImGuiCol_Header]                = ImVec4(0.13f, 0.75f, 0.55f, 0.40f);
	colors[ImGuiCol_HeaderHovered]         = ImVec4(0.13f, 0.75f, 0.75f, 0.60f);
	colors[ImGuiCol_HeaderActive]          = ImVec4(0.13f, 0.75f, 1.00f, 0.80f);
	colors[ImGuiCol_Separator]             = ImVec4(0.13f, 0.75f, 0.55f, 0.40f);
	colors[ImGuiCol_SeparatorHovered]      = ImVec4(0.13f, 0.75f, 0.75f, 0.60f);
	colors[ImGuiCol_SeparatorActive]       = ImVec4(0.13f, 0.75f, 1.00f, 0.80f);
	colors[ImGuiCol_ResizeGrip]            = ImVec4(0.13f, 0.75f, 0.55f, 0.40f);
	colors[ImGuiCol_ResizeGripHovered]     = ImVec4(0.13f, 0.75f, 0.75f, 0.60f);
	colors[ImGuiCol_ResizeGripActive]      = ImVec4(0.13f, 0.75f, 1.00f, 0.80f);
	colors[ImGuiCol_Tab]                   = ImVec4(0.13f, 0.75f, 0.55f, 0.80f);
	colors[ImGuiCol_TabHovered]            = ImVec4(0.13f, 0.75f, 0.75f, 0.80f);
	colors[ImGuiCol_TabActive]             = ImVec4(0.13f, 0.75f, 1.00f, 0.80f);
	colors[ImGuiCol_TabUnfocused]          = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
	colors[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.36f, 0.36f, 0.36f, 0.54f);
	colors[ImGuiCol_PlotLines]             = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
	colors[ImGuiCol_PlotLinesHovered]      = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
	colors[ImGuiCol_PlotHistogram]         = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
	colors[ImGuiCol_PlotHistogramHovered]  = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
	colors[ImGuiCol_TableHeaderBg]         = ImVec4(0.19f, 0.19f, 0.20f, 1.00f);
	colors[ImGuiCol_TableBorderStrong]     = ImVec4(0.31f, 0.31f, 0.35f, 1.00f);
	colors[ImGuiCol_TableBorderLight]      = ImVec4(0.23f, 0.23f, 0.25f, 1.00f);
	colors[ImGuiCol_TableRowBg]            = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
	colors[ImGuiCol_TableRowBgAlt]         = ImVec4(1.00f, 1.00f, 1.00f, 0.07f);
	colors[ImGuiCol_TextSelectedBg]        = ImVec4(0.26f, 0.59f, 0.98f, 0.35f);
	colors[ImGuiCol_DragDropTarget]        = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
	colors[ImGuiCol_NavHighlight]          = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
	colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
	colors[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
	colors[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);

	style.WindowPadding     = ImVec2(8.00f, 8.00f);
	style.FramePadding      = ImVec2(5.00f, 2.00f);
	style.CellPadding       = ImVec2(6.00f, 6.00f);
	style.ItemSpacing       = ImVec2(6.00f, 6.00f);
	style.ItemInnerSpacing  = ImVec2(6.00f, 6.00f);
	style.TouchExtraPadding = ImVec2(0.00f, 0.00f);
	style.IndentSpacing     = 25;
	style.ScrollbarSize     = 15;
	style.GrabMinSize       = 10;
	style.WindowBorderSize  = 1;
	style.ChildBorderSize   = 1;
	style.PopupBorderSize   = 1;
	style.FrameBorderSize   = 1;
	style.TabBorderSize     = 1;
	style.WindowRounding    = 7;
	style.ChildRounding     = 4;
	style.FrameRounding     = 3;
	style.PopupRounding     = 4;
	style.ScrollbarRounding = 9;
	style.GrabRounding      = 3;
	style.LogSliderDeadzone = 4;
	style.TabRounding       = 4;
}