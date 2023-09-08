#include "pipeline/raytrace_gi.hpp"

#include <glm/gtc/quaternion.hpp>

#include <imgui.h>

static const uint32_t NUM_THREADS_X = 8;
static const uint32_t NUM_THREADS_Y = 8;

static unsigned char g_gi_raytraced_comp_spv_data[] = {
#include "gi_raytraced.comp.spv.h"
};

static unsigned char g_gi_probe_update_irradiance_comp_spv_data[] = {
#include "gi_probe_update_irradiance.comp.spv.h"
};

static unsigned char g_gi_probe_update_depth_comp_spv_data[] = {
#include "gi_probe_update_depth.comp.spv.h"
};

static unsigned char g_gi_border_update_irradiance_comp_spv_data[] = {
#include "gi_border_update_irradiance.comp.spv.h"
};

static unsigned char g_gi_border_update_depth_comp_spv_data[] = {
#include "gi_border_update_depth.comp.spv.h"
};

static unsigned char g_gi_probe_sample_comp_spv_data[] = {
#include "gi_probe_sample.comp.spv.h"
};

// static unsigned char g_gi_probe_visualize_vert_spv_data[] = {
// #include "gi_probe_visualize.vert.spv.h"
// };
//
// static unsigned char g_gi_probe_visualize_frag_spv_data[] = {
// #include "gi_probe_visualize.frag.spv.h"
// };

