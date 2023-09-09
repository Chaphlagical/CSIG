#include "pipeline/raytrace_di.hpp"

#include <imgui.h>
#include <spdlog/fmt/fmt.h>

#define NUM_THREADS_X 8
#define NUM_THREADS_Y 8

struct Reservoir
{
	int       light_id;
	float     p_hat;
	float     sum_weights;
	float     w;
	glm::vec3 padding;
	uint32_t  num_samples;
};

RayTracedDI::RayTracedDI(const Context &context, const Scene &scene, const GBufferPass &gbuffer_pass, RayTracedScale scale) :
    m_context(&context), m_scale(scale)
{
	m_raytrace.temporal.descriptor_set_layout = m_context->create_descriptor_layout()
	                                                // Temporal Reservoir
	                                                .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
	                                                // Passthrough Reservoir
	                                                .add_descriptor_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
	                                                .create();
	m_raytrace.temporal.descriptor_set  = m_context->allocate_descriptor_set(m_raytrace.temporal.descriptor_set_layout);
	m_raytrace.temporal.pipeline_layout = m_context->create_pipeline_layout({scene.descriptor.layout,
	                                                                         gbuffer_pass.descriptor.layout,
	                                                                         m_raytrace.temporal.descriptor_set_layout},
	                                                                        sizeof(m_raytrace.temporal.push_constants), VK_SHADER_STAGE_COMPUTE_BIT);
	m_raytrace.temporal.pipeline        = m_context->create_compute_pipeline("di_temporal.slang", m_raytrace.temporal.pipeline_layout);

	m_raytrace.spatial.descriptor_set_layout = m_context->create_descriptor_layout()
	                                               // Spatial Reservoir
	                                               .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
	                                               // Passthrough Reservoir
	                                               .add_descriptor_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
	                                               .create();
	m_raytrace.spatial.descriptor_set  = m_context->allocate_descriptor_set(m_raytrace.spatial.descriptor_set_layout);
	m_raytrace.spatial.pipeline_layout = m_context->create_pipeline_layout({scene.descriptor.layout,
	                                                                        gbuffer_pass.descriptor.layout,
	                                                                        m_raytrace.spatial.descriptor_set_layout},
	                                                                       sizeof(m_raytrace.spatial.push_constants), VK_SHADER_STAGE_COMPUTE_BIT);
	m_raytrace.spatial.pipeline        = m_context->create_compute_pipeline("di_spatial.slang", m_raytrace.spatial.pipeline_layout);

	m_raytrace.composite.descriptor_set_layout = m_context->create_descriptor_layout()
	                                                 // Temporal Reservoir
	                                                 .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
	                                                 // Spatial Reservoir
	                                                 .add_descriptor_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
	                                                 // Raytraced Image
	                                                 .add_descriptor_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                                                 .create();
	m_raytrace.composite.descriptor_set  = m_context->allocate_descriptor_set(m_raytrace.composite.descriptor_set_layout);
	m_raytrace.composite.pipeline_layout = m_context->create_pipeline_layout({scene.descriptor.layout,
	                                                                          gbuffer_pass.descriptor.layout,
	                                                                          m_raytrace.composite.descriptor_set_layout},
	                                                                         sizeof(m_raytrace.composite.push_constants), VK_SHADER_STAGE_COMPUTE_BIT);
	m_raytrace.composite.pipeline        = m_context->create_compute_pipeline("di_composite.slang", m_raytrace.composite.pipeline_layout);

	m_reprojection.descriptor_set_layout = m_context->create_descriptor_layout()
	                                           // Output image
	                                           .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT, 1)
	                                           // Moments image
	                                           .add_descriptor_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT, 1)
	                                           // Input image
	                                           .add_descriptor_binding(2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT, 1)
	                                           // History output image
	                                           .add_descriptor_binding(3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT, 1)
	                                           // History moments image
	                                           .add_descriptor_binding(4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT, 1)
	                                           // Denoise Tile Data
	                                           .add_descriptor_binding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT, 1)
	                                           // Denoise Tile Dispatch Args
	                                           .add_descriptor_binding(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT, 1)
	                                           // Copy Tile Data
	                                           .add_descriptor_binding(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT, 1)
	                                           // Copy Tile Dispatch Args
	                                           .add_descriptor_binding(8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT, 1)
	                                           .create();
	m_reprojection.descriptor_sets = m_context->allocate_descriptor_sets<2>(m_reprojection.descriptor_set_layout);
	m_reprojection.pipeline_layout = m_context->create_pipeline_layout({
	                                                                       scene.descriptor.layout,
	                                                                       gbuffer_pass.descriptor.layout,
	                                                                       m_reprojection.descriptor_set_layout,
	                                                                   },
	                                                                   sizeof(m_reprojection.push_constants), VK_SHADER_STAGE_COMPUTE_BIT);
	m_reprojection.pipeline        = m_context->create_compute_pipeline("di_reprojection.slang", m_reprojection.pipeline_layout);

	m_denoise.copy_tiles.descriptor_set_layout = m_context->create_descriptor_layout()
	                                                 // Output image
	                                                 .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT, 1)
	                                                 // Input image
	                                                 .add_descriptor_binding(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT, 1)
	                                                 // Copy Tile Data
	                                                 .add_descriptor_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT, 1)
	                                                 .create();
	m_denoise.copy_tiles.copy_atrous_sets       = m_context->allocate_descriptor_sets<2>(m_denoise.copy_tiles.descriptor_set_layout);
	m_denoise.copy_tiles.copy_reprojection_sets = m_context->allocate_descriptor_sets<2>(m_denoise.copy_tiles.descriptor_set_layout);
	m_denoise.copy_tiles.pipeline_layout        = m_context->create_pipeline_layout({m_denoise.copy_tiles.descriptor_set_layout});
	m_denoise.copy_tiles.pipeline               = m_context->create_compute_pipeline("di_copy_tiles.slang", m_denoise.copy_tiles.pipeline_layout);

	m_denoise.a_trous.descriptor_set_layout = m_context->create_descriptor_layout()
	                                              // Output image
	                                              .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT, 1)
	                                              // Input image
	                                              .add_descriptor_binding(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT, 1)
	                                              // Denoise Tile Data
	                                              .add_descriptor_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT, 1)
	                                              .create();
	m_denoise.a_trous.filter_reprojection_sets = m_context->allocate_descriptor_sets<2>(m_denoise.a_trous.descriptor_set_layout);
	m_denoise.a_trous.filter_atrous_sets       = m_context->allocate_descriptor_sets<2>(m_denoise.a_trous.descriptor_set_layout);
	m_denoise.a_trous.pipeline_layout          = m_context->create_pipeline_layout({
                                                                              scene.descriptor.layout,
                                                                              gbuffer_pass.descriptor.layout,
                                                                              m_denoise.a_trous.descriptor_set_layout,
                                                                          },
	                                                                               sizeof(m_denoise.a_trous.push_constants), VK_SHADER_STAGE_COMPUTE_BIT);
	m_denoise.a_trous.pipeline                 = m_context->create_compute_pipeline("di_atrous.slang", m_denoise.a_trous.pipeline_layout);

	m_upsampling.descriptor_set_layout = m_context->create_descriptor_layout()
	                                         // Output image
	                                         .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT, 1)
	                                         // Input image
	                                         .add_descriptor_binding(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT, 1)
	                                         .create();
	m_upsampling.descriptor_set  = m_context->allocate_descriptor_set(m_upsampling.descriptor_set_layout);
	m_upsampling.pipeline_layout = m_context->create_pipeline_layout({
	                                                                     scene.descriptor.layout,
	                                                                     gbuffer_pass.descriptor.layout,
	                                                                     m_upsampling.descriptor_set_layout,
	                                                                 },
	                                                                 sizeof(m_upsampling.push_constants), VK_SHADER_STAGE_COMPUTE_BIT);
	m_upsampling.pipeline        = m_context->create_compute_pipeline("di_upsampling.slang", m_upsampling.pipeline_layout);

	descriptor.layout = m_context->create_descriptor_layout()
	                        .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                        .create();
	descriptor.set = m_context->allocate_descriptor_set(descriptor.layout);

	create_resource();
}

