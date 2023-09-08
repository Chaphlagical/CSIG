#include "pipeline/taa.hpp"

#include <spdlog/fmt/fmt.h>

#include <GLFW/glfw3.h>

#include <imgui.h>

#define NUM_THREADS_X 8
#define NUM_THREADS_Y 8

static unsigned char g_taa_comp_spv_data[] = {
#include "taa.comp.spv.h"
};

TAA::TAA(const Context &context, const Scene &scene, const GBufferPass &gbuffer_pass, const DeferredPass &deferred) :
    m_context(&context)
{
	for (uint32_t i = 0; i < 2; i++)
	{
		output_image[i] = m_context->create_texture_2d(
		    fmt::format("TAA Image - {}", i),
		    m_context->render_extent.width, m_context->render_extent.height,
		    VK_FORMAT_R16G16B16A16_SFLOAT,
		    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
		output_view[i] = m_context->create_texture_view(
		    fmt::format("TAA View - {}", i),
		    output_image[i].vk_image,
		    VK_FORMAT_R16G16B16A16_SFLOAT);
	}

	m_descriptor_set_layout = m_context->create_descriptor_layout()
	                              // Output Image
	                              .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                              // Prev Image
	                              .add_descriptor_binding(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                              .create();
	m_descriptor_sets = m_context->allocate_descriptor_sets<2>(m_descriptor_set_layout);
	m_pipeline_layout = m_context->create_pipeline_layout(
	    {
	        scene.descriptor.layout,
	        gbuffer_pass.descriptor.layout,
	        deferred.descriptor.layout,
	        m_descriptor_set_layout,
	    },
	    sizeof(m_push_constants), VK_SHADER_STAGE_COMPUTE_BIT);
	m_pipeline = m_context->create_compute_pipeline("taa.slang", m_pipeline_layout);

	descriptor.layout = m_context->create_descriptor_layout()
	                        .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                        .create();
	descriptor.sets = m_context->allocate_descriptor_sets<2>(descriptor.layout);

	for (uint32_t i = 0; i < 2; i++)
	{
		m_context->update_descriptor()
		    .write_storage_images(0, {output_view[i]})
		    .write_sampled_images(1, {output_view[!i]})
		    .update(m_descriptor_sets[i]);
	}

	init();
}

TAA::~TAA()
{
	m_context
	    ->destroy(output_image)
	    .destroy(output_view)
	    .destroy(m_descriptor_sets)
	    .destroy(m_descriptor_set_layout)
	    .destroy(m_pipeline_layout)
	    .destroy(m_pipeline);
}

void TAA::init()
{
	m_context->record_command()
	    .begin()
	    .insert_barrier()
	    .add_image_barrier(
	        output_image[0].vk_image,
	        0, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        output_image[1].vk_image,
	        0, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .insert()
	    .end()
	    .flush();
}

void TAA::draw(CommandBufferRecorder &recorder, const Scene &scene, const GBufferPass &gbuffer_pass, const DeferredPass &deferred)
{
	m_push_constants.time_params = glm::vec4(static_cast<float>(glfwGetTime()), sinf(static_cast<float>(glfwGetTime())), cosf(static_cast<float>(glfwGetTime())), m_delta_time);
	m_push_constants.texel_size  = glm::vec4(1.0f / float(m_context->render_extent.width), 1.0f / float(m_context->render_extent.height), float(m_context->render_extent.width), float(m_context->render_extent.height));

	recorder
	    .begin_marker("TAA")
	    .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline_layout,
	                         {
	                             scene.descriptor.set,
	                             gbuffer_pass.descriptor.sets[m_context->ping_pong],
	                             deferred.descriptor.set,
	                             m_descriptor_sets[m_context->ping_pong],
	                         })
	    .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline)
	    .push_constants(m_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, m_push_constants)
	    .dispatch({m_context->render_extent.width, m_context->render_extent.height, 1}, {NUM_THREADS_X, NUM_THREADS_Y, 1})

	    .insert_barrier()
	    .add_image_barrier(
	        output_image[m_context->ping_pong].vk_image,
	        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .add_image_barrier(
	        output_image[!m_context->ping_pong].vk_image,
	        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
	    .insert()
	    .end_marker();
}

bool TAA::draw_ui()
{
	m_delta_time = ImGui::GetIO().DeltaTime;
	return false;
}
