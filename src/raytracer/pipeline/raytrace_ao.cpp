#include "pipeline/raytrace_ao.hpp"

#include <spdlog/fmt/fmt.h>

#include <imgui.h>

#define CAMERA_NEAR_PLANE 0.01f
#define CAMERA_FAR_PLANE 1000.f

static const uint32_t RAY_TRACE_NUM_THREADS_X = 8;
static const uint32_t RAY_TRACE_NUM_THREADS_Y = 4;

static const uint32_t TEMPORAL_ACCUMULATION_NUM_THREADS_X = 8;
static const uint32_t TEMPORAL_ACCUMULATION_NUM_THREADS_Y = 8;

static const uint32_t NUM_THREADS_X = 8;
static const uint32_t NUM_THREADS_Y = 8;

static unsigned char g_ao_raytraced_comp_spv_data[] = {
#include "ao_raytraced.comp.spv.h"
};

static unsigned char g_ao_temporal_accumulation_comp_spv_data[] = {
#include "ao_temporal_accumulation.comp.spv.h"
};

static unsigned char g_ao_bilateral_blur_comp_spv_data[] = {
#include "ao_bilateral_blur.comp.spv.h"
};

static unsigned char g_ao_upsampling_comp_spv_data[] = {
#include "ao_upsampling.comp.spv.h"
};