RayTracedGI::RayTracedGI(const Context &context, const Scene &scene, const GBufferPass &gbuffer_pass, RayTracedScale scale) :
    m_context(&context)
{
	float scale_divisor = std::powf(2.0f, float(scale));

	m_width  = m_context->render_extent.width / static_cast<uint32_t>(scale_divisor);
	m_height = m_context->render_extent.height / static_cast<uint32_t>(scale_divisor);

	m_gbuffer_mip = static_cast<uint32_t>(scale);

	std::random_device random_device;
	m_random_generator = std::mt19937(random_device());
	m_random_distrib   = std::uniform_real_distribution<float>(0.0f, 1.0f);

	m_raytraced.descriptor_set_layout = m_context->create_descriptor_layout()
	                                        // DDGI uniform buffer
	                                        .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
	                                        // Radiance
	                                        .add_descriptor_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                                        // Direction Depth
	                                        .add_descriptor_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                                        // Probe Irradiance
	                                        .add_descriptor_binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT)
	                                        // Probe Depth
	                                        .add_descriptor_binding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT)
	                                        .create();
	m_raytraced.descriptor_sets = m_context->allocate_descriptor_sets<2>(m_raytraced.descriptor_set_layout);
	m_raytraced.pipeline_layout = m_context->create_pipeline_layout({
	                                                                    scene.glsl_descriptor.layout,
	                                                                    gbuffer_pass.glsl_descriptor.layout,
	                                                                    m_raytraced.descriptor_set_layout,
	                                                                },
	                                                                sizeof(m_raytraced.push_constants), VK_SHADER_STAGE_COMPUTE_BIT);
	m_raytraced.pipeline        = m_context->create_compute_pipeline((uint32_t *) g_gi_raytraced_comp_spv_data, sizeof(g_gi_raytraced_comp_spv_data), m_raytraced.pipeline_layout);

	m_probe_update.update_probe.descriptor_set_layout = m_context->create_descriptor_layout()
	                                                        // Output irradiance
	                                                        .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                                                        // Output depth
	                                                        .add_descriptor_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                                                        // Input irradiance
	                                                        .add_descriptor_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT)
	                                                        // Input depth
	                                                        .add_descriptor_binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT)
	                                                        // Input radiance
	                                                        .add_descriptor_binding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT)
	                                                        // Input direction depth
	                                                        .add_descriptor_binding(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT)
	                                                        // DDGI uniform buffer
	                                                        .add_descriptor_binding(6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
	                                                        // Probe data buffer
	                                                        .add_descriptor_binding(7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT)
	                                                        .create();
	m_probe_update.update_probe.descriptor_sets     = m_context->allocate_descriptor_sets<2>(m_probe_update.update_probe.descriptor_set_layout);
	m_probe_update.update_probe.pipeline_layout     = m_context->create_pipeline_layout({
                                                                                        m_probe_update.update_probe.descriptor_set_layout,
                                                                                    },
	                                                                                    sizeof(m_probe_update.update_probe.push_constants), VK_SHADER_STAGE_COMPUTE_BIT);
	m_probe_update.update_probe.irradiance_pipeline = m_context->create_compute_pipeline((uint32_t *) g_gi_probe_update_irradiance_comp_spv_data, sizeof(g_gi_probe_update_irradiance_comp_spv_data), m_probe_update.update_probe.pipeline_layout);
	m_probe_update.update_probe.depth_pipeline      = m_context->create_compute_pipeline((uint32_t *) g_gi_probe_update_depth_comp_spv_data, sizeof(g_gi_probe_update_depth_comp_spv_data), m_probe_update.update_probe.pipeline_layout);

	m_probe_update.update_border.descriptor_set_layout = m_context->create_descriptor_layout()
	                                                         // Output irradiance
	                                                         .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                                                         // Output depth
	                                                         .add_descriptor_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                                                         .create();
	m_probe_update.update_border.descriptor_sets     = m_context->allocate_descriptor_sets<2>(m_probe_update.update_border.descriptor_set_layout);
	m_probe_update.update_border.pipeline_layout     = m_context->create_pipeline_layout({m_probe_update.update_border.descriptor_set_layout});
	m_probe_update.update_border.irradiance_pipeline = m_context->create_compute_pipeline((uint32_t *) g_gi_border_update_irradiance_comp_spv_data, sizeof(g_gi_border_update_irradiance_comp_spv_data), m_probe_update.update_border.pipeline_layout);
	m_probe_update.update_border.depth_pipeline      = m_context->create_compute_pipeline((uint32_t *) g_gi_border_update_depth_comp_spv_data, sizeof(g_gi_border_update_depth_comp_spv_data), m_probe_update.update_border.pipeline_layout);

	m_probe_sample.descriptor_set_layout = m_context->create_descriptor_layout()
	                                           // DDGI buffer
	                                           .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
	                                           // Probe irradiance
	                                           .add_descriptor_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT)
	                                           // Probe depth
	                                           .add_descriptor_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT)
	                                           // Output GI
	                                           .add_descriptor_binding(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                                           // Probe data
	                                           .add_descriptor_binding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT)
	                                           .create();
	m_probe_sample.descriptor_sets = m_context->allocate_descriptor_sets<2>(m_probe_sample.descriptor_set_layout);
	m_probe_sample.pipeline_layout = m_context->create_pipeline_layout({
	                                                                       scene.glsl_descriptor.layout,
	                                                                       gbuffer_pass.glsl_descriptor.layout,
	                                                                       m_probe_sample.descriptor_set_layout,
	                                                                   },
	                                                                   sizeof(m_probe_sample.push_constants), VK_SHADER_STAGE_COMPUTE_BIT);
	m_probe_sample.pipeline        = m_context->create_compute_pipeline((uint32_t *) g_gi_probe_sample_comp_spv_data, sizeof(g_gi_probe_sample_comp_spv_data), m_probe_sample.pipeline_layout);

	bind_descriptor.layout = m_context->create_descriptor_layout()
	                             .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                             .create();
	bind_descriptor.set = m_context->allocate_descriptor_set(bind_descriptor.layout);
}

RayTracedGI::~RayTracedGI()
{
	destroy_resource();

	m_context->destroy(bind_descriptor.layout)
	    .destroy(bind_descriptor.set);

	m_context->destroy(m_raytraced.pipeline)
	    .destroy(m_raytraced.pipeline_layout)
	    .destroy(m_raytraced.descriptor_set_layout)
	    .destroy(m_raytraced.descriptor_sets)
	    .destroy(m_probe_update.update_probe.irradiance_pipeline)
	    .destroy(m_probe_update.update_probe.depth_pipeline)
	    .destroy(m_probe_update.update_probe.pipeline_layout)
	    .destroy(m_probe_update.update_probe.descriptor_set_layout)
	    .destroy(m_probe_update.update_probe.descriptor_sets)

	    .destroy(m_probe_update.update_border.irradiance_pipeline)
	    .destroy(m_probe_update.update_border.depth_pipeline)
	    .destroy(m_probe_update.update_border.pipeline_layout)
	    .destroy(m_probe_update.update_border.descriptor_set_layout)
	    .destroy(m_probe_update.update_border.descriptor_sets)

	    .destroy(m_probe_sample.pipeline)
	    .destroy(m_probe_sample.pipeline_layout)
	    .destroy(m_probe_sample.descriptor_set_layout)
	    .destroy(m_probe_sample.descriptor_sets)

	    .destroy(descriptor.layout)
	    .destroy(descriptor.sets)

	    ;
}