RayTracedDI::~RayTracedDI()
{
	m_context->destroy(raytraced_image)
	    .destroy(raytraced_view)
	    .destroy(reprojection_output_image)
	    .destroy(reprojection_output_view)
	    .destroy(reprojection_moment_image)
	    .destroy(reprojection_moment_view)
	    .destroy(a_trous_image)
	    .destroy(a_trous_view)
	    .destroy(upsampling_image)
	    .destroy(upsampling_view)
	    .destroy(temporal_reservoir_buffer)
	    .destroy(passthrough_reservoir_buffer)
	    .destroy(spatial_reservoir_buffer)
	    .destroy(denoise_tile_data_buffer)
	    .destroy(copy_tile_data_buffer)
	    .destroy(denoise_tile_dispatch_args_buffer)
	    .destroy(copy_tile_dispatch_args_buffer)
	    .destroy(descriptor.layout)
	    .destroy(descriptor.set)
	    .destroy(m_raytrace.spatial.pipeline)
	    .destroy(m_raytrace.spatial.pipeline_layout)
	    .destroy(m_raytrace.spatial.descriptor_set_layout)
	    .destroy(m_raytrace.spatial.descriptor_set)
	    .destroy(m_raytrace.temporal.pipeline)
	    .destroy(m_raytrace.temporal.pipeline_layout)
	    .destroy(m_raytrace.temporal.descriptor_set_layout)
	    .destroy(m_raytrace.temporal.descriptor_set)
	    .destroy(m_raytrace.composite.pipeline)
	    .destroy(m_raytrace.composite.pipeline_layout)
	    .destroy(m_raytrace.composite.descriptor_set_layout)
	    .destroy(m_raytrace.composite.descriptor_set)
	    .destroy(m_reprojection.descriptor_set_layout)
	    .destroy(m_reprojection.descriptor_sets)
	    .destroy(m_reprojection.pipeline_layout)
	    .destroy(m_reprojection.pipeline)
	    .destroy(m_denoise.copy_tiles.descriptor_set_layout)
	    .destroy(m_denoise.copy_tiles.copy_atrous_sets)
	    .destroy(m_denoise.copy_tiles.copy_reprojection_sets)
	    .destroy(m_denoise.copy_tiles.pipeline_layout)
	    .destroy(m_denoise.copy_tiles.pipeline)
	    .destroy(m_denoise.a_trous.descriptor_set_layout)
	    .destroy(m_denoise.a_trous.filter_reprojection_sets)
	    .destroy(m_denoise.a_trous.filter_atrous_sets)
	    .destroy(m_denoise.a_trous.pipeline_layout)
	    .destroy(m_denoise.a_trous.pipeline)
	    .destroy(m_upsampling.descriptor_set_layout)
	    .destroy(m_upsampling.descriptor_set)
	    .destroy(m_upsampling.pipeline_layout)
	    .destroy(m_upsampling.pipeline);
}

