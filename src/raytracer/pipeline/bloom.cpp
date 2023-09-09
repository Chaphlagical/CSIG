#include "pipeline/bloom.hpp"

#include <imgui.h>

#include <spdlog/fmt/fmt.h>

Bloom::Bloom(const Context &context) :
    m_context(&context)
{
	sampler = m_context->create_sampler(VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT);

	descriptor.layout = m_context->create_descriptor_layout()
	                        .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                        .create();
	descriptor.set = m_context->allocate_descriptor_set(descriptor.layout);

	m_mask.descriptor_layout = m_context->create_descriptor_layout()
	                               // Output Image
	                               .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                               .create();
	m_mask.descriptor_set  = m_context->allocate_descriptor_set(m_mask.descriptor_layout);
	m_mask.pipeline_layout = m_context->create_pipeline_layout({descriptor.layout, m_mask.descriptor_layout}, sizeof(m_mask.push_constants), VK_SHADER_STAGE_COMPUTE_BIT);
	m_mask.pipeline        = m_context->create_compute_pipeline("bloom_mask.slang", m_mask.pipeline_layout);

	m_dowsample.descriptor_layout = m_context->create_descriptor_layout()
	                                    // Input Image
	                                    .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                                    // Output Image
	                                    .add_descriptor_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                                    // Sampler
	                                    .add_descriptor_binding(2, VK_DESCRIPTOR_TYPE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT)
	                                    .create();
	m_dowsample.descriptor_sets = m_context->allocate_descriptor_sets<4>(m_dowsample.descriptor_layout);
	m_dowsample.pipeline_layout = m_context->create_pipeline_layout({m_dowsample.descriptor_layout});
	m_dowsample.pipeline        = m_context->create_compute_pipeline("bloom_downsample.slang", m_dowsample.pipeline_layout);

	m_blur.descriptor_layout = m_context->create_descriptor_layout()
	                               // Input Image
	                               .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                               // Output Image
	                               .add_descriptor_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                               .create();
	m_blur.descriptor_sets = m_context->allocate_descriptor_sets<4>(m_blur.descriptor_layout);
	m_blur.pipeline_layout = m_context->create_pipeline_layout({m_blur.descriptor_layout});
	m_blur.pipeline        = m_context->create_compute_pipeline("bloom_blur.slang", m_blur.pipeline_layout);

	m_upsample.descriptor_layout = m_context->create_descriptor_layout()
	                                   // Low Level Image
	                                   .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                                   // High Level Image
	                                   .add_descriptor_binding(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                                   // Output Image
	                                   .add_descriptor_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                                   .create();
	m_upsample.descriptor_sets = m_context->allocate_descriptor_sets<3>(m_upsample.descriptor_layout);
	m_upsample.pipeline_layout = m_context->create_pipeline_layout({m_upsample.descriptor_layout}, sizeof(m_upsample.push_constants), VK_SHADER_STAGE_COMPUTE_BIT);
	m_upsample.pipeline        = m_context->create_compute_pipeline("bloom_upsample.slang", m_upsample.pipeline_layout);

	m_blend.descriptor_layout = m_context->create_descriptor_layout()
	                                // Bloom Image
	                                .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                                // Output Image
	                                .add_descriptor_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                                .create();
	m_blend.descriptor_set  = m_context->allocate_descriptor_set(m_blend.descriptor_layout);
	m_blend.pipeline_layout = m_context->create_pipeline_layout({descriptor.layout, m_blend.descriptor_layout}, sizeof(m_blend.push_constants), VK_SHADER_STAGE_COMPUTE_BIT);
	m_blend.pipeline        = m_context->create_compute_pipeline("bloom_blend.slang", m_blend.pipeline_layout);

	create_resource();
}

Bloom::~Bloom()
{
	destroy_resource();
	m_context->destroy(sampler)
	    .destroy(descriptor.layout)
	    .destroy(descriptor.set)
	    .destroy(m_mask.descriptor_layout)
	    .destroy(m_mask.descriptor_set)
	    .destroy(m_mask.pipeline_layout)
	    .destroy(m_mask.pipeline)
	    .destroy(m_dowsample.descriptor_layout)
	    .destroy(m_dowsample.descriptor_sets)
	    .destroy(m_dowsample.pipeline_layout)
	    .destroy(m_dowsample.pipeline)
	    .destroy(m_blur.descriptor_layout)
	    .destroy(m_blur.descriptor_sets)
	    .destroy(m_blur.pipeline_layout)
	    .destroy(m_blur.pipeline)
	    .destroy(m_upsample.descriptor_layout)
	    .destroy(m_upsample.descriptor_sets)
	    .destroy(m_upsample.pipeline_layout)
	    .destroy(m_upsample.pipeline)
	    .destroy(m_blend.descriptor_layout)
	    .destroy(m_blend.descriptor_set)
	    .destroy(m_blend.pipeline_layout)
	    .destroy(m_blend.pipeline);
}

void Bloom::init()
{
	m_context->record_command()
	    .begin()
	    .insert_barrier()
	    .add_image_barrier(
	        mask_image.vk_image,
	        0, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        output_image.vk_image,
	        0, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .add_image_barrier(
	        level_image[0].vk_image,
	        0, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        level_image[1].vk_image,
	        0, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        level_image[2].vk_image,
	        0, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        level_image[3].vk_image,
	        0, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        blur_image[0].vk_image,
	        0, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        blur_image[1].vk_image,
	        0, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        blur_image[2].vk_image,
	        0, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        blur_image[3].vk_image,
	        0, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)
	    .insert()
	    .end()
	    .flush();
}

void Bloom::resize()
{
	m_context->wait();
	destroy_resource();
	create_resource();
}

void Bloom::draw(CommandBufferRecorder &recorder, const PathTracing &path_tracing)
{
	draw(recorder, path_tracing.descriptor.sets[m_context->ping_pong]);
}

void Bloom::draw(CommandBufferRecorder &recorder, const TAA &taa)
{
	draw(recorder, taa.descriptor.sets[m_context->ping_pong]);
}

bool Bloom::draw_ui()
{
	bool update = false;
	if (ImGui::TreeNode("Bloom"))
	{
		ImGui::DragFloat("Threshold", &m_mask.push_constants.threshold, 0.01f, 0.f, std::numeric_limits<float>::max());
		ImGui::DragFloat("Radius", &m_upsample.push_constants.radius, 0.01f, 0.f, 1.f);
		ImGui::DragFloat("Intensity", &m_blend.push_constants.intensity, 0.01f, 0.f, std::numeric_limits<float>::max());
		ImGui::TreePop();
	}
	return update;
}

void Bloom::draw(CommandBufferRecorder &recorder, VkDescriptorSet input_set)
{
	recorder.begin_marker("Bloom")
	    .begin_marker("Mask")
	    .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_mask.pipeline_layout, {input_set, m_mask.descriptor_set})
	    .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_mask.pipeline)
	    .push_constants(m_mask.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, m_mask.push_constants)
	    .dispatch({m_context->render_extent.width, m_context->render_extent.height, 1}, {8, 8, 1})
	    .end_marker()
	    .insert_barrier()
	    .add_image_barrier(
	        mask_image.vk_image,
	        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .insert()
	    .begin_marker("Down Sample")
	    .execute([&]() {
		    for (uint32_t i = 0; i < 4; i++)
		    {
			    recorder.begin_marker(fmt::format("Down Sample #{}", i))
			        .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_dowsample.pipeline_layout, {m_dowsample.descriptor_sets[i]})
			        .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_dowsample.pipeline)
			        .dispatch({m_context->render_extent.width >> (i + 1), m_context->render_extent.height >> (i + 1), 1}, {8, 8, 1})
			        .insert_barrier()
			        .add_image_barrier(
			            level_image[i].vk_image,
			            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
			            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
			        .insert()
			        .end_marker();
		    }
	    })
	    .end_marker()

	    .begin_marker("Blur")
	    .execute([&]() {
		    for (uint32_t i = 0; i < 4; i++)
		    {
			    recorder.begin_marker(fmt::format("Blur #{}", i))
			        .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_blur.pipeline_layout, {m_blur.descriptor_sets[i]})
			        .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_blur.pipeline)
			        .dispatch({m_context->render_extent.width >> (i + 1), m_context->render_extent.height >> (i + 1), 1}, {8, 8, 1})
			        .end_marker();
		    }
	    })
	    .end_marker()

	    .insert_barrier()
	    .add_image_barrier(
	        blur_image[0].vk_image,
	        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .add_image_barrier(
	        blur_image[1].vk_image,
	        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .add_image_barrier(
	        blur_image[2].vk_image,
	        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .add_image_barrier(
	        blur_image[3].vk_image,
	        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .add_image_barrier(
	        level_image[0].vk_image,
	        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        level_image[1].vk_image,
	        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        level_image[2].vk_image,
	        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        output_image.vk_image,
	        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
	    .insert()

	    .begin_marker("Up Sample")
	    .execute([&]() {
		    for (int32_t i = 2; i >= 0; i--)
		    {
			    recorder.begin_marker(fmt::format("Up Sample #{}", i))
			        .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_upsample.pipeline_layout, {m_upsample.descriptor_sets[i]})
			        .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_upsample.pipeline)
			        .push_constants(m_upsample.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, m_upsample.push_constants)
			        .dispatch({m_context->render_extent.width >> (i + 1), m_context->render_extent.height >> (i + 1), 1}, {8, 8, 1})
			        .insert_barrier()
			        .add_image_barrier(
			            level_image[i].vk_image,
			            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
			            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
			        .insert()
			        .end_marker();
		    }
	    })
	    .end_marker()

	    .begin_marker("Blend")
	    .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_blend.pipeline_layout, {input_set, m_blend.descriptor_set})
	    .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_blend.pipeline)
	    .push_constants(m_blend.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, m_blend.push_constants)
	    .dispatch({m_context->render_extent.width, m_context->render_extent.height, 1}, {8, 8, 1})
	    .end_marker()

	    .insert_barrier()
	    .add_image_barrier(
	        mask_image.vk_image,
	        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        output_image.vk_image,
	        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .add_image_barrier(
	        level_image[0].vk_image,
	        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        level_image[1].vk_image,
	        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        level_image[2].vk_image,
	        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        level_image[3].vk_image,
	        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        blur_image[0].vk_image,
	        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        blur_image[1].vk_image,
	        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        blur_image[2].vk_image,
	        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        blur_image[3].vk_image,
	        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
	    .insert()

	    .end_marker();
}

void Bloom::create_resource()
{
	mask_image = m_context->create_texture_2d(
	    "Bloom Mask Image",
	    m_context->render_extent.width, m_context->render_extent.height,
	    VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	mask_view = m_context->create_texture_view("Bloom Mask View", mask_image.vk_image, VK_FORMAT_R16G16B16A16_SFLOAT);

	output_image = m_context->create_texture_2d(
	    "Bloom Output Image",
	    m_context->render_extent.width, m_context->render_extent.height,
	    VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	output_view = m_context->create_texture_view("Output Mask View", output_image.vk_image, VK_FORMAT_R16G16B16A16_SFLOAT);

	for (uint32_t i = 0; i < 4; i++)
	{
		level_image[i] = m_context->create_texture_2d(
		    fmt::format("Bloom Level Image - {}", i),
		    m_context->render_extent.width >> (i + 1), m_context->render_extent.height >> (i + 1),
		    VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
		blur_image[i] = m_context->create_texture_2d(
		    fmt::format("Bloom Blur Image - {}", i),
		    m_context->render_extent.width >> (i + 1), m_context->render_extent.height >> (i + 1),
		    VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
		level_view[i] = m_context->create_texture_view(fmt::format("Bloom Level View - {}", i), level_image[i].vk_image, VK_FORMAT_R16G16B16A16_SFLOAT);
		blur_view[i]  = m_context->create_texture_view(fmt::format("Bloom Blur View - {}", i), blur_image[i].vk_image, VK_FORMAT_R16G16B16A16_SFLOAT);
	}

	update_descriptor();
	init();
}

void Bloom::update_descriptor()
{
	m_context->update_descriptor()
	    .write_storage_images(0, {mask_view})
	    .update(m_mask.descriptor_set);

	m_context->update_descriptor()
	    .write_sampled_images(0, {output_view})
	    .update(descriptor.set);

	m_context->update_descriptor()
	    .write_sampled_images(0, {mask_view})
	    .write_storage_images(1, {level_view[0]})
	    .write_samplers(2, {sampler})
	    .update(m_dowsample.descriptor_sets[0]);

	m_context->update_descriptor()
	    .write_sampled_images(0, {blur_view[3]})
	    .write_sampled_images(1, {blur_view[2]})
	    .write_storage_images(2, {level_view[2]})
	    .update(m_upsample.descriptor_sets[2]);

	m_context->update_descriptor()
	    .write_sampled_images(0, {level_view[0]})
	    .write_storage_images(1, {output_view})
	    .update(m_blend.descriptor_set);

	for (uint32_t i = 1; i < 4; i++)
	{
		m_context->update_descriptor()
		    .write_sampled_images(0, {level_view[i - 1]})
		    .write_storage_images(1, {level_view[i]})
		    .write_samplers(2, {sampler})
		    .update(m_dowsample.descriptor_sets[i]);
	}

	for (uint32_t i = 0; i < 4; i++)
	{
		m_context->update_descriptor()
		    .write_sampled_images(0, {level_view[i]})
		    .write_storage_images(1, {blur_view[i]})
		    .update(m_blur.descriptor_sets[i]);
	}

	for (int32_t i = 1; i >= 0; i--)
	{
		m_context->update_descriptor()
		    .write_sampled_images(0, {level_view[i + 1]})
		    .write_sampled_images(1, {blur_view[i]})
		    .write_storage_images(2, {level_view[i]})
		    .update(m_upsample.descriptor_sets[i]);
	}
}

void Bloom::destroy_resource()
{
	m_context->destroy(mask_image)
	    .destroy(mask_view)
	    .destroy(output_image)
	    .destroy(output_view)
	    .destroy(level_image)
	    .destroy(level_view)
	    .destroy(blur_image)
	    .destroy(blur_view);
}
