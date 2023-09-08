#include "pipeline/deferred.hpp"

#include <imgui.h>

DeferredPass::DeferredPass(const Context &context, const Scene &scene, const GBufferPass &gbuffer, const RayTracedAO &ao, const RayTracedDI &di, const RayTracedGI &gi, const RayTracedReflection &reflection) :
    m_context(&context)
{
	deferred_image = m_context->create_texture_2d(
	    "Deferred Image",
	    m_context->extent.width, m_context->extent.height,
	    VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	deferred_view = m_context->create_texture_view("Deferred View", deferred_image.vk_image, VK_FORMAT_R16G16B16A16_SFLOAT);

	m_descriptor_layout = m_context->create_descriptor_layout()
	                          .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                          .create();
	m_descriptor_set = m_context->allocate_descriptor_set(m_descriptor_layout);
	m_context->update_descriptor()
	    .write_storage_images(0, {deferred_view})
	    .update(m_descriptor_set);

	m_pipeline_layout = m_context->create_pipeline_layout(
	    {
	        scene.descriptor.layout,
	        gbuffer.descriptor.layout,
	        ao.descriptor.layout,
	        di.descriptor.layout,
	        gi.descriptor.layout,
	        reflection.descriptor.layout,
	        m_descriptor_layout,
	    },
	    sizeof(m_push_constant), VK_SHADER_STAGE_COMPUTE_BIT);
	m_pipeline = m_context->create_compute_pipeline("deferred.slang", m_pipeline_layout);

	descriptor.layout = m_context->create_descriptor_layout()
	                        .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                        .create();
	descriptor.set = m_context->allocate_descriptor_set(descriptor.layout);
	m_context->update_descriptor()
	    .write_sampled_images(0, {deferred_view})
	    .update(descriptor.set);

	init();
}

DeferredPass::~DeferredPass()
{
	m_context->destroy(deferred_image)
	    .destroy(deferred_view)
	    .destroy(descriptor.layout)
	    .destroy(descriptor.set)
	    .destroy(m_descriptor_layout)
	    .destroy(m_descriptor_set)
	    .destroy(m_pipeline_layout)
	    .destroy(m_pipeline);
}

void DeferredPass::init()
{
	m_context->record_command()
	    .begin()
	    .insert_barrier()
	    .add_image_barrier(
	        deferred_image.vk_image,
	        0, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .insert()
	    .end()
	    .flush();
}

void DeferredPass::draw(CommandBufferRecorder &recorder, const Scene &scene, const GBufferPass &gbuffer, const RayTracedAO &ao, const RayTracedDI &di, const RayTracedGI &gi, const RayTracedReflection &reflection)
{
	recorder
	    .begin_marker("Deferred")
	    .insert_barrier()
	    .add_image_barrier(
	        deferred_image.vk_image,
	        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
	    .insert()
	    .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline)
	    .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline_layout,
	                         {scene.descriptor.set,
	                          gbuffer.descriptor.sets[m_context->ping_pong],
	                          ao.descriptor.set,
	                          di.descriptor.set,
	                          gi.descriptor.set,
	                          reflection.descriptor.set,
	                          m_descriptor_set})
	    .push_constants(m_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, m_push_constant)
	    .dispatch({m_context->extent.width, m_context->extent.height, 1}, {8, 8, 1})
	    .insert_barrier()
	    .add_image_barrier(
	        deferred_image.vk_image,
	        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .insert()
	    .end_marker();
}

bool DeferredPass::draw_ui()
{
	bool update = false;
	update |= ImGui::Checkbox("Enable AO", reinterpret_cast<bool *>(&m_push_constant.enable_ao));
	update |= ImGui::Checkbox("Enable Reflection", reinterpret_cast<bool *>(&m_push_constant.enable_reflection));
	update |= ImGui::Checkbox("Enable GI", reinterpret_cast<bool *>(&m_push_constant.enable_gi));
	return update;
}
