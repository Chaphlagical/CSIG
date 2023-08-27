#include "pipeline/gbuffer.hpp"

#include <spdlog/fmt/fmt.h>

GBufferPass::GBufferPass(const Context &context, const Scene &scene) :
    m_context(&context),
    m_width(context.render_extent.width),
    m_height(context.render_extent.height),
    m_mip_level(static_cast<uint32_t>(std::floor(std::log2(std::max(m_width, m_height))) + 1))
{
	// Create Texture
	for (uint32_t i = 0; i < 2; i++)
	{
		gbufferA[i] = m_context->create_texture_2d(
		    fmt::format("GBuffer A - {}", i),
		    m_width, m_height, VK_FORMAT_R8G8B8A8_UNORM,
		    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		    true);
		gbufferB[i] = m_context->create_texture_2d(
		    fmt::format("GBuffer B - {}", i),
		    m_width, m_height, VK_FORMAT_R16G16B16A16_SFLOAT,
		    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		    true);
		gbufferC[i] = m_context->create_texture_2d(
		    fmt::format("GBuffer C - {}", i),
		    m_width, m_height, VK_FORMAT_R16G16B16A16_SFLOAT,
		    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		    true);
		depth_buffer[i] = m_context->create_texture_2d(
		    fmt::format("Depth Buffer - {}", i),
		    m_width, m_height, VK_FORMAT_D32_SFLOAT,
		    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		    true);
		gbufferA_view[i] = m_context->create_texture_view(
		    fmt::format("GBuffer A - {} View", i),
		    gbufferA[i].vk_image,
		    VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D,
		    {
		        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		        .baseMipLevel   = 0,
		        .levelCount     = m_mip_level,
		        .baseArrayLayer = 0,
		        .layerCount     = 1,
		    });
		gbufferB_view[i] = m_context->create_texture_view(
		    fmt::format("GBuffer B - {} View", i),
		    gbufferB[i].vk_image,
		    VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_VIEW_TYPE_2D,
		    {
		        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		        .baseMipLevel   = 0,
		        .levelCount     = m_mip_level,
		        .baseArrayLayer = 0,
		        .layerCount     = 1,
		    });
		gbufferC_view[i] = m_context->create_texture_view(
		    fmt::format("GBuffer C - {} View", i),
		    gbufferC[i].vk_image,
		    VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_VIEW_TYPE_2D,
		    {
		        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		        .baseMipLevel   = 0,
		        .levelCount     = m_mip_level,
		        .baseArrayLayer = 0,
		        .layerCount     = 1,
		    });
		depth_buffer_view[i] = m_context->create_texture_view(
		    fmt::format("Depth Buffer - {} View", i),
		    depth_buffer[i].vk_image,
		    VK_FORMAT_D32_SFLOAT, VK_IMAGE_VIEW_TYPE_2D,
		    {
		        .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
		        .baseMipLevel   = 0,
		        .levelCount     = m_mip_level,
		        .baseArrayLayer = 0,
		        .layerCount     = 1,
		    });
	}

	m_pipeline_layout = m_context->create_pipeline_layout({scene.descriptor.layout});
	m_pipeline        = m_context->create_graphics_pipeline(m_pipeline_layout)
	                 .add_color_attachment(VK_FORMAT_R8G8B8A8_UNORM)
	                 .add_color_attachment(VK_FORMAT_R16G16B16A16_SFLOAT)
	                 .add_color_attachment(VK_FORMAT_R16G16B16A16_SFLOAT)
	                 .add_depth_stencil(VK_FORMAT_D32_SFLOAT)
	                 .add_viewport({
	                     .x        = 0,
	                     .y        = 0,
	                     .width    = (float) m_width,
	                     .height   = (float) m_height,
	                     .minDepth = 0.f,
	                     .maxDepth = 1.f,
	                 })
	                 .add_scissor({.offset = {0, 0}, .extent = {m_width, m_height}})
	                 .add_shader(VK_SHADER_STAGE_VERTEX_BIT, "gbuffer.slang", "vs_main")
	                 .add_shader(VK_SHADER_STAGE_FRAGMENT_BIT, "gbuffer.slang", "fs_main")
	                 .add_vertex_input_attribute(0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0)
	                 .add_vertex_input_attribute(1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(glm::vec4))
	                 .add_vertex_input_binding(0, 2 * sizeof(glm::vec4))
	                 .create();
}

GBufferPass::~GBufferPass()
{
	for (uint32_t i = 0; i < 2; i++)
	{
		m_context->destroy(gbufferA[i])
		    .destroy(gbufferB[i])
		    .destroy(gbufferC[i])
		    .destroy(depth_buffer[i])
		    .destroy(gbufferA_view[i])
		    .destroy(gbufferB_view[i])
		    .destroy(gbufferC_view[i])
		    .destroy(depth_buffer_view[i]);
	}
	m_context->destroy(m_pipeline_layout)
	    .destroy(m_pipeline);
}

void GBufferPass::init(CommandBufferRecorder &recorder)
{
	auto barrier_builder = recorder.insert_barrier();
	for (uint32_t i = 0; i < 2; i++)
	{
		barrier_builder
		    .add_image_barrier(
		        gbufferA[i].vk_image,
		        0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
		    .add_image_barrier(
		        gbufferB[i].vk_image,
		        0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
		    .add_image_barrier(
		        gbufferC[i].vk_image,
		        0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
		    .add_image_barrier(
		        depth_buffer[i].vk_image,
		        0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
		        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		        {
		            .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
		            .baseMipLevel   = 0,
		            .levelCount     = 1,
		            .baseArrayLayer = 0,
		            .layerCount     = 1,
		        });
	}
	barrier_builder.insert();
}

void GBufferPass::draw(CommandBufferRecorder &recorder, const Scene &scene)
{
	recorder.begin_marker("GBuffer Pass")
	    .bind_vertex_buffers({scene.buffer.vertex.vk_buffer})
	    .bind_index_buffer({scene.buffer.index.vk_buffer})
	    .add_color_attachment(gbufferA_view[m_context->ping_pong])
	    .add_color_attachment(gbufferB_view[m_context->ping_pong])
	    .add_color_attachment(gbufferC_view[m_context->ping_pong])
	    .add_depth_attachment(depth_buffer_view[m_context->ping_pong])
	    .bind_pipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline)
	    .bind_descriptor_set(VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline_layout, {scene.descriptor.set})
	    .begin_rendering(m_context->render_extent.width, m_context->render_extent.height)
	    .draw_indexed_indirect(scene.buffer.indirect_draw.vk_buffer, scene.scene_info.instance_count)
	    .end_rendering()
	    .end_marker();
}