void RayTracedGI::init()
{
	m_context->record_command()
	    .begin()
	    .insert_barrier()
	    .add_buffer_barrier(
	        uniform_buffer.vk_buffer,
	        0, VK_ACCESS_SHADER_READ_BIT)
	    .add_image_barrier(
	        radiance_image.vk_image,
	        0, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        direction_depth_image.vk_image,
	        0, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        probe_grid_irradiance_image[0].vk_image,
	        0, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .add_image_barrier(
	        probe_grid_depth_image[0].vk_image,
	        0, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .add_image_barrier(
	        probe_grid_irradiance_image[1].vk_image,
	        0, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        probe_grid_depth_image[1].vk_image,
	        0, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        sample_probe_grid_image.vk_image,
	        0, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .insert()
	    .end()
	    .flush();
}

void RayTracedGI::update(const Scene &scene)
{
	glm::vec3 min_extent = scene.scene_info.min_extent;
	glm::vec3 max_extent = scene.scene_info.max_extent;

	if (m_scene_min_extent != min_extent ||
	    m_scene_max_extent != max_extent)
	{
		m_init = true;

		m_scene_min_extent = min_extent;
		m_scene_max_extent = max_extent;

		glm::vec3 scene_length = max_extent - min_extent;

		m_probe_update.params.probe_count  = glm::ivec3(scene_length / m_probe_update.params.probe_distance) + glm::ivec3(2);
		m_probe_update.params.grid_start   = min_extent;
		m_probe_update.params.max_distance = m_probe_update.params.probe_distance * 1.5f;

		create_resource();
	}

	m_context->update_descriptor()
	    .write_sampled_images(0, {sample_probe_grid_view})
	    .update(bind_descriptor.set);

	for (uint32_t i = 0; i < 2; i++)
	{
		m_context->update_descriptor()
		    .write_uniform_buffers(0, {uniform_buffer.vk_buffer})
		    .write_storage_images(1, {radiance_view})
		    .write_storage_images(2, {direction_depth_view})
		    .write_combine_sampled_images(3, scene.linear_sampler, {probe_grid_irradiance_view[i]})
		    .write_combine_sampled_images(4, scene.linear_sampler, {probe_grid_depth_view[i]})
		    .update(m_raytraced.descriptor_sets[i]);

		m_context->update_descriptor()
		    .write_storage_images(0, {probe_grid_irradiance_view[!i]})
		    .write_storage_images(1, {probe_grid_depth_view[!i]})
		    .write_combine_sampled_images(2, scene.linear_sampler, {probe_grid_irradiance_view[i]})
		    .write_combine_sampled_images(3, scene.linear_sampler, {probe_grid_depth_view[i]})
		    .write_combine_sampled_images(4, scene.linear_sampler, {radiance_view})
		    .write_combine_sampled_images(5, scene.linear_sampler, {direction_depth_view})
		    .write_uniform_buffers(6, {uniform_buffer.vk_buffer})
		    .update(m_probe_update.update_probe.descriptor_sets[i]);

		m_context->update_descriptor()
		    .write_storage_images(0, {probe_grid_irradiance_view[!i]})
		    .write_storage_images(1, {probe_grid_depth_view[!i]})
		    .update(m_probe_update.update_border.descriptor_sets[i]);

		m_context->update_descriptor()
		    .write_uniform_buffers(0, {uniform_buffer.vk_buffer})
		    .write_combine_sampled_images(1, scene.linear_sampler, {probe_grid_irradiance_view[!i]})
		    .write_combine_sampled_images(2, scene.linear_sampler, {probe_grid_depth_view[!i]})
		    .write_storage_images(3, {sample_probe_grid_view})
		    .update(m_probe_sample.descriptor_sets[i]);
	}
}

void RayTracedGI::draw(CommandBufferRecorder &recorder, const Scene &scene, const GBufferPass &gbuffer_pass)
{
	UBO ubo = {
	    .grid_start                   = m_probe_update.params.grid_start + m_probe_update.params.grid_offset,
	    .max_distance                 = m_probe_update.params.max_distance,
	    .grid_step                    = glm::vec3(m_probe_update.params.probe_distance),
	    .depth_sharpness              = m_probe_update.params.depth_sharpness,
	    .probe_count                  = m_probe_update.params.probe_count,
	    .hysteresis                   = m_probe_update.params.hysteresis,
	    .normal_bias                  = m_probe_update.params.normal_bias,
	    .energy_preservation          = m_probe_update.params.recursive_energy_preservation,
	    .rays_per_probe               = static_cast<uint32_t>(m_raytraced.params.rays_per_probe),
	    .visibility_test              = true,
	    .irradiance_probe_side_length = m_probe_update.params.irradiance_oct_size,
	    .irradiance_texture_width     = m_probe_update.params.irradiance_width,
	    .irradiance_texture_height    = m_probe_update.params.irradiance_height,
	    .depth_probe_side_length      = m_probe_update.params.depth_oct_size,
	    .depth_texture_width          = m_probe_update.params.depth_width,
	    .depth_texture_height         = m_probe_update.params.depth_height,
	};

	uint32_t total_probes = m_probe_update.params.probe_count.x * m_probe_update.params.probe_count.y * m_probe_update.params.probe_count.z;

	m_raytraced.push_constants.random_orientation = glm::mat4_cast(glm::angleAxis(m_random_distrib(m_random_generator) * (glm::pi<float>() * 2.0f), glm::normalize(glm::vec3(m_random_distrib(m_random_generator), m_random_distrib(m_random_generator), m_random_distrib(m_random_generator)))));
	m_raytraced.push_constants.num_frames         = m_frame_count;
	m_raytraced.push_constants.infinite_bounces   = m_raytraced.params.infinite_bounces && m_frame_count == 0 ? 0u : 1u;
	m_raytraced.push_constants.gi_intensity       = m_raytraced.params.infinite_bounce_intensity;

	m_probe_update.update_probe.push_constants.frame_count = m_frame_count;

	m_probe_sample.push_constants.gbuffer_mip  = m_gbuffer_mip;
	m_probe_sample.push_constants.gi_intensity = m_probe_sample.params.gi_intensity;

	{
		recorder
		    .begin_marker("RayTraced GI")
		    .update_buffer(uniform_buffer.vk_buffer, &ubo, sizeof(UBO))
		    .insert_barrier()
		    .add_buffer_barrier(uniform_buffer.vk_buffer, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
		    .insert(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
		    .begin_marker("Ray Traced")
		    .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_raytraced.pipeline_layout,
		                         {
		                             scene.glsl_descriptor.set,
		                             gbuffer_pass.glsl_descriptor.sets[m_context->ping_pong],
		                             m_raytraced.descriptor_sets[m_context->ping_pong],
		                         })
		    .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_raytraced.pipeline)
		    .push_constants(m_raytraced.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, m_raytraced.push_constants)
		    .dispatch({(float) m_raytraced.params.rays_per_probe, (float) total_probes, 1}, {NUM_THREADS_X, NUM_THREADS_Y, 1})
		    .end_marker()
		    .insert_barrier()
		    .add_image_barrier(
		        radiance_image.vk_image,
		        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
		        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		    .add_image_barrier(
		        direction_depth_image.vk_image,
		        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
		        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		    .add_image_barrier(
		        sample_probe_grid_image.vk_image,
		        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
		        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
		    .insert()

		    .begin_marker("Probe Update")
		    .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_probe_update.update_probe.pipeline_layout, {m_probe_update.update_probe.descriptor_sets[m_context->ping_pong]})
		    .push_constants(m_probe_update.update_probe.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, m_probe_update.update_probe.push_constants)
		    .begin_marker("Update Irradiance")
		    .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_probe_update.update_probe.irradiance_pipeline)
		    .dispatch({m_probe_update.params.probe_count.x * m_probe_update.params.probe_count.y, m_probe_update.params.probe_count.z, 1}, {1, 1, 1})
		    .end_marker()
		    .begin_marker("Update Depth Direction")
		    .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_probe_update.update_probe.depth_pipeline)
		    .dispatch({m_probe_update.params.probe_count.x * m_probe_update.params.probe_count.y, m_probe_update.params.probe_count.z, 1}, {1, 1, 1})
		    .end_marker()

		    .insert_barrier()
		    .add_image_barrier(
		        probe_grid_irradiance_image[!m_context->ping_pong].vk_image,
		        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
		        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL)
		    .add_image_barrier(
		        probe_grid_depth_image[!m_context->ping_pong].vk_image,
		        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
		        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL)
		    .insert(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)

		    .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_probe_update.update_border.pipeline_layout, {m_probe_update.update_border.descriptor_sets[m_context->ping_pong]})
		    .begin_marker("Update Irradiance")
		    .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_probe_update.update_border.irradiance_pipeline)
		    .dispatch({m_probe_update.params.probe_count.x * m_probe_update.params.probe_count.y, m_probe_update.params.probe_count.z, 1}, {1, 1, 1})
		    .end_marker()
		    .begin_marker("Update Depth Direction")
		    .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_probe_update.update_border.depth_pipeline)
		    .dispatch({m_probe_update.params.probe_count.x * m_probe_update.params.probe_count.y, m_probe_update.params.probe_count.z, 1}, {1, 1, 1})
		    .end_marker()

		    .end_marker()

		    .insert_barrier()
		    .add_image_barrier(
		        probe_grid_irradiance_image[!m_context->ping_pong].vk_image,
		        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
		        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		    .add_image_barrier(
		        probe_grid_depth_image[!m_context->ping_pong].vk_image,
		        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
		        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		    .insert(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)

		    .begin_marker("Sample Probe Grid")
		    .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_probe_sample.pipeline_layout,
		                         {
		                             scene.glsl_descriptor.set,
		                             gbuffer_pass.glsl_descriptor.sets[m_context->ping_pong],
		                             m_probe_sample.descriptor_sets[m_context->ping_pong],
		                         })
		    .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_probe_sample.pipeline)
		    .push_constants(m_probe_sample.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, m_probe_sample.push_constants)
		    .dispatch({m_width, m_height, 1}, {NUM_THREADS_X, NUM_THREADS_Y, 1})
		    .end_marker()

		    .insert_barrier()
		    .add_buffer_barrier(uniform_buffer.vk_buffer, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT)
		    .add_image_barrier(
		        probe_grid_irradiance_image[m_context->ping_pong].vk_image,
		        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
		        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
		    .add_image_barrier(
		        probe_grid_depth_image[m_context->ping_pong].vk_image,
		        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
		        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
		    .add_image_barrier(
		        radiance_image.vk_image,
		        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
		        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
		    .add_image_barrier(
		        direction_depth_image.vk_image,
		        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
		        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
		    .add_image_barrier(
		        sample_probe_grid_image.vk_image,
		        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
		        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		    .insert(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)

		    .end_marker();
	}

	m_frame_count++;
}

bool RayTracedGI::draw_ui()
{
	if (ImGui::TreeNode("Ray Trace GI"))
	{
		ImGui::Text("Probe Grid Size: [%i, %i, %i]",
		            m_probe_update.params.probe_count.x,
		            m_probe_update.params.probe_count.y,
		            m_probe_update.params.probe_count.z);
		ImGui::Checkbox("Visibility Test", &m_probe_update.params.visibility_test);
		ImGui::Checkbox("Infinite Bounce", reinterpret_cast<bool *>(&m_raytraced.params.infinite_bounces));
		ImGui::SliderFloat("Normal Bias", &m_probe_update.params.normal_bias, 0.0f, 1.0f, "%.3f");
		ImGui::DragFloat3("Grid Offset", &m_probe_update.params.grid_offset.x, 0.01f, -10.f, 10.f);
		ImGui::SliderFloat("Infinite Bounce Intensity", &m_raytraced.params.infinite_bounce_intensity, 0.0f, 10.0f);
		ImGui::SliderFloat("GI Intensity", &m_probe_sample.params.gi_intensity, 0.0f, 10.0f);
		ImGui::TreePop();
	}

	return false;
}

void RayTracedGI::create_resource()
{
	vkDeviceWaitIdle(m_context->vk_device);

	m_frame_count = 0;

	destroy_resource();

	uint32_t total_probes = m_probe_update.params.probe_count.x * m_probe_update.params.probe_count.y * m_probe_update.params.probe_count.z;

	radiance_image = m_context->create_texture_2d(
	    "GI Radiance Image",
	    (uint32_t) m_raytraced.params.rays_per_probe, total_probes,
	    VK_FORMAT_R16G16B16A16_SFLOAT,
	    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	radiance_view = m_context->create_texture_view(
	    "GI Radiance View",
	    radiance_image.vk_image,
	    VK_FORMAT_R16G16B16A16_SFLOAT);

	direction_depth_image = m_context->create_texture_2d(
	    "GI Direction Depth Image",
	    (uint32_t) m_raytraced.params.rays_per_probe, total_probes,
	    VK_FORMAT_R16G16B16A16_SFLOAT,
	    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	direction_depth_view = m_context->create_texture_view(
	    "GI Direction Depth View",
	    direction_depth_image.vk_image,
	    VK_FORMAT_R16G16B16A16_SFLOAT);

	{
		m_probe_update.params.irradiance_width  = (m_probe_update.params.irradiance_oct_size + 2) * m_probe_update.params.probe_count.x * m_probe_update.params.probe_count.y + 2;
		m_probe_update.params.irradiance_height = (m_probe_update.params.irradiance_oct_size + 2) * m_probe_update.params.probe_count.z + 2;

		for (uint32_t i = 0; i < 2; i++)
		{
			probe_grid_irradiance_image[i] = m_context->create_texture_2d(
			    "GI Probe Grid Irradiance Image",
			    m_probe_update.params.irradiance_width, m_probe_update.params.irradiance_height,
			    VK_FORMAT_R16G16B16A16_SFLOAT,
			    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
			probe_grid_irradiance_view[i] = m_context->create_texture_view(
			    "GI Probe Grid Irradiance View",
			    probe_grid_irradiance_image[i].vk_image,
			    VK_FORMAT_R16G16B16A16_SFLOAT);
		}
	}

	{
		m_probe_update.params.depth_width  = (m_probe_update.params.depth_oct_size + 2) * m_probe_update.params.probe_count.x * m_probe_update.params.probe_count.y + 2;
		m_probe_update.params.depth_height = (m_probe_update.params.depth_oct_size + 2) * m_probe_update.params.probe_count.z + 2;

		for (uint32_t i = 0; i < 2; i++)
		{
			probe_grid_depth_image[i] = m_context->create_texture_2d(
			    "GI Probe Grid Depth Image",
			    m_probe_update.params.depth_width, m_probe_update.params.depth_height,
			    VK_FORMAT_R16G16B16A16_SFLOAT,
			    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
			probe_grid_depth_view[i] = m_context->create_texture_view(
			    "GI Probe Grid Depth View",
			    probe_grid_depth_image[i].vk_image,
			    VK_FORMAT_R16G16B16A16_SFLOAT);
		}
	}

	{
		sample_probe_grid_image = m_context->create_texture_2d(
		    "GI Sample Probe Grid Image",
		    m_width, m_height,
		    VK_FORMAT_R16G16B16A16_SFLOAT,
		    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
		sample_probe_grid_view = m_context->create_texture_view(
		    "GI Sample Probe Grid View",
		    sample_probe_grid_image.vk_image,
		    VK_FORMAT_R16G16B16A16_SFLOAT);
	}

	uniform_buffer = m_context->create_buffer("GI Uniform Buffer", sizeof(UBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

	init();
}

void RayTracedGI::destroy_resource()
{
	m_context->destroy(radiance_image)
	    .destroy(radiance_view)
	    .destroy(direction_depth_image)
	    .destroy(direction_depth_view)
	    .destroy(probe_grid_irradiance_image[0])
	    .destroy(probe_grid_irradiance_image[1])
	    .destroy(probe_grid_irradiance_view[0])
	    .destroy(probe_grid_irradiance_view[1])
	    .destroy(probe_grid_depth_image[0])
	    .destroy(probe_grid_depth_image[1])
	    .destroy(probe_grid_depth_view[0])
	    .destroy(probe_grid_depth_view[1])
	    .destroy(sample_probe_grid_image)
	    .destroy(sample_probe_grid_view)
	    .destroy(uniform_buffer);
}