RayTracedAO::RayTracedAO(const Context &context, const Scene &scene, const GBufferPass &gbuffer_pass, RayTracedScale scale) :
    m_context(&context)
{
	float scale_divisor = powf(2.0f, float(scale));

	m_width       = static_cast<uint32_t>(static_cast<float>(context.render_extent.width) / scale_divisor);
	m_height      = static_cast<uint32_t>(static_cast<float>(context.render_extent.height) / scale_divisor);
	m_gbuffer_mip = static_cast<uint32_t>(scale);

	raytraced_image      = m_context->create_texture_2d("AO RayTraced Image", static_cast<uint32_t>(ceil(float(m_width) / float(RAY_TRACE_NUM_THREADS_X))), static_cast<uint32_t>(ceil(float(m_height) / float(RAY_TRACE_NUM_THREADS_Y))), VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	raytraced_image_view = m_context->create_texture_view("AO RayTraced Image View", raytraced_image.vk_image, VK_FORMAT_R32_UINT);

	for (uint32_t i = 0; i < 2; i++)
	{
		ao_image[i]             = m_context->create_texture_2d(fmt::format("AO Image - {}", i), m_width, m_height, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
		history_length_image[i] = m_context->create_texture_2d(fmt::format("History Length Image - {}", i), m_width, m_height, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
		bilateral_blur_image[i] = m_context->create_texture_2d(fmt::format("Bilateral Blur Image - {}", i), m_width, m_height, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);

		ao_image_view[i]             = m_context->create_texture_view(fmt::format("AO Image View - {}", i), ao_image[i].vk_image, VK_FORMAT_R32_SFLOAT);
		history_length_image_view[i] = m_context->create_texture_view(fmt::format("History Length Image View - {}", i), history_length_image[i].vk_image, VK_FORMAT_R32_SFLOAT);
		bilateral_blur_image_view[i] = m_context->create_texture_view(fmt::format("Bilateral Blur Image View - {}", i), bilateral_blur_image[i].vk_image, VK_FORMAT_R32_SFLOAT);
	}

	upsampled_ao_image      = m_context->create_texture_2d("AO Upsampled Image", m_width, m_height, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	upsampled_ao_image_view = m_context->create_texture_view("AO Upsampled Image View", upsampled_ao_image.vk_image, VK_FORMAT_R32_SFLOAT);

	denoise_tile_buffer               = m_context->create_buffer("AO Denoise Tile Buffer", sizeof(glm::ivec2) * static_cast<uint32_t>(ceil(float(m_width) / float(TEMPORAL_ACCUMULATION_NUM_THREADS_X))) * static_cast<uint32_t>(ceil(float(m_height) / float(TEMPORAL_ACCUMULATION_NUM_THREADS_Y))), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	denoise_tile_dispatch_args_buffer = m_context->create_buffer("AO Denoise Tile Dispatch Args Buffer", sizeof(VkDispatchIndirectCommand), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

	m_raytraced.descriptor_set_layout = m_context->create_descriptor_layout()
	                                        .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                                        .create();
	m_raytraced.descriptor_set  = m_context->allocate_descriptor_set(m_raytraced.descriptor_set_layout);
	m_raytraced.pipeline_layout = m_context->create_pipeline_layout({scene.descriptor.layout, gbuffer_pass.descriptor.layout, m_raytraced.descriptor_set_layout}, sizeof(m_raytraced.push_constant), VK_SHADER_STAGE_COMPUTE_BIT);
	m_raytraced.pipeline        = m_context->create_compute_pipeline((uint32_t *) g_ao_raytraced_comp_spv_data, sizeof(g_ao_raytraced_comp_spv_data), m_raytraced.pipeline_layout);

	m_temporal_accumulation.descriptor_set_layout = m_context->create_descriptor_layout()
	                                                    .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                                                    .add_descriptor_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                                                    .add_descriptor_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                                                    .add_descriptor_binding(3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                                                    .add_descriptor_binding(4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                                                    .add_descriptor_binding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
	                                                    .add_descriptor_binding(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
	                                                    .create();
	m_temporal_accumulation.descriptor_sets = m_context->allocate_descriptor_sets<2>(m_temporal_accumulation.descriptor_set_layout);
	m_temporal_accumulation.pipeline_layout = m_context->create_pipeline_layout({scene.descriptor.layout, gbuffer_pass.descriptor.layout, m_temporal_accumulation.descriptor_set_layout}, sizeof(m_temporal_accumulation.push_constant), VK_SHADER_STAGE_COMPUTE_BIT);
	//m_temporal_accumulation.pipeline        = m_context->create_compute_pipeline("ao_temporal_accumulation.slang", m_temporal_accumulation.pipeline_layout);

	m_bilateral_blur.descriptor_set_layout = m_context->create_descriptor_layout()
	                                             .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                                             .add_descriptor_binding(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                                             .add_descriptor_binding(2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                                             .add_descriptor_binding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
	                                             .create();
	m_bilateral_blur.descriptor_sets[0] = m_context->allocate_descriptor_sets<2>(m_bilateral_blur.descriptor_set_layout);
	m_bilateral_blur.descriptor_sets[1] = m_context->allocate_descriptor_sets<2>(m_bilateral_blur.descriptor_set_layout);
	m_bilateral_blur.pipeline_layout    = m_context->create_pipeline_layout({scene.descriptor.layout, gbuffer_pass.descriptor.layout, m_bilateral_blur.descriptor_set_layout}, sizeof(m_bilateral_blur.push_constant), VK_SHADER_STAGE_COMPUTE_BIT);
	//m_bilateral_blur.pipeline           = m_context->create_compute_pipeline("ao_bilateral_blur.slang", m_bilateral_blur.pipeline_layout);

	m_upsampling.descriptor_set_layout = m_context->create_descriptor_layout()
	                                         .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                                         .add_descriptor_binding(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                                         .create();
	m_upsampling.descriptor_set  = m_context->allocate_descriptor_set(m_upsampling.descriptor_set_layout);
	m_upsampling.pipeline_layout = m_context->create_pipeline_layout({scene.descriptor.layout, gbuffer_pass.descriptor.layout, m_upsampling.descriptor_set_layout}, sizeof(m_upsampling.push_constant), VK_SHADER_STAGE_COMPUTE_BIT);
	//m_upsampling.pipeline        = m_context->create_compute_pipeline("ao_upsampling.slang", m_upsampling.pipeline_layout);

	m_context->update_descriptor()
	    .write_storage_images(0, {raytraced_image_view})
	    .update(m_raytraced.descriptor_set);

	for (uint32_t i = 0; i < 2; i++)
	{
		m_context->update_descriptor()
		    .write_sampled_images(0, {raytraced_image_view})
		    .write_storage_images(1, {ao_image_view[i]})
		    .write_storage_images(2, {history_length_image_view[i]})
		    .write_sampled_images(3, {ao_image_view[!i]})
		    .write_sampled_images(4, {history_length_image_view[!i]})
		    .write_storage_buffers(5, {denoise_tile_buffer.vk_buffer})
		    .write_storage_buffers(6, {denoise_tile_dispatch_args_buffer.vk_buffer})
		    .update(m_temporal_accumulation.descriptor_sets[i]);
	}

	for (uint32_t i = 0; i < 2; i++)
	{
		for (uint32_t j = 0; j < 2; j++)
		{
			m_context->update_descriptor()
			    .write_storage_images(0, {bilateral_blur_image_view[j]})
			    .write_sampled_images(1, {j == 0 ? ao_image_view[i] : bilateral_blur_image_view[0]})
			    .write_sampled_images(2, {history_length_image_view[i]})
			    .write_storage_buffers(3, {denoise_tile_buffer.vk_buffer})
			    .update(m_bilateral_blur.descriptor_sets[i][j]);
		}
	}

	m_context->update_descriptor()
	    .write_storage_images(0, {upsampled_ao_image_view})
	    .write_sampled_images(1, {bilateral_blur_image_view[1]})
	    .update(m_upsampling.descriptor_set);

	m_context->record_command()
	    .begin()
	    .insert_barrier()
	    .add_image_barrier(
	        raytraced_image.vk_image,
	        0, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        ao_image[0].vk_image,
	        0, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        ao_image[1].vk_image,
	        0, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .add_image_barrier(
	        history_length_image[0].vk_image,
	        0, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        history_length_image[1].vk_image,
	        0, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .add_image_barrier(
	        bilateral_blur_image[0].vk_image,
	        0, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        bilateral_blur_image[1].vk_image,
	        0, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .add_image_barrier(
	        upsampled_ao_image.vk_image,
	        0, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .insert()
	    .end()
	    .flush();
}

RayTracedAO::~RayTracedAO()
{
	m_context->destroy(raytraced_image)
	    .destroy(raytraced_image_view)
	    .destroy(ao_image)
	    .destroy(ao_image_view)
	    .destroy(history_length_image)
	    .destroy(history_length_image_view)
	    .destroy(bilateral_blur_image)
	    .destroy(bilateral_blur_image_view)
	    .destroy(upsampled_ao_image)
	    .destroy(upsampled_ao_image_view)
	    .destroy(denoise_tile_buffer)
	    .destroy(denoise_tile_dispatch_args_buffer)
	    .destroy(m_raytraced.descriptor_set_layout)
	    .destroy(m_temporal_accumulation.descriptor_set_layout)
	    .destroy(m_bilateral_blur.descriptor_set_layout)
	    .destroy(m_upsampling.descriptor_set_layout)
	    .destroy(m_raytraced.descriptor_set)
	    .destroy(m_temporal_accumulation.descriptor_sets)
	    .destroy(m_bilateral_blur.descriptor_sets)
	    .destroy(m_upsampling.descriptor_set)
	    .destroy(m_raytraced.pipeline_layout)
	    .destroy(m_temporal_accumulation.pipeline_layout)
	    .destroy(m_bilateral_blur.pipeline_layout)
	    .destroy(m_upsampling.pipeline_layout)
	    .destroy(m_raytraced.pipeline)
	    .destroy(m_temporal_accumulation.pipeline)
	    .destroy(m_bilateral_blur.pipeline)
	    .destroy(m_upsampling.pipeline);
}

void RayTracedAO::draw(CommandBufferRecorder &recorder, const Scene &scene, const GBufferPass &gbuffer_pass)
{
	m_raytraced.push_constant.gbuffer_mip             = m_gbuffer_mip;
	m_temporal_accumulation.push_constant.gbuffer_mip = m_gbuffer_mip;
	m_upsampling.push_constant.gbuffer_mip            = m_gbuffer_mip;

	float z_buffer_params_x = -1.f + (CAMERA_NEAR_PLANE / CAMERA_FAR_PLANE);

	m_bilateral_blur.push_constant.gbuffer_mip     = m_gbuffer_mip;
	m_bilateral_blur.push_constant.z_buffer_params = glm::vec4(z_buffer_params_x, 1.0f, z_buffer_params_x / CAMERA_NEAR_PLANE, 1.0f / CAMERA_NEAR_PLANE);

	recorder.begin_marker("RayTraced AO")
	    .begin_marker("Ray Traced")
	    .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_raytraced.pipeline_layout, {scene.descriptor.set, gbuffer_pass.descriptor.sets[m_context->ping_pong], m_raytraced.descriptor_set})
	    .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_raytraced.pipeline)
	    .push_constants(m_raytraced.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, m_raytraced.push_constant)
	    .dispatch({m_width, m_height, 1}, {RAY_TRACE_NUM_THREADS_X, RAY_TRACE_NUM_THREADS_Y, 1})
	    .end_marker()

	    .insert_barrier()
	    .add_image_barrier(
	        raytraced_image.vk_image,
	        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .insert()

	    .begin_marker("Temporal Accumulation")
	    //.bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_temporal_accumulation.pipeline_layout, {scene.descriptor.set, gbuffer_pass.descriptor.sets[m_context->ping_pong], m_temporal_accumulation.descriptor_sets[m_context->ping_pong]})
	    //.bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_temporal_accumulation.pipeline)
	    //.push_constants(m_temporal_accumulation.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, m_temporal_accumulation.push_constant)
	    //.dispatch({m_width, m_height, 1}, {TEMPORAL_ACCUMULATION_NUM_THREADS_X, TEMPORAL_ACCUMULATION_NUM_THREADS_Y, 1})
	    .end_marker()

	    .insert_barrier()
	    .add_image_barrier(
	        ao_image[m_context->ping_pong].vk_image,
	        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .add_image_barrier(
	        ao_image[!m_context->ping_pong].vk_image,
	        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        history_length_image[m_context->ping_pong].vk_image,
	        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .add_image_barrier(
	        history_length_image[!m_context->ping_pong].vk_image,
	        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
	    .insert()

	    .begin_marker("Bilateral Blur")
	    .begin_marker("Vertical Blur")

	    /*.bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_bilateral_blur.pipeline_layout, {scene.descriptor.set, gbuffer_pass.descriptor.sets[m_context->ping_pong], m_bilateral_blur.descriptor_sets[m_context->ping_pong][0]})
	    .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_bilateral_blur.pipeline)
	    .execute([&]() { m_bilateral_blur.push_constant.direction = glm::ivec2(1, 0); })
	    .push_constants(m_bilateral_blur.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, m_bilateral_blur.push_constant)
	    .dispatch_indirect(denoise_tile_dispatch_args_buffer.vk_buffer)*/

	    .end_marker()

	    .insert_barrier()
	    .add_image_barrier(bilateral_blur_image[0].vk_image,
	                       VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	                       VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .add_image_barrier(bilateral_blur_image[1].vk_image,
	                       VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
	    .insert(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)

	    .begin_marker("Horizontal Blur")

	    //.bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_bilateral_blur.pipeline_layout, {scene.descriptor.set, gbuffer_pass.descriptor.sets[m_context->ping_pong], m_bilateral_blur.descriptor_sets[m_context->ping_pong][1]})
	    //.bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_bilateral_blur.pipeline)
	    //.execute([&]() { m_bilateral_blur.push_constant.direction = glm::ivec2(0, 1); })
	    //.push_constants(m_bilateral_blur.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, m_bilateral_blur.push_constant)
	    //.dispatch_indirect(denoise_tile_dispatch_args_buffer.vk_buffer)

	    .end_marker()

	    .insert_barrier()
	    .add_image_barrier(bilateral_blur_image[1].vk_image,
	                       VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	                       VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .add_image_barrier(
	        upsampled_ao_image.vk_image,
	        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
	    .insert(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)

	    .end_marker()

	    .begin_marker("Upsampling")

	    //.bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_upsampling.pipeline_layout, {scene.descriptor.set, gbuffer_pass.descriptor.sets[m_context->ping_pong], m_upsampling.descriptor_set})
	    //.bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_upsampling.pipeline)
	    //.push_constants(m_upsampling.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, m_upsampling.push_constant)
	    //.dispatch({m_context->render_extent.width, m_context->render_extent.height, 1}, {NUM_THREADS_X, NUM_THREADS_Y, 1})

	    .end_marker()

	    .insert_barrier()
	    .add_image_barrier(
	        raytraced_image.vk_image,
	        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(bilateral_blur_image[0].vk_image,
	                       VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        upsampled_ao_image.vk_image,
	        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .insert()

	    .end_marker();
}

bool RayTracedAO::draw_ui()
{
	bool update = false;
	if (ImGui::TreeNode("Ray Traced AO"))
	{
		update |= ImGui::SliderFloat("Ray Length", &m_raytraced.push_constant.ray_length, 0.0f, 10.0f);
		update |= ImGui::DragFloat("Ray Traced Bias", &m_raytraced.push_constant.bias, 0.001f, 0.0f, 100.0f, "%.3f");
		update |= ImGui::DragInt("Blur Radius", &m_bilateral_blur.push_constant.radius, 1, 1, 10);
		update |= ImGui::Checkbox("Debug", reinterpret_cast<bool *>(&m_upsampling.push_constant.debug));
		ImGui::TreePop();
	}
	return update;
}
