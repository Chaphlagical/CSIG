#include "pipeline/gbuffer.hpp"

#include <spdlog/fmt/fmt.h>

static unsigned char g_gbuffer_vert_spv_data[] = {
#include "gbuffer.vert.spv.h"
};

static unsigned char g_gbuffer_frag_spv_data[] = {
#include "gbuffer.frag.spv.h"
};

GBufferPass::GBufferPass(const Context &context, const Scene &scene) :
    m_context(&context),
    m_width(context.render_extent.width),
    m_height(context.render_extent.height),
    m_mip_level(std::min(static_cast<uint32_t>(std::floor(std::log2(std::max(m_width, m_height))) + 1), 4u))
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
	                 .add_shader(VK_SHADER_STAGE_VERTEX_BIT, (uint32_t *) g_gbuffer_vert_spv_data, sizeof(g_gbuffer_vert_spv_data))
	                 .add_shader(VK_SHADER_STAGE_FRAGMENT_BIT, (uint32_t *) g_gbuffer_frag_spv_data, sizeof(g_gbuffer_frag_spv_data))
	                 .add_vertex_input_attribute(0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0)
	                 .add_vertex_input_attribute(1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(glm::vec4))
	                 .add_vertex_input_binding(0, 2 * sizeof(glm::vec4))
	                 .create();

	descriptor.layout = m_context->create_descriptor_layout()
	                        .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                        .add_descriptor_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                        .add_descriptor_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                        .add_descriptor_binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                        .add_descriptor_binding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                        .add_descriptor_binding(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                        .add_descriptor_binding(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                        .add_descriptor_binding(7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                        .create();
	descriptor.sets = m_context->allocate_descriptor_sets<2>(descriptor.layout);

	for (uint32_t i = 0; i < 2; i++)
	{
		m_context->update_descriptor()
		    .write_combine_sampled_images(0, scene.linear_sampler, {gbufferA_view[i]})
		    .write_combine_sampled_images(1, scene.nearest_sampler, {gbufferB_view[i]})
		    .write_combine_sampled_images(2, scene.linear_sampler, {gbufferC_view[i]})
		    .write_combine_sampled_images(3, scene.linear_sampler, {depth_buffer_view[i]})
		    .write_combine_sampled_images(4, scene.linear_sampler, {gbufferA_view[!i]})
		    .write_combine_sampled_images(5, scene.nearest_sampler, {gbufferB_view[!i]})
		    .write_combine_sampled_images(6, scene.linear_sampler, {gbufferC_view[!i]})
		    .write_combine_sampled_images(7, scene.linear_sampler, {depth_buffer_view[!i]})
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
	    //.insert_barrier()
	    //.add_image_barrier(
	    //    gbufferA[m_context->ping_pong].vk_image,
	    //    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
	    //    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	    //    {
	    //        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	    //        .baseMipLevel   = 0,
	    //        .levelCount     = m_mip_level,
	    //        .baseArrayLayer = 0,
	    //        .layerCount     = 1,
	    //    })
	    //.add_image_barrier(
	    //    gbufferB[m_context->ping_pong].vk_image,
	    //    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
	    //    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	    //    {
	    //        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	    //        .baseMipLevel   = 0,
	    //        .levelCount     = m_mip_level,
	    //        .baseArrayLayer = 0,
	    //        .layerCount     = 1,
	    //    })
	    //.add_image_barrier(
	    //    gbufferC[m_context->ping_pong].vk_image,
	    //    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
	    //    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	    //    {
	    //        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	    //        .baseMipLevel   = 0,
	    //        .levelCount     = m_mip_level,
	    //        .baseArrayLayer = 0,
	    //        .layerCount     = 1,
	    //    })
	    //.add_image_barrier(
	    //    depth_buffer[m_context->ping_pong].vk_image,
	    //    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
	    //    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	    //    {
	    //        .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
	    //        .baseMipLevel   = 0,
	    //        .levelCount     = m_mip_level,
	    //        .baseArrayLayer = 0,
	    //        .layerCount     = 1,
	    //    })
	    //.insert()
	    .execute([&](CommandBufferRecorder &recorder) {
		    {
			    VkImageBlit blit_info = {
			        .srcSubresource = VkImageSubresourceLayers{
			            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
			            .mipLevel       = 0,
			            .baseArrayLayer = 0,
			            .layerCount     = 1,
			        },
			        .srcOffsets = {
			            VkOffset3D{
			                .x = 0,
			                .y = 0,
			                .z = 0,
			            },
			            VkOffset3D{
			                .x = static_cast<int32_t>(m_width),
			                .y = static_cast<int32_t>(m_height),
			                .z = 1,
			            },
			        },
			        .dstSubresource = VkImageSubresourceLayers{
			            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
			            .mipLevel       = 0,
			            .baseArrayLayer = 0,
			            .layerCount     = 1,
			        },
			        .dstOffsets = {
			            VkOffset3D{
			                .x = 0,
			                .y = 0,
			                .z = 0,
			            },
			            VkOffset3D{
			                .x = static_cast<int32_t>(m_width),
			                .y = static_cast<int32_t>(m_height),
			                .z = 1,
			            },
			        },
			    };
			    VkImageMemoryBarrier image_barriers[] = {
			        {
			            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			            .srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			            .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
			            .oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			            .image               = gbufferA[m_context->ping_pong].vk_image,
			            .subresourceRange    = VkImageSubresourceRange{
			                   .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
			                   .baseMipLevel   = 0,
			                   .levelCount     = m_mip_level,
			                   .baseArrayLayer = 0,
			                   .layerCount     = 1,
                        },
			        },
			        {
			            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			            .srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			            .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
			            .oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			            .image               = gbufferB[m_context->ping_pong].vk_image,
			            .subresourceRange    = VkImageSubresourceRange{
			                   .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
			                   .baseMipLevel   = 0,
			                   .levelCount     = m_mip_level,
			                   .baseArrayLayer = 0,
			                   .layerCount     = 1,
                        },
			        },
			        {
			            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			            .srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			            .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
			            .oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			            .image               = gbufferC[m_context->ping_pong].vk_image,
			            .subresourceRange    = VkImageSubresourceRange{
			                   .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
			                   .baseMipLevel   = 0,
			                   .levelCount     = m_mip_level,
			                   .baseArrayLayer = 0,
			                   .layerCount     = 1,
                        },
			        },
			        {
			            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			            .srcAccessMask       = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
			            .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
			            .oldLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
			            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			            .image               = depth_buffer[m_context->ping_pong].vk_image,
			            .subresourceRange    = VkImageSubresourceRange{
			                   .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
			                   .baseMipLevel   = 0,
			                   .levelCount     = m_mip_level,
			                   .baseArrayLayer = 0,
			                   .layerCount     = 1,
                        },
			        },
			    };

			    vkCmdPipelineBarrier(
			        recorder.cmd_buffer,
			        VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
			        VK_PIPELINE_STAGE_TRANSFER_BIT,
			        0, 0, nullptr, 0, nullptr, 4, image_barriers);
		    }

		    for (uint32_t i = 1; i < m_mip_level; i++)
		    {
			    VkImageBlit blit_info = {
			        .srcSubresource = VkImageSubresourceLayers{
			            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
			            .mipLevel       = i - 1,
			            .baseArrayLayer = 0,
			            .layerCount     = 1,
			        },
			        .srcOffsets = {
			            VkOffset3D{
			                .x = 0,
			                .y = 0,
			                .z = 0,
			            },
			            VkOffset3D{
			                .x = static_cast<int32_t>(m_width >> (i - 1)),
			                .y = static_cast<int32_t>(m_height >> (i - 1)),
			                .z = 1,
			            },
			        },
			        .dstSubresource = VkImageSubresourceLayers{
			            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
			            .mipLevel       = i,
			            .baseArrayLayer = 0,
			            .layerCount     = 1,
			        },
			        .dstOffsets = {
			            VkOffset3D{
			                .x = 0,
			                .y = 0,
			                .z = 0,
			            },
			            VkOffset3D{
			                .x = static_cast<int32_t>(m_width >> i),
			                .y = static_cast<int32_t>(m_height >> i),
			                .z = 1,
			            },
			        },
			    };

			    {
				    VkImageMemoryBarrier image_barriers[] = {
				        {
				            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				            .srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
				            .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
				            .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				            .image               = gbufferA[m_context->ping_pong].vk_image,
				            .subresourceRange    = VkImageSubresourceRange{
				                   .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
				                   .baseMipLevel   = i,
				                   .levelCount     = 1,
				                   .baseArrayLayer = 0,
				                   .layerCount     = 1,
                            },
				        },
				        {
				            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				            .srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
				            .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
				            .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				            .image               = gbufferB[m_context->ping_pong].vk_image,
				            .subresourceRange    = VkImageSubresourceRange{
				                   .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
				                   .baseMipLevel   = i,
				                   .levelCount     = 1,
				                   .baseArrayLayer = 0,
				                   .layerCount     = 1,
                            },
				        },
				        {
				            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				            .srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
				            .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
				            .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				            .image               = gbufferC[m_context->ping_pong].vk_image,
				            .subresourceRange    = VkImageSubresourceRange{
				                   .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
				                   .baseMipLevel   = i,
				                   .levelCount     = 1,
				                   .baseArrayLayer = 0,
				                   .layerCount     = 1,
                            },
				        },
				        {
				            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				            .srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
				            .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
				            .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				            .image               = depth_buffer[m_context->ping_pong].vk_image,
				            .subresourceRange    = VkImageSubresourceRange{
				                   .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
				                   .baseMipLevel   = i,
				                   .levelCount     = 1,
				                   .baseArrayLayer = 0,
				                   .layerCount     = 1,
                            },
				        },
				    };
				    vkCmdPipelineBarrier(
				        recorder.cmd_buffer,
				        VK_PIPELINE_STAGE_TRANSFER_BIT,
				        VK_PIPELINE_STAGE_TRANSFER_BIT,
				        0, 0, nullptr, 0, nullptr, 4, image_barriers);
			    }

			    vkCmdBlitImage(
			        recorder.cmd_buffer,
			        gbufferA[m_context->ping_pong].vk_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			        gbufferA[m_context->ping_pong].vk_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			        1, &blit_info, VK_FILTER_NEAREST);
			    vkCmdBlitImage(
			        recorder.cmd_buffer,
			        gbufferB[m_context->ping_pong].vk_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			        gbufferB[m_context->ping_pong].vk_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			        1, &blit_info, VK_FILTER_NEAREST);
			    vkCmdBlitImage(
			        recorder.cmd_buffer,
			        gbufferC[m_context->ping_pong].vk_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			        gbufferC[m_context->ping_pong].vk_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			        1, &blit_info, VK_FILTER_NEAREST);
			    blit_info.srcSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
			    blit_info.dstSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
			    vkCmdBlitImage(
			        recorder.cmd_buffer,
			        depth_buffer[m_context->ping_pong].vk_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			        depth_buffer[m_context->ping_pong].vk_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			        1, &blit_info, VK_FILTER_NEAREST);

			    {
				    VkImageMemoryBarrier image_barriers[] = {
				        {
				            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				            .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
				            .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
				            .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				            .image               = gbufferA[m_context->ping_pong].vk_image,
				            .subresourceRange    = VkImageSubresourceRange{
				                   .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
				                   .baseMipLevel   = i,
				                   .levelCount     = 1,
				                   .baseArrayLayer = 0,
				                   .layerCount     = 1,
                            },
				        },
				        {
				            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				            .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
				            .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
				            .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				            .image               = gbufferB[m_context->ping_pong].vk_image,
				            .subresourceRange    = VkImageSubresourceRange{
				                   .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
				                   .baseMipLevel   = i,
				                   .levelCount     = 1,
				                   .baseArrayLayer = 0,
				                   .layerCount     = 1,
                            },
				        },
				        {
				            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				            .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
				            .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
				            .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				            .image               = gbufferC[m_context->ping_pong].vk_image,
				            .subresourceRange    = VkImageSubresourceRange{
				                   .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
				                   .baseMipLevel   = i,
				                   .levelCount     = 1,
				                   .baseArrayLayer = 0,
				                   .layerCount     = 1,
                            },
				        },
				        {
				            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				            .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
				            .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
				            .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				            .image               = depth_buffer[m_context->ping_pong].vk_image,
				            .subresourceRange    = VkImageSubresourceRange{
				                   .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
				                   .baseMipLevel   = i,
				                   .levelCount     = 1,
				                   .baseArrayLayer = 0,
				                   .layerCount     = 1,
                            },
				        },
				    };
				    vkCmdPipelineBarrier(
				        recorder.cmd_buffer,
				        VK_PIPELINE_STAGE_TRANSFER_BIT,
				        VK_PIPELINE_STAGE_TRANSFER_BIT,
				        0, 0, nullptr, 0, nullptr, 4, image_barriers);
			    }
		    }

		    {
			    VkImageMemoryBarrier image_barriers[] = {
			        {
			            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			            .srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
			            .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
			            .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			            .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			            .image               = gbufferA[m_context->ping_pong].vk_image,
			            .subresourceRange    = VkImageSubresourceRange{
			                   .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
			                   .baseMipLevel   = 0,
			                   .levelCount     = m_mip_level,
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
			            .image               = gbufferB[m_context->ping_pong].vk_image,
			            .subresourceRange    = VkImageSubresourceRange{
			                   .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
			                   .baseMipLevel   = 0,
			                   .levelCount     = m_mip_level,
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
			            .image               = gbufferC[m_context->ping_pong].vk_image,
			            .subresourceRange    = VkImageSubresourceRange{
			                   .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
			                   .baseMipLevel   = 0,
			                   .levelCount     = m_mip_level,
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
			            .image               = depth_buffer[m_context->ping_pong].vk_image,
			            .subresourceRange    = VkImageSubresourceRange{
			                   .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
			                   .baseMipLevel   = 0,
			                   .levelCount     = m_mip_level,
			                   .baseArrayLayer = 0,
			                   .layerCount     = 1,
                        },
			        },
			    };
			    vkCmdPipelineBarrier(
			        recorder.cmd_buffer,
			        VK_PIPELINE_STAGE_TRANSFER_BIT,
			        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			        0, 0, nullptr, 0, nullptr, 4, image_barriers);
		    }
	    })
	    /*.generate_mipmap(gbufferA[m_context->ping_pong].vk_image, m_width, m_height, m_mip_level, 1, VK_IMAGE_ASPECT_COLOR_BIT, VK_FILTER_NEAREST)
	    .generate_mipmap(gbufferB[m_context->ping_pong].vk_image, m_width, m_height, m_mip_level, 1, VK_IMAGE_ASPECT_COLOR_BIT, VK_FILTER_NEAREST)
	    .generate_mipmap(gbufferC[m_context->ping_pong].vk_image, m_width, m_height, m_mip_level, 1, VK_IMAGE_ASPECT_COLOR_BIT, VK_FILTER_NEAREST)
	    .generate_mipmap(depth_buffer[m_context->ping_pong].vk_image, m_width, m_height, m_mip_level, 1, VK_IMAGE_ASPECT_DEPTH_BIT, VK_FILTER_NEAREST)
	    */
	    /*.insert_barrier()
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
	    .insert()*/
	    .end_marker()
	    .end_marker();
}