void RayTracedDI::init()
{
	m_context->record_command()
	    .begin()
	    .insert_barrier()
	    .add_buffer_barrier(
	        temporal_reservoir_buffer.vk_buffer,
	        0, VK_ACCESS_SHADER_READ_BIT)
	    .add_buffer_barrier(
	        spatial_reservoir_buffer.vk_buffer,
	        0, VK_ACCESS_SHADER_WRITE_BIT)
	    .add_buffer_barrier(
	        passthrough_reservoir_buffer.vk_buffer,
	        0, VK_ACCESS_SHADER_WRITE_BIT)
	    .add_image_barrier(
	        raytraced_image.vk_image,
	        0, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        reprojection_output_image[m_context->ping_pong].vk_image,
	        0, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        reprojection_output_image[!m_context->ping_pong].vk_image,
	        0, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .add_image_barrier(
	        reprojection_moment_image[m_context->ping_pong].vk_image,
	        0, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        reprojection_moment_image[!m_context->ping_pong].vk_image,
	        0, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .add_image_barrier(
	        a_trous_image[m_context->ping_pong].vk_image,
	        0, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        a_trous_image[!m_context->ping_pong].vk_image,
	        0, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .add_image_barrier(
	        upsampling_image.vk_image,
	        0, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .add_buffer_barrier(
	        copy_tile_dispatch_args_buffer.vk_buffer,
	        0, VK_ACCESS_SHADER_WRITE_BIT)
	    .add_buffer_barrier(
	        denoise_tile_dispatch_args_buffer.vk_buffer,
	        0, VK_ACCESS_SHADER_WRITE_BIT)
	    .add_buffer_barrier(
	        copy_tile_data_buffer.vk_buffer,
	        0, VK_ACCESS_SHADER_WRITE_BIT)
	    .add_buffer_barrier(
	        denoise_tile_data_buffer.vk_buffer,
	        0, VK_ACCESS_SHADER_WRITE_BIT)
	    .insert()
	    .end()
	    .flush();
}

void RayTracedDI::resize()
{
	m_context->wait();
	destroy_resource();
	create_resource();
}

void RayTracedDI::draw(CommandBufferRecorder &recorder, const Scene &scene, const GBufferPass &gbuffer_pass)
{
	m_raytrace.temporal.push_constants.gbuffer_mip  = m_gbuffer_mip;
	m_raytrace.spatial.push_constants.gbuffer_mip   = m_gbuffer_mip;
	m_raytrace.composite.push_constants.gbuffer_mip = m_gbuffer_mip;

	recorder
	    .begin_marker("Raytraced DI")
	    .begin_marker("Ray Traced")
	    .begin_marker("Temporal Pass")
	    .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_raytrace.temporal.pipeline_layout, {scene.descriptor.set, gbuffer_pass.descriptor.sets[m_context->ping_pong], m_raytrace.temporal.descriptor_set})
	    .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_raytrace.temporal.pipeline)
	    .push_constants(m_raytrace.temporal.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, m_raytrace.temporal.push_constants)
	    .dispatch({m_width, m_height, 1}, {NUM_THREADS_X, NUM_THREADS_Y, 1})
	    .end_marker()
	    .insert_barrier()
	    .add_buffer_barrier(
	        passthrough_reservoir_buffer.vk_buffer,
	        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
	    .insert(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
	    .begin_marker("Spatial Pass")
	    .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_raytrace.spatial.pipeline_layout, {scene.descriptor.set, gbuffer_pass.descriptor.sets[m_context->ping_pong], m_raytrace.spatial.descriptor_set})
	    .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_raytrace.spatial.pipeline)
	    .push_constants(m_raytrace.spatial.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, m_raytrace.spatial.push_constants)
	    .dispatch({m_width, m_height, 1}, {NUM_THREADS_X, NUM_THREADS_Y, 1})
	    .end_marker()
	    .insert_barrier()
	    .add_buffer_barrier(
	        temporal_reservoir_buffer.vk_buffer,
	        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT)
	    .add_buffer_barrier(
	        spatial_reservoir_buffer.vk_buffer,
	        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
	    .insert(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
	    .begin_marker("Composite Pass")
	    .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_raytrace.composite.pipeline_layout, {scene.descriptor.set, gbuffer_pass.descriptor.sets[m_context->ping_pong], m_raytrace.composite.descriptor_set})
	    .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_raytrace.composite.pipeline)
	    .push_constants(m_raytrace.composite.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, m_raytrace.composite.push_constants)
	    .dispatch({m_width, m_height, 1}, {NUM_THREADS_X, NUM_THREADS_Y, 1})
	    .end_marker()
	    .insert_barrier()
	    .add_buffer_barrier(
	        passthrough_reservoir_buffer.vk_buffer,
	        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT)
	    .add_buffer_barrier(
	        temporal_reservoir_buffer.vk_buffer,
	        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
	    .add_buffer_barrier(
	        spatial_reservoir_buffer.vk_buffer,
	        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT)
	    .add_image_barrier(
	        raytraced_image.vk_image,
	        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .insert(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
	    .end_marker()

	    .begin_marker("Reprojection")
	    .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_reprojection.pipeline_layout, {scene.descriptor.set, gbuffer_pass.descriptor.sets[m_context->ping_pong], m_reprojection.descriptor_sets[m_context->ping_pong]})
	    .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_reprojection.pipeline)
	    .push_constants(m_reprojection.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, m_reprojection.push_constants)
	    .dispatch({m_width, m_height, 1}, {NUM_THREADS_X, NUM_THREADS_Y, 1})
	    .end_marker()
	    .insert_barrier()
	    .add_buffer_barrier(
	        copy_tile_dispatch_args_buffer.vk_buffer,
	        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT)
	    .add_buffer_barrier(
	        denoise_tile_dispatch_args_buffer.vk_buffer,
	        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT)
	    .add_buffer_barrier(
	        copy_tile_data_buffer.vk_buffer,
	        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
	    .add_buffer_barrier(
	        denoise_tile_data_buffer.vk_buffer,
	        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT)
	    .add_image_barrier(
	        reprojection_output_image[m_context->ping_pong].vk_image,
	        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .add_image_barrier(
	        reprojection_output_image[!m_context->ping_pong].vk_image,
	        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        reprojection_moment_image[m_context->ping_pong].vk_image,
	        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .add_image_barrier(
	        reprojection_moment_image[!m_context->ping_pong].vk_image,
	        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        upsampling_image.vk_image,
	        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
	    .insert(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT)
	    .begin_marker("Denoise")
	    .execute([&](CommandBufferRecorder &recorder) {
		    bool ping_pong = false;
		    for (uint32_t i = 0; i < 3; i++)
		    {
			    recorder.begin_marker(fmt::format("Iteration - {}", i))
			        .begin_marker("Copy Tile Data")
			        .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_denoise.copy_tiles.pipeline_layout, {i == 0 ? m_denoise.copy_tiles.copy_reprojection_sets[m_context->ping_pong] : m_denoise.copy_tiles.copy_atrous_sets[ping_pong]})
			        .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_denoise.copy_tiles.pipeline)
			        .dispatch_indirect(copy_tile_dispatch_args_buffer.vk_buffer)
			        .end_marker()
			        .begin_marker("A-trous Filter")
			        .execute([&]() { m_denoise.a_trous.push_constants.step_size = 1 << i; })
			        .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_denoise.a_trous.pipeline_layout, {scene.descriptor.set, gbuffer_pass.descriptor.sets[m_context->ping_pong], i == 0 ? m_denoise.a_trous.filter_reprojection_sets[m_context->ping_pong] : m_denoise.a_trous.filter_atrous_sets[ping_pong]})
			        .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_denoise.a_trous.pipeline)
			        .push_constants(m_denoise.a_trous.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, m_denoise.a_trous.push_constants)
			        .dispatch_indirect(denoise_tile_dispatch_args_buffer.vk_buffer)
			        .end_marker()
			        .insert_barrier()
			        .add_image_barrier(
			            a_trous_image[ping_pong].vk_image,
			            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
			            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
			        .add_image_barrier(
			            a_trous_image[!ping_pong].vk_image,
			            VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
			            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
			        .insert(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
			        .end_marker();
			    ping_pong = !ping_pong;
		    }
	    })
	    .end_marker()
	    .begin_marker("Upsampling")
	    .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_upsampling.pipeline_layout, {scene.descriptor.set, gbuffer_pass.descriptor.sets[m_context->ping_pong], m_upsampling.descriptor_set})
	    .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_upsampling.pipeline)
	    .push_constants(m_upsampling.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, m_upsampling.push_constants)
	    .dispatch({m_context->render_extent.width, m_context->render_extent.height, 1}, {NUM_THREADS_X, NUM_THREADS_Y, 1})
	    .end_marker()
	    .insert_barrier()
	    .add_image_barrier(
	        raytraced_image.vk_image,
	        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        upsampling_image.vk_image,
	        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .add_image_barrier(
	        a_trous_image[0].vk_image,
	        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        a_trous_image[1].vk_image,
	        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .add_buffer_barrier(
	        copy_tile_dispatch_args_buffer.vk_buffer,
	        VK_ACCESS_INDIRECT_COMMAND_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT)
	    .add_buffer_barrier(
	        denoise_tile_dispatch_args_buffer.vk_buffer,
	        VK_ACCESS_INDIRECT_COMMAND_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT)
	    .add_buffer_barrier(
	        copy_tile_data_buffer.vk_buffer,
	        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT)
	    .add_buffer_barrier(
	        denoise_tile_data_buffer.vk_buffer,
	        VK_ACCESS_INDIRECT_COMMAND_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT)
	    .insert()

	    .end_marker();
}

bool RayTracedDI::draw_ui()
{
	bool update = false;
	if (ImGui::TreeNode("RayTraced DI"))
	{
		update |= ImGui::Checkbox("Temporal Reuse", reinterpret_cast<bool *>(&m_raytrace.temporal.push_constants.temporal_reuse));
		update |= ImGui::Checkbox("Spatial Reuse", reinterpret_cast<bool *>(&m_raytrace.spatial.push_constants.spatial_reuse));
		ImGui::TreePop();
	}
	return update;
}

void RayTracedDI::create_resource()
{
	float scale_divisor = powf(2.0f, float(m_scale));

	m_width  = static_cast<uint32_t>(static_cast<float>(m_context->render_extent.width) / scale_divisor);
	m_height = static_cast<uint32_t>(static_cast<float>(m_context->render_extent.height) / scale_divisor);

	m_gbuffer_mip = static_cast<uint32_t>(m_scale);

	size_t reservoir_size = static_cast<size_t>(m_width * m_height) * sizeof(Reservoir);

	temporal_reservoir_buffer = m_context->create_buffer(
	    "DI Temporal Reservoir Buffer",
	    reservoir_size,
	    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	    VMA_MEMORY_USAGE_GPU_ONLY);
	passthrough_reservoir_buffer = m_context->create_buffer(
	    "DI Passthrough Reservoir Buffer",
	    reservoir_size,
	    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	    VMA_MEMORY_USAGE_GPU_ONLY);
	spatial_reservoir_buffer = m_context->create_buffer(
	    "DI Spatial Reservoir Buffer",
	    reservoir_size,
	    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	    VMA_MEMORY_USAGE_GPU_ONLY);
	denoise_tile_data_buffer = m_context->create_buffer(
	    "DI Denoise Tile Data Buffer",
	    sizeof(glm::ivec2) * static_cast<uint32_t>(ceil(float(m_width) / float(NUM_THREADS_X))) * static_cast<uint32_t>(ceil(float(m_height) / float(NUM_THREADS_Y))),
	    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
	    VMA_MEMORY_USAGE_GPU_ONLY);
	copy_tile_data_buffer = m_context->create_buffer(
	    "DI Copy Tile Data Buffer",
	    sizeof(glm::ivec2) * static_cast<uint32_t>(ceil(float(m_width) / float(NUM_THREADS_X))) * static_cast<uint32_t>(ceil(float(m_height) / float(NUM_THREADS_Y))),
	    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
	    VMA_MEMORY_USAGE_GPU_ONLY);
	denoise_tile_dispatch_args_buffer = m_context->create_buffer(
	    "DI Denoise Tile Dispatch Args Buffer",
	    sizeof(int32_t) * 3,
	    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
	    VMA_MEMORY_USAGE_GPU_ONLY);
	copy_tile_dispatch_args_buffer = m_context->create_buffer(
	    "DI Copy Tile Dispatch Args Buffer",
	    sizeof(int32_t) * 3,
	    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
	    VMA_MEMORY_USAGE_GPU_ONLY);

	raytraced_image = m_context->create_texture_2d(
	    "DI RayTraced Image",
	    m_width, m_height,
	    VK_FORMAT_R32G32B32A32_SFLOAT,
	    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	raytraced_view = m_context->create_texture_view("DI RayTraced View", raytraced_image.vk_image, VK_FORMAT_R32G32B32A32_SFLOAT);

	for (uint32_t i = 0; i < 2; i++)
	{
		reprojection_output_image[i] = m_context->create_texture_2d(
		    fmt::format("DI Reprojection Output Image - {}", i),
		    m_width, m_height,
		    VK_FORMAT_R16G16B16A16_SFLOAT,
		    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
		reprojection_moment_image[i] = m_context->create_texture_2d(
		    fmt::format("DI Reprojection Moment Image - {}", i),
		    m_width, m_height,
		    VK_FORMAT_R16G16B16A16_SFLOAT,
		    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
		a_trous_image[i] = m_context->create_texture_2d(
		    fmt::format("DI A-Trous Image - {}", i),
		    m_width, m_height,
		    VK_FORMAT_R16G16B16A16_SFLOAT,
		    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
		reprojection_output_view[i] = m_context->create_texture_view(
		    fmt::format("DI Reprojection Output View - {}", i),
		    reprojection_output_image[i].vk_image,
		    VK_FORMAT_R16G16B16A16_SFLOAT);
		reprojection_moment_view[i] = m_context->create_texture_view(
		    fmt::format("DI Reprojection Moment View - {}", i),
		    reprojection_moment_image[i].vk_image,
		    VK_FORMAT_R16G16B16A16_SFLOAT);
		a_trous_view[i] = m_context->create_texture_view(
		    fmt::format("DI A-Trous View - {}", i),
		    a_trous_image[i].vk_image,
		    VK_FORMAT_R16G16B16A16_SFLOAT);
	}

	upsampling_image = m_context->create_texture_2d(
	    "DI Upsampling Output Image",
	    m_context->render_extent.width, m_context->render_extent.height,
	    VK_FORMAT_R16G16B16A16_SFLOAT,
	    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	upsampling_view = m_context->create_texture_view(
	    "DI Upsampling Output View",
	    upsampling_image.vk_image,
	    VK_FORMAT_R16G16B16A16_SFLOAT);

	update_descriptor();
	init();
}

void RayTracedDI::update_descriptor()
{
	m_context->update_descriptor()
	    .write_storage_buffers(0, {temporal_reservoir_buffer.vk_buffer})
	    .write_storage_buffers(1, {passthrough_reservoir_buffer.vk_buffer})
	    .update(m_raytrace.temporal.descriptor_set);

	m_context->update_descriptor()
	    .write_storage_buffers(0, {spatial_reservoir_buffer.vk_buffer})
	    .write_storage_buffers(1, {passthrough_reservoir_buffer.vk_buffer})
	    .update(m_raytrace.spatial.descriptor_set);

	m_context->update_descriptor()
	    .write_storage_buffers(0, {temporal_reservoir_buffer.vk_buffer})
	    .write_storage_buffers(1, {spatial_reservoir_buffer.vk_buffer})
	    .write_storage_images(2, {raytraced_view})
	    .update(m_raytrace.composite.descriptor_set);

	for (uint32_t i = 0; i < 2; i++)
	{
		m_context->update_descriptor()
		    .write_storage_images(0, {reprojection_output_view[i]})
		    .write_storage_images(1, {reprojection_moment_view[i]})
		    .write_sampled_images(2, {raytraced_view})
		    .write_sampled_images(3, {reprojection_output_view[!i]})
		    .write_sampled_images(4, {reprojection_moment_view[!i]})
		    .write_storage_buffers(5, {denoise_tile_data_buffer.vk_buffer})
		    .write_storage_buffers(6, {denoise_tile_dispatch_args_buffer.vk_buffer})
		    .write_storage_buffers(7, {copy_tile_data_buffer.vk_buffer})
		    .write_storage_buffers(8, {copy_tile_dispatch_args_buffer.vk_buffer})
		    .update(m_reprojection.descriptor_sets[i]);

		m_context->update_descriptor()
		    .write_storage_images(0, {a_trous_view[0]})
		    .write_sampled_images(1, {reprojection_output_view[i]})
		    .write_storage_buffers(2, {copy_tile_data_buffer.vk_buffer})
		    .update(m_denoise.copy_tiles.copy_reprojection_sets[i]);

		m_context->update_descriptor()
		    .write_storage_images(0, {a_trous_view[i]})
		    .write_sampled_images(1, {a_trous_view[!i]})
		    .write_storage_buffers(2, {copy_tile_data_buffer.vk_buffer})
		    .update(m_denoise.copy_tiles.copy_atrous_sets[i]);

		m_context->update_descriptor()
		    .write_storage_images(0, {a_trous_view[0]})
		    .write_sampled_images(1, {reprojection_output_view[i]})
		    .write_storage_buffers(2, {denoise_tile_data_buffer.vk_buffer})
		    .update(m_denoise.a_trous.filter_reprojection_sets[i]);

		m_context->update_descriptor()
		    .write_storage_images(0, {a_trous_view[i]})
		    .write_sampled_images(1, {a_trous_view[!i]})
		    .write_storage_buffers(2, {denoise_tile_data_buffer.vk_buffer})
		    .update(m_denoise.a_trous.filter_atrous_sets[i]);
	}

	m_context->update_descriptor()
	    .write_storage_images(0, {upsampling_view})
	    .write_sampled_images(1, {a_trous_view[0]})
	    .update(m_upsampling.descriptor_set);

	m_context->update_descriptor()
	    .write_sampled_images(0, {upsampling_view})
	    .update(descriptor.set);
}

void RayTracedDI::destroy_resource()
{
	m_context->destroy(raytraced_image)
	    .destroy(raytraced_view)
	    .destroy(reprojection_output_image)
	    .destroy(reprojection_output_view)
	    .destroy(reprojection_moment_image)
	    .destroy(reprojection_moment_view)
	    .destroy(a_trous_image)
	    .destroy(a_trous_view)
	    .destroy(upsampling_image)
	    .destroy(upsampling_view)
	    .destroy(temporal_reservoir_buffer)
	    .destroy(passthrough_reservoir_buffer)
	    .destroy(spatial_reservoir_buffer)
	    .destroy(denoise_tile_data_buffer)
	    .destroy(copy_tile_data_buffer)
	    .destroy(denoise_tile_dispatch_args_buffer)
	    .destroy(copy_tile_dispatch_args_buffer);
}
