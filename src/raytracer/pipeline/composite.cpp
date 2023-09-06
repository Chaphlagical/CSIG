#include "pipeline/composite.hpp"

CompositePass::CompositePass(const Context &context, const Scene &scene, const GBufferPass &gbuffer, const RayTracedAO &ao, const RayTracedDI &di, const RayTracedReflection &reflection) :
    m_context(&context)
{
	composite_image = m_context->create_texture_2d(
	    "Composite Image",
	    m_context->extent.width, m_context->extent.height,
	    VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	composite_view = m_context->create_texture_view("Composite View", composite_image.vk_image, VK_FORMAT_R8G8B8A8_UNORM);

	m_descriptor_layout = m_context->create_descriptor_layout()
	                          .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                          .create();
	m_descriptor_set = m_context->allocate_descriptor_set(m_descriptor_layout);
	m_context->update_descriptor()
	    .write_storage_images(0, {composite_view})
	    .update(m_descriptor_set);

	m_gbuffer.pipeline_layout    = m_context->create_pipeline_layout({scene.descriptor.layout, gbuffer.descriptor.layout, m_descriptor_layout});
	m_gbuffer.albedo_pipeline    = m_context->create_compute_pipeline("composite.slang", m_gbuffer.pipeline_layout, "main", {{"VISUALIZE_GBUFFER", "1"}, {"VISUALIZE_ALBEDO", "1"}});
	m_gbuffer.normal_pipeline    = m_context->create_compute_pipeline("composite.slang", m_gbuffer.pipeline_layout, "main", {{"VISUALIZE_GBUFFER", "1"}, {"VISUALIZE_NORMAL", "1"}});
	m_gbuffer.metallic_pipeline  = m_context->create_compute_pipeline("composite.slang", m_gbuffer.pipeline_layout, "main", {{"VISUALIZE_GBUFFER", "1"}, {"VISUALIZE_METALLIC", "1"}});
	m_gbuffer.roughness_pipeline = m_context->create_compute_pipeline("composite.slang", m_gbuffer.pipeline_layout, "main", {{"VISUALIZE_GBUFFER", "1"}, {"VISUALIZE_ROUGHNESS", "1"}});
	m_gbuffer.position_pipeline  = m_context->create_compute_pipeline("composite.slang", m_gbuffer.pipeline_layout, "main", {{"VISUALIZE_GBUFFER", "1"}, {"VISUALIZE_POSITION", "1"}});

	m_ao.pipeline_layout = m_context->create_pipeline_layout({scene.descriptor.layout, ao.descriptor.layout, m_descriptor_layout});
	m_ao.pipeline        = m_context->create_compute_pipeline("composite.slang", m_ao.pipeline_layout, "main", {{"VISUALIZE_AO", "1"}});

	m_reflection.pipeline_layout = m_context->create_pipeline_layout({scene.descriptor.layout, reflection.descriptor.layout, m_descriptor_layout});
	m_reflection.pipeline        = m_context->create_compute_pipeline("composite.slang", m_ao.pipeline_layout, "main", {{"VISUALIZE_REFLECTION", "1"}});

	m_di.pipeline_layout = m_context->create_pipeline_layout({scene.descriptor.layout, di.descriptor.layout, m_descriptor_layout});
	m_di.pipeline        = m_context->create_compute_pipeline("composite.slang", m_ao.pipeline_layout, "main", {{"VISUALIZE_DI", "1"}});

	init();
}

CompositePass::~CompositePass()
{
	m_context->destroy(m_descriptor_layout)
	    .destroy(m_descriptor_set)
	    .destroy(composite_image)
	    .destroy(composite_view)
	    .destroy(m_gbuffer.pipeline_layout)
	    .destroy(m_gbuffer.albedo_pipeline)
	    .destroy(m_gbuffer.normal_pipeline)
	    .destroy(m_gbuffer.metallic_pipeline)
	    .destroy(m_gbuffer.roughness_pipeline)
	    .destroy(m_gbuffer.position_pipeline)
	    .destroy(m_ao.pipeline_layout)
	    .destroy(m_ao.pipeline)
	    .destroy(m_di.pipeline_layout)
	    .destroy(m_di.pipeline)
	    .destroy(m_reflection.pipeline_layout)
	    .destroy(m_reflection.pipeline);
}

void CompositePass::init()
{
	m_context->record_command()
	    .begin()
	    .insert_barrier()
	    .add_image_barrier(composite_image.vk_image, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)
	    .insert()
	    .end()
	    .flush();
}

void CompositePass::draw(CommandBufferRecorder &recorder, const Scene &scene, const GBufferPass &gbuffer, GBufferOption option)
{
	VkPipeline pipelines[] = {
	    m_gbuffer.albedo_pipeline,
	    m_gbuffer.normal_pipeline,
	    m_gbuffer.metallic_pipeline,
	    m_gbuffer.roughness_pipeline,
	    m_gbuffer.position_pipeline,
	};

	recorder
	    .begin_marker("Composite")
	    .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[(size_t) option])
	    .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_gbuffer.pipeline_layout, {scene.descriptor.set, gbuffer.descriptor.sets[m_context->ping_pong], m_descriptor_set})
	    .dispatch({m_context->extent.width, m_context->extent.height, 1}, {8, 8, 1})
	    .execute([&](CommandBufferRecorder &recorder) { blit(recorder); })
	    .end_marker();
}

