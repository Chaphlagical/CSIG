#include "pipeline/tonemap.hpp"

#include <imgui.h>

#define NUM_THREADS_X 8
#define NUM_THREADS_Y 8

Tonemap::Tonemap(const Context &context) :
    m_context(&context)
{
	render_target      = m_context->create_texture_2d("Tonemap Image", m_context->render_extent.width, m_context->render_extent.height, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	render_target_view = m_context->create_texture_view("Tonemap Image View", render_target.vk_image, VK_FORMAT_R32G32B32A32_SFLOAT);

	m_descriptor.input_layout = m_context->create_descriptor_layout()
	                                .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                                .create();
	m_descriptor.output_layout = m_context->create_descriptor_layout()
	                                 .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                                 .create();
	m_descriptor.output_set = m_context->allocate_descriptor_set(m_descriptor.output_layout);
	m_pipeline_layout       = m_context->create_pipeline_layout({m_descriptor.input_layout, m_descriptor.output_layout}, sizeof(m_push_constant), VK_SHADER_STAGE_COMPUTE_BIT);
	m_pipeline              = m_context->create_compute_pipeline("tonemap.slang", m_pipeline_layout);

	m_context->update_descriptor()
	    .write_storage_images(0, {render_target_view})
	    .update(m_descriptor.output_set);

	m_context->record_command()
	    .begin()
	    .insert_barrier()
	    .add_image_barrier(render_target.vk_image, 
			0, VK_ACCESS_SHADER_READ_BIT, 
			VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .insert()
	    .end()
	    .flush();
}

Tonemap::~Tonemap()
{
	m_context->destroy(render_target)
	    .destroy(render_target_view)
	    .destroy(m_pipeline_layout)
	    .destroy(m_pipeline)
	    .destroy(m_descriptor.input_layout)
	    .destroy(m_descriptor.output_layout)
	    .destroy(m_descriptor.output_set);
}

void Tonemap::draw(CommandBufferRecorder &recorder, const PathTracing &path_tracing)
{
	recorder
	    .begin_marker("Tonemapping")
	    .insert_barrier()
	    .add_image_barrier(render_target.vk_image,
	                       VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
	    .insert()
	    .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline)
	    .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline_layout, {path_tracing.descriptor.sets[m_context->ping_pong], m_descriptor.output_set})
	    .push_constants(m_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, m_push_constant)
	    .dispatch({m_context->render_extent.width, m_context->render_extent.height, 1}, {NUM_THREADS_X, NUM_THREADS_Y, 1})
	    .insert_barrier()
	    .add_image_barrier(render_target.vk_image,
	                       VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	                       VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		.insert()
	    .end_marker();
}

void Tonemap::draw(CommandBufferRecorder &recorder, const DeferredPass &deferred)
{
	recorder
	    .begin_marker("Tonemapping")
	    .insert_barrier()
	    .add_image_barrier(render_target.vk_image,
	                       VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
	    .insert()
	    .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline)
	    .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline_layout, {deferred.descriptor.set, m_descriptor.output_set})
	    .push_constants(m_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, m_push_constant)
	    .dispatch({m_context->render_extent.width, m_context->render_extent.height, 1}, {NUM_THREADS_X, NUM_THREADS_Y, 1})
	    .insert_barrier()
	    .add_image_barrier(render_target.vk_image,
	                       VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	                       VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .insert()
	    .end_marker();
}

bool Tonemap::draw_ui()
{
	bool update = false;
	if (ImGui::TreeNode("Tonemapping"))
	{
		update |= ImGui::SliderFloat("Exposure", &m_push_constant.avg_lum, 0.001f, 5.0f);
		update |= ImGui::SliderFloat("Brightness", &m_push_constant.brightness, 0.0f, 2.0f);
		update |= ImGui::SliderFloat("Contrast", &m_push_constant.contrast, 0.0f, 2.0f);
		update |= ImGui::SliderFloat("Saturation", &m_push_constant.saturation, 0.0f, 5.0f);
		update |= ImGui::SliderFloat("Vignette", &m_push_constant.vignette, 0.0f, 2.0f);
		ImGui::TreePop();
	}
	return update;
}
