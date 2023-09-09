#include "pipeline/path_tracing.hpp"

#include <spdlog/fmt/fmt.h>

#include <imgui.h>

#define RAY_TRACE_NUM_THREADS_X 8
#define RAY_TRACE_NUM_THREADS_Y 8

PathTracing::PathTracing(const Context &context, const Scene &scene, const GBufferPass &gbuffer_pass) :
    m_context(&context)
{
	for (uint32_t i = 0; i < 2; i++)
	{
		render_target[i]      = m_context->create_texture_2d(fmt::format("Path Tracing Image - {}", i), m_context->render_extent.width, m_context->render_extent.height, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
		render_target_view[i] = m_context->create_texture_view(fmt::format("Path Tracing Image View - {}", i), render_target[i].vk_image, VK_FORMAT_R32G32B32A32_SFLOAT);
	}

	m_descriptor_set_layout = m_context->create_descriptor_layout()
	                              .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                              .add_descriptor_binding(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                              .create();
	m_descriptor_sets = m_context->allocate_descriptor_sets<2>(m_descriptor_set_layout);
	m_pipeline_layout = m_context->create_pipeline_layout({scene.descriptor.layout, gbuffer_pass.descriptor.layout, m_descriptor_set_layout}, sizeof(m_push_constant), VK_SHADER_STAGE_COMPUTE_BIT);
	m_pipeline        = m_context->create_compute_pipeline("path_tracing.slang", m_pipeline_layout);

	descriptor.layout = m_context->create_descriptor_layout()
	                        .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                        .create();
	descriptor.sets = m_context->allocate_descriptor_sets<2>(descriptor.layout);

	for (uint32_t i = 0; i < 2; i++)
	{
		m_context->update_descriptor()
		    .write_storage_images(0, {render_target_view[i]})
		    .write_sampled_images(1, {render_target_view[i]})
		    .update(m_descriptor_sets[i]);
		m_context->update_descriptor()
		    .write_sampled_images(0, {render_target_view[i]})
		    .update(descriptor.sets[i]);
	}

	init();
}

PathTracing::~PathTracing()
{
	m_context->destroy(render_target)
	    .destroy(render_target_view)
	    .destroy(m_descriptor_set_layout)
	    .destroy(m_descriptor_sets)
	    .destroy(m_pipeline_layout)
	    .destroy(m_pipeline)
	    .destroy(descriptor.layout)
	    .destroy(descriptor.sets);
}

void PathTracing::init()
{
	m_context->record_command()
	    .begin()
	    .insert_barrier()
	    .add_image_barrier(render_target[0].vk_image,
	                       0, VK_ACCESS_SHADER_WRITE_BIT,
	                       VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(render_target[1].vk_image,
	                       0, VK_ACCESS_SHADER_READ_BIT,
	                       VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .insert()
	    .end()
	    .flush();
}

void PathTracing::draw(CommandBufferRecorder &recorder, const Scene &scene, const GBufferPass &gbuffer_pass)
{
	recorder
	    .begin_marker("Path Tracing")
	    .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline)
	    .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline_layout, {scene.descriptor.set, gbuffer_pass.descriptor.sets[m_context->ping_pong], m_descriptor_sets[m_context->ping_pong]})
	    .push_constants(m_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, m_push_constant)
	    .dispatch({m_context->render_extent.width, m_context->render_extent.height, 1}, {RAY_TRACE_NUM_THREADS_X, RAY_TRACE_NUM_THREADS_Y, 1})
	    .insert_barrier()
	    .add_image_barrier(render_target[m_context->ping_pong].vk_image,
	                       VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	                       VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .add_image_barrier(render_target[!m_context->ping_pong].vk_image,
	                       VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
	    .insert(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
	    .end_marker();
	m_push_constant.frame_count++;
}

bool PathTracing::draw_ui()
{
	bool update = false;
	if (ImGui::TreeNode("Path Tracing"))
	{
		ImGui::Text("Iteration: %d", m_push_constant.frame_count);
		update |= ImGui::SliderInt("Max Depth", reinterpret_cast<int32_t *>(&m_push_constant.max_depth), 1, 100);
		update |= ImGui::DragFloat("Bias", &m_push_constant.bias, 0.00001f, -1.f, 1.f, "%.10f");
	}
	return update;
}

void PathTracing::reset_frames()
{
	m_push_constant.frame_count = 0;
}
