#include "pipeline/composite.hpp"

#include <imgui.h>

CompositePass::CompositePass(const Context &context, const Scene &scene, const GBufferPass &gbuffer, const RayTracedAO &ao, const RayTracedDI &di, const RayTracedGI &gi, const RayTracedReflection &reflection) :
    m_context(&context)
{
	m_descriptor_layout = m_context->create_descriptor_layout()
	                          // Output Image
	                          .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                          // Sampler
	                          .add_descriptor_binding(1, VK_DESCRIPTOR_TYPE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT)
	                          .create();
	m_descriptor_set = m_context->allocate_descriptor_set(m_descriptor_layout);

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

	m_gi.pipeline_layout = m_context->create_pipeline_layout({scene.descriptor.layout, gi.descriptor.layout, m_descriptor_layout});
	m_gi.pipeline        = m_context->create_compute_pipeline("composite.slang", m_gi.pipeline_layout, "main", {{"VISUALIZE_GI", "1"}});

	create_resource();
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
	    .destroy(m_gi.pipeline_layout)
	    .destroy(m_gi.pipeline)
	    .destroy(m_reflection.pipeline_layout)
	    .destroy(m_reflection.pipeline);
}

void CompositePass::init()
{
	m_context->record_command()
	    .begin()
	    .insert_barrier()
	    .add_image_barrier(
	        composite_image.vk_image,
	        0, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)
	    .insert()
	    .end()
	    .flush();
}

void CompositePass::resize()
{
	m_context->wait();
	destroy_resource();
	create_resource();
}

void CompositePass::draw(CommandBufferRecorder &recorder, const Scene &scene, const GBufferPass &gbuffer, const RayTracedAO &ao, const RayTracedDI &di, const RayTracedGI &gi, const RayTracedReflection &reflection, const FSR1Pass &fsr)
{
	recorder.begin_marker("Composite");
	switch (option)
	{
		case Option::Result: {
			recorder
			    .insert_barrier()
			    .add_image_barrier(fsr.upsampled_image.vk_image,
			                       VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
			                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
			    .add_image_barrier(m_context->swapchain_images[m_context->image_index],
			                       VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
			                       VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
			    .insert();
			m_context->blit_back_buffer(recorder.cmd_buffer, fsr.upsampled_image.vk_image);
			recorder.insert_barrier()
			    .add_image_barrier(fsr.upsampled_image.vk_image,
			                       VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
			                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
			    .add_image_barrier(m_context->swapchain_images[m_context->image_index],
			                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
			                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
			    .insert();
		}
		break;
		case Option::Normal:
		case Option::Albedo:
		case Option::Roughness:
		case Option::Metallic:
		case Option::Position: {
			VkPipeline pipelines[] = {
			    m_gbuffer.albedo_pipeline,
			    m_gbuffer.normal_pipeline,
			    m_gbuffer.metallic_pipeline,
			    m_gbuffer.roughness_pipeline,
			    m_gbuffer.position_pipeline,
			};
			recorder
			    .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[(size_t) option - 1])
			    .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_gbuffer.pipeline_layout, {scene.descriptor.set, gbuffer.descriptor.sets[m_context->ping_pong], m_descriptor_set})
			    .dispatch({m_context->extent.width, m_context->extent.height, 1}, {8, 8, 1})
			    .execute([&](CommandBufferRecorder &recorder) { blit(recorder); });
		}
		break;
		case Option::AO: {
			recorder
			    .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_ao.pipeline)
			    .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_ao.pipeline_layout, {scene.descriptor.set, ao.descriptor.set, m_descriptor_set})
			    .dispatch({m_context->extent.width, m_context->extent.height, 1}, {8, 8, 1})
			    .execute([&](CommandBufferRecorder &recorder) { blit(recorder); });
		}
		break;
		case Option::Reflection: {
			recorder
			    .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_reflection.pipeline)
			    .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_reflection.pipeline_layout, {scene.descriptor.set, reflection.descriptor.set, m_descriptor_set})
			    .dispatch({m_context->extent.width, m_context->extent.height, 1}, {8, 8, 1})
			    .execute([&](CommandBufferRecorder &recorder) { blit(recorder); });
		}
		break;
		case Option::DI: {
			recorder
			    .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_di.pipeline)
			    .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_di.pipeline_layout, {scene.descriptor.set, di.descriptor.set, m_descriptor_set})
			    .dispatch({m_context->extent.width, m_context->extent.height, 1}, {8, 8, 1})
			    .execute([&](CommandBufferRecorder &recorder) { blit(recorder); });
		}
		break;
		case Option::GI: {
			recorder
			    .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_gi.pipeline)
			    .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_gi.pipeline_layout, {scene.descriptor.set, gi.descriptor.set, m_descriptor_set})
			    .dispatch({m_context->extent.width, m_context->extent.height, 1}, {8, 8, 1})
			    .execute([&](CommandBufferRecorder &recorder) { blit(recorder); });
		}
		break;
		default:
			break;
	}
	recorder.end_marker();
}

bool CompositePass::draw_ui()
{
	bool update = false;

	const char *const debug_views[] = {
	    "Result",
	    "Albedo",
	    "Normal",
	    "Metallic",
	    "Roughness",
	    "Position",
	    "AO",
	    "Reflection",
	    "DI",
	    "GI",
	};
	if (ImGui::TreeNode("Composite"))
	{
		update |= ImGui::Combo("Debug View", reinterpret_cast<int32_t *>(&option), debug_views, 10);
		ImGui::TreePop();
	}
	return update;
}

void CompositePass::create_resource()
{
	composite_image = m_context->create_texture_2d(
	    "Composite Image",
	    m_context->extent.width, m_context->extent.height,
	    VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	composite_view = m_context->create_texture_view("Composite View", composite_image.vk_image, VK_FORMAT_R8G8B8A8_UNORM);

	update_descriptor();
	init();
}

void CompositePass::update_descriptor()
{
	m_context->update_descriptor()
	    .write_storage_images(0, {composite_view})
	    .update(m_descriptor_set);
}

void CompositePass::destroy_resource()
{
	m_context->destroy(composite_image)
	    .destroy(composite_view);
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
