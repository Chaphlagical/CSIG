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

	descriptor.layout = m_context->create_descriptor_layout()
	                        .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                        .add_descriptor_binding(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                        .add_descriptor_binding(2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                        .add_descriptor_binding(3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                        .add_descriptor_binding(4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                        .add_descriptor_binding(5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                        .add_descriptor_binding(6, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                        .add_descriptor_binding(7, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                        .create();
	descriptor.sets = m_context->allocate_descriptor_sets<2>(descriptor.layout);

	for (uint32_t i = 0; i < 2; i++)
	{
		m_context->update_descriptor()
		    .write_sampled_images(0, {gbufferA_view[i]})
		    .write_sampled_images(1, {gbufferB_view[i]})
		    .write_sampled_images(2, {gbufferC_view[i]})
		    .write_sampled_images(3, {depth_buffer_view[i]})
		    .write_sampled_images(4, {gbufferA_view[!i]})
		    .write_sampled_images(5, {gbufferB_view[!i]})
		    .write_sampled_images(6, {gbufferC_view[!i]})
		    .write_sampled_images(7, {depth_buffer_view[!i]})
		    .update(descriptor.sets[i]);
	}

	auto barrier_builder = m_context->record_command()
	                           .begin()
	                           .insert_barrier();
	for (uint32_t i = 0; i < 2; i++)
	{
		barrier_builder
		    .add_image_barrier(
		        gbufferA[i].vk_image,
		        0, VK_ACCESS_SHADER_READ_BIT,
		        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		        {
		            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		            .baseMipLevel   = 0,
		            .levelCount     = m_mip_level,
		            .baseArrayLayer = 0,
		            .layerCount     = 1,
		        })
		    .add_image_barrier(
		        gbufferB[i].vk_image,
		        0, VK_ACCESS_SHADER_READ_BIT,
		        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		        {
		            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		            .baseMipLevel   = 0,
		            .levelCount     = m_mip_level,
		            .baseArrayLayer = 0,
		            .layerCount     = 1,
		        })
		    .add_image_barrier(
		        gbufferC[i].vk_image,
		        0, VK_ACCESS_SHADER_READ_BIT,
		        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		        {
		            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		            .baseMipLevel   = 0,
		            .levelCount     = m_mip_level,
		            .baseArrayLayer = 0,
		            .layerCount     = 1,
		        })
		    .add_image_barrier(
		        depth_buffer[i].vk_image,
		        0, VK_ACCESS_SHADER_READ_BIT,
		        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		        {
		            .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
		            .baseMipLevel   = 0,
		            .levelCount     = m_mip_level,
		            .baseArrayLayer = 0,
		            .layerCount     = 1,
		        });
	}
	barrier_builder.insert()
	    .end()
	    .flush();
}

GBufferPass::~GBufferPass()
{
	m_context->destroy(gbufferA)
	    .destroy(gbufferB)
	    .destroy(gbufferC)
	    .destroy(depth_buffer)
	    .destroy(gbufferA_view)
	    .destroy(gbufferB_view)
	    .destroy(gbufferC_view)
	    .destroy(depth_buffer_view)
	    .destroy(descriptor.layout)
	    .destroy(descriptor.sets)
	    .destroy(m_pipeline_layout)
	    .destroy(m_pipeline);
}

void GBufferPass::draw(CommandBufferRecorder &recorder, const Scene &scene)
{
	recorder.begin_marker("GBuffer Pass")
	    .begin_marker("Render GBuffer")
	    .insert_barrier()
	    .add_image_barrier(
	        gbufferA[m_context->ping_pong].vk_image,
	        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	        {
	            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	            .baseMipLevel   = 0,
	            .levelCount     = m_mip_level,
	            .baseArrayLayer = 0,
	            .layerCount     = 1,
	        })
	    .add_image_barrier(
	        gbufferB[m_context->ping_pong].vk_image,
	        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	        {
	            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	            .baseMipLevel   = 0,
	            .levelCount     = m_mip_level,
	            .baseArrayLayer = 0,
	            .layerCount     = 1,
	        })
	    .add_image_barrier(
	        gbufferC[m_context->ping_pong].vk_image,
	        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	        {
	            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	            .baseMipLevel   = 0,
	            .levelCount     = m_mip_level,
	            .baseArrayLayer = 0,
	            .layerCount     = 1,
	        })
	    .add_image_barrier(
	        depth_buffer[m_context->ping_pong].vk_image,
	        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
	        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
	        {
	            .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
	            .baseMipLevel   = 0,
	            .levelCount     = m_mip_level,
	            .baseArrayLayer = 0,
	            .layerCount     = 1,
	        })
	    .insert()
	    .bind_pipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline)
	    .bind_descriptor_set(VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline_layout, {scene.descriptor.set})
	    .bind_vertex_buffers({scene.buffer.vertex.vk_buffer})
	    .bind_index_buffer({scene.buffer.index.vk_buffer})
	    .add_color_attachment(gbufferA_view[m_context->ping_pong])
	    .add_color_attachment(gbufferB_view[m_context->ping_pong])
	    .add_color_attachment(gbufferC_view[m_context->ping_pong])
	    .add_depth_attachment(depth_buffer_view[m_context->ping_pong])
	    .begin_rendering(m_context->render_extent.width, m_context->render_extent.height)
	    .draw_indexed_indirect(scene.buffer.indirect_draw.vk_buffer, scene.scene_info.instance_count)
	    .end_rendering()
	    .end_marker()
	    .begin_marker("Generate Mipmap")
	    .insert_barrier()
	    .add_image_barrier(
	        gbufferA[m_context->ping_pong].vk_image,
	        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
	        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	        {
	            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	            .baseMipLevel   = 0,
	            .levelCount     = m_mip_level,
	            .baseArrayLayer = 0,
	            .layerCount     = 1,
	        })
	    .add_image_barrier(
	        gbufferB[m_context->ping_pong].vk_image,
	        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
	        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	        {
	            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	            .baseMipLevel   = 0,
	            .levelCount     = m_mip_level,
	            .baseArrayLayer = 0,
	            .layerCount     = 1,
	        })
	    .add_image_barrier(
	        gbufferC[m_context->ping_pong].vk_image,
	        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
	        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	        {
	            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	            .baseMipLevel   = 0,
	            .levelCount     = m_mip_level,
	            .baseArrayLayer = 0,
	            .layerCount     = 1,
	        })
	    .add_image_barrier(
	        depth_buffer[m_context->ping_pong].vk_image,
	        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
	        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	        {
	            .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
	            .baseMipLevel   = 0,
	            .levelCount     = m_mip_level,
	            .baseArrayLayer = 0,
	            .layerCount     = 1,
	        })
	    .insert()
	    .generate_mipmap(gbufferA[m_context->ping_pong].vk_image, m_width, m_height, m_mip_level)
	    .generate_mipmap(gbufferB[m_context->ping_pong].vk_image, m_width, m_height, m_mip_level)
	    .generate_mipmap(gbufferC[m_context->ping_pong].vk_image, m_width, m_height, m_mip_level)
	    .generate_mipmap(depth_buffer[m_context->ping_pong].vk_image, m_width, m_height, m_mip_level, 1, VK_IMAGE_ASPECT_DEPTH_BIT)
	    .insert_barrier()
	    .add_image_barrier(
	        gbufferA[m_context->ping_pong].vk_image,
	        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        {
	            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	            .baseMipLevel   = 0,
	            .levelCount     = m_mip_level,
	            .baseArrayLayer = 0,
	            .layerCount     = 1,
	        })
	    .add_image_barrier(
	        gbufferB[m_context->ping_pong].vk_image,
	        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        {
	            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	            .baseMipLevel   = 0,
	            .levelCount     = m_mip_level,
	            .baseArrayLayer = 0,
	            .layerCount     = 1,
	        })
	    .add_image_barrier(
	        gbufferC[m_context->ping_pong].vk_image,
	        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        {
	            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	            .baseMipLevel   = 0,
	            .levelCount     = m_mip_level,
	            .baseArrayLayer = 0,
	            .layerCount     = 1,
	        })
	    .add_image_barrier(
	        depth_buffer[m_context->ping_pong].vk_image,
	        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        {
	            .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
	            .baseMipLevel   = 0,
	            .levelCount     = m_mip_level,
	            .baseArrayLayer = 0,
	            .layerCount     = 1,
	        })
	    .insert()
	    .end_marker()
	    .end_marker();
}
