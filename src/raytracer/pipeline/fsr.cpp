#include "pipeline/fsr.hpp"

#include <imgui.h>

#define NUM_THREADS_X 16
#define NUM_THREADS_Y 16

#define A_CPU
#include "ffx_a.h"
#include "ffx_fsr1.h"

static unsigned char g_fsr1_fp16_easu[] = {
#include "fsr1_fp16_easu.comp.spv.h"
};

static unsigned char g_fsr1_fp16_rcas[] = {
#include "fsr1_fp16_rcas.comp.spv.h"
};

static unsigned char g_fsr1_fp32_easu[] = {
#include "fsr1_fp32_easu.comp.spv.h"
};

static unsigned char g_fsr1_fp32_rcas[] = {
#include "fsr1_fp32_rcas.comp.spv.h"
};

inline static size_t pad_uniform_buffer_size(const Context &context, size_t originalSize)
{
	// Calculate required alignment based on minimum device offset alignment
	size_t minUboAlignment = context.physical_device_properties.limits.minUniformBufferOffsetAlignment;
	size_t alignedSize     = originalSize;
	if (minUboAlignment > 0)
	{
		alignedSize = (alignedSize + minUboAlignment - 1) & ~(minUboAlignment - 1);
	}
	return alignedSize;
}

FSR1Pass::FSR1Pass(const Context &context, const Tonemap &tonemap) :
    m_context(&context)
{
	upsampled_image = m_context->create_texture_2d(
	    "FSR upsampled Image",
	    m_context->extent.width, m_context->extent.height,
	    VK_FORMAT_R16G16B16A16_SFLOAT,
	    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	upsampled_image_view = m_context->create_texture_view(
	    "FSR upsampled View",
	    upsampled_image.vk_image,
	    VK_FORMAT_R16G16B16A16_SFLOAT);
	intermediate_image = m_context->create_texture_2d(
	    "FSR intermediate Image",
	    m_context->extent.width, m_context->extent.height,
	    VK_FORMAT_R16G16B16A16_SFLOAT,
	    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	intermediate_image_view = m_context->create_texture_view(
	    "FSR intermediate View",
	    intermediate_image.vk_image,
	    VK_FORMAT_R16G16B16A16_SFLOAT);

	sampler = m_context->create_sampler(
	    VK_FILTER_LINEAR, VK_FILTER_LINEAR,
	    VK_SAMPLER_MIPMAP_MODE_NEAREST,
	    VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

	m_fsr_params_buffer = m_context->create_buffer(
	    "FSR easu and rcas parameter Buffer",
	    pad_uniform_buffer_size(context, sizeof(FSRPassUniforms)) * 2,
	    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	    VMA_MEMORY_USAGE_CPU_TO_GPU);

	m_descriptor_set_layout = m_context->create_descriptor_layout()
	                              // Uniform buffers for easu & rcas; offseted differently for each
	                              .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
	                              // Input Image
	                              .add_descriptor_binding(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                              // Output Image
	                              .add_descriptor_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                              // Input Sampler
	                              .add_descriptor_binding(3, VK_DESCRIPTOR_TYPE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT)
	                              .create();
	m_rcas_descriptor_set = m_context->allocate_descriptor_set(m_descriptor_set_layout);
	m_easu_descriptor_set = m_context->allocate_descriptor_set(m_descriptor_set_layout);
	m_pipeline_layout     = m_context->create_pipeline_layout({m_descriptor_set_layout});

	if (m_context->FsrFp16Enabled)
	{
		m_pipeline_easu = m_context->create_compute_pipeline(reinterpret_cast<uint32_t *>(g_fsr1_fp16_easu), sizeof(g_fsr1_fp16_easu), m_pipeline_layout);
		m_pipeline_rcas = m_context->create_compute_pipeline(reinterpret_cast<uint32_t *>(g_fsr1_fp16_rcas), sizeof(g_fsr1_fp16_rcas), m_pipeline_layout);
	}
	else
	{
		m_pipeline_easu = m_context->create_compute_pipeline(reinterpret_cast<uint32_t *>(g_fsr1_fp32_rcas), sizeof(g_fsr1_fp32_rcas), m_pipeline_layout);
		m_pipeline_rcas = m_context->create_compute_pipeline(reinterpret_cast<uint32_t *>(g_fsr1_fp32_rcas), sizeof(g_fsr1_fp32_rcas), m_pipeline_layout);
	}

	m_context->update_descriptor()
	    // FSR Uniform
	    .write_uniform_buffers(0, {m_fsr_params_buffer.vk_buffer})
	    // Input for easu
	    .write_sampled_images(1, {tonemap.render_target_view})
	    // Output for easu
	    .write_storage_images(2, {intermediate_image_view})
		// Sampler
	    .write_samplers(3, {sampler})
	    .update(m_easu_descriptor_set);

	m_context->update_descriptor()
	    // FSR Uniform
	    .write_uniform_buffers(0, {m_fsr_params_buffer.vk_buffer})
	    // Input for rcas
	    .write_sampled_images(1, {intermediate_image_view})
	    // Output for rcas
	    .write_storage_images(2, {upsampled_image_view})
	    // Sampler
	    .write_samplers(3, {sampler})
	    .update(m_rcas_descriptor_set);

	{
		memset(&m_easu_buffer_data, 0, sizeof(FSRPassUniforms));
		memset(&m_rcas_buffer_data, 0, sizeof(FSRPassUniforms));

		FsrEasuCon(
		    reinterpret_cast<AU1 *>(&m_easu_buffer_data.Const0),
		    reinterpret_cast<AU1 *>(&m_easu_buffer_data.Const1),
		    reinterpret_cast<AU1 *>(&m_easu_buffer_data.Const2),
		    reinterpret_cast<AU1 *>(&m_easu_buffer_data.Const3),
		    // Input sizes
		    static_cast<AF1>(m_context->render_extent.width),
		    static_cast<AF1>(m_context->render_extent.height),
		    static_cast<AF1>(m_context->render_extent.width),
		    static_cast<AF1>(m_context->render_extent.height),
		    // Output sizes
		    (AF1) m_context->extent.width, (AF1) m_context->extent.height);
		// fsr sample: (hdr && !pState->bUseRcas) ? 1 : 0;
		m_easu_buffer_data.Sample[0] = (m_isHDR && !m_useRCAS) ? 1 : 0;

		FsrRcasCon(reinterpret_cast<AU1 *>(&m_rcas_buffer_data.Const0), m_rcasAttenuation);
		// hdr ? 1 : 0
		m_rcas_buffer_data.Sample[0] = (m_isHDR ? 1 : 0);

		// initiate a transfer to uniform buffer
		std::vector<char> paddedData(pad_uniform_buffer_size(*m_context, sizeof(FSRPassUniforms)) * 2);
		std::memcpy(paddedData.data(), &m_easu_buffer_data, sizeof(FSRPassUniforms));
		std::memcpy(paddedData.data() + pad_uniform_buffer_size(*m_context, sizeof(FSRPassUniforms)), &m_rcas_buffer_data, sizeof(FSRPassUniforms));

		m_context->buffer_copy_to_device(m_fsr_params_buffer, paddedData.data(), paddedData.size());
	}

	init();
}

FSR1Pass::~FSR1Pass()
{
	m_context->destroy(upsampled_image)
	    .destroy(upsampled_image_view)
	    .destroy(intermediate_image)
	    .destroy(intermediate_image_view)
	    .destroy(sampler)
	    .destroy(m_fsr_params_buffer)
	    .destroy(m_descriptor_set_layout)
	    .destroy(m_rcas_descriptor_set)
	    .destroy(m_easu_descriptor_set)
	    .destroy(m_pipeline_layout)
	    .destroy(m_pipeline_easu)
	    .destroy(m_pipeline_rcas);
}

void FSR1Pass::init()
{
	m_context->record_command()
	    .begin()
	    .insert_barrier()
	    .add_image_barrier(
	        upsampled_image.vk_image,
	        0, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        intermediate_image.vk_image,
	        0, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)
	    .insert()
	    .end()
	    .flush();
}

void FSR1Pass::draw(CommandBufferRecorder &recorder)
{
	recorder
	    .begin_marker("FSR")
	    .insert_barrier()
	    .add_image_barrier(
	        intermediate_image.vk_image,
	        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        upsampled_image.vk_image,
	        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)
	    .insert(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
	    .begin_marker("FSR EASU")
	    .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline_layout, {m_easu_descriptor_set})
	    .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline_easu)
	    .dispatch({m_context->extent.width, m_context->extent.height, 1}, {NUM_THREADS_X, NUM_THREADS_Y, 1})
	    .end_marker()
	    .insert_barrier()
	    .add_image_barrier(
	        intermediate_image.vk_image,
	        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .insert(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
	    .begin_marker("FSR RCAS")
	    .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline_layout, {m_rcas_descriptor_set})
	    .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline_rcas)
	    .dispatch({m_context->extent.width, m_context->extent.height, 1}, {NUM_THREADS_X, NUM_THREADS_Y, 1})
	    .end_marker()
	    .end_marker();
}

bool FSR1Pass::draw_ui()
{
	bool update = false;
	if (ImGui::TreeNode("FSR"))
	{
		ImGui::Text("Upscaled factor: %.2f", (float) m_context->extent.height / m_context->render_extent.height);
		ImGui::Text("-> Render resolution: (%d, %d)", m_context->render_extent.width, m_context->render_extent.height);
		ImGui::Text("-> Display resolution: (%d, %d)", m_context->extent.width, m_context->extent.height);
		ImGui::Text("RCAS attentuation: %f", m_rcasAttenuation);
		ImGui::TreePop();
	}
	return update;
}