void CompositePass::draw(CommandBufferRecorder &recorder, const Scene &scene, const Tonemap &tonemap)
{
	recorder
	    .begin_marker("Composite")
	    .insert_barrier()
	    .add_image_barrier(tonemap.render_target.vk_image,
	                       VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
	                       VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
	    .add_image_barrier(m_context->swapchain_images[m_context->image_index],
	                       VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
	                       VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
	    .insert();
	m_context->blit_back_buffer(recorder.cmd_buffer, tonemap.render_target.vk_image);
	recorder.insert_barrier()
	    .add_image_barrier(tonemap.render_target.vk_image,
	                       VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(m_context->swapchain_images[m_context->image_index],
	                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
	                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
	    .insert()
	    .end_marker();
}

void CompositePass::draw(CommandBufferRecorder &recorder, const Scene &scene, const RayTracedAO &ao)
{
	recorder
	    .begin_marker("Composite")
	    .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_ao.pipeline)
	    .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_ao.pipeline_layout, {scene.descriptor.set, ao.descriptor.set, m_descriptor_set})
	    .dispatch({m_context->extent.width, m_context->extent.height, 1}, {8, 8, 1})
	    .execute([&](CommandBufferRecorder &recorder) { blit(recorder); })
	    .end_marker();
}

void CompositePass::draw(CommandBufferRecorder &recorder, const Scene &scene, const RayTracedDI &di)
{
	recorder
	    .begin_marker("Composite")
	    .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_di.pipeline)
	    .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_di.pipeline_layout, {scene.descriptor.set, di.descriptor.set, m_descriptor_set})
	    .dispatch({m_context->extent.width, m_context->extent.height, 1}, {8, 8, 1})
	    .execute([&](CommandBufferRecorder &recorder) { blit(recorder); })
	    .end_marker();
}

void CompositePass::draw(CommandBufferRecorder &recorder, const Scene &scene, const RayTracedReflection &reflection)
{
	recorder
	    .begin_marker("Composite")
	    .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_reflection.pipeline)
	    .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_reflection.pipeline_layout, {scene.descriptor.set, reflection.descriptor.set, m_descriptor_set})
	    .dispatch({m_context->extent.width, m_context->extent.height, 1}, {8, 8, 1})
	    .execute([&](CommandBufferRecorder &recorder) { blit(recorder); })
	    .end_marker();
}

void CompositePass::blit(CommandBufferRecorder &recorder)
{
	recorder.insert_barrier()
	    .add_image_barrier(composite_image.vk_image,
	                       VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
	                       VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
	    .add_image_barrier(m_context->swapchain_images[m_context->image_index],
	                       VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
	                       VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
	    .insert();
	m_context->blit_back_buffer(recorder.cmd_buffer, composite_image.vk_image);
	recorder.insert_barrier()
	    .add_image_barrier(composite_image.vk_image,
	                       VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(m_context->swapchain_images[m_context->image_index],
	                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
	                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
	    .insert();
}
