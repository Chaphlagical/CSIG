#include "render/pipeline/raytraced_shadow.hpp"

static const int RAY_TRACE_NUM_THREADS_X = 8;
static const int RAY_TRACE_NUM_THREADS_Y = 4;


RayTracedShadow::RayTracedShadow(const Context &context, RayTracedScale scale) :
    m_context(&context)
{
	float scale_divisor = powf(2.0f, float(scale));

	m_width  = static_cast<uint32_t>(static_cast<float>(context.extent.width) / scale_divisor);
	m_height = static_cast<uint32_t>(static_cast<float>(context.extent.height) / scale_divisor);

	m_gbuffer_mip = static_cast<uint32_t>(scale);

	// Create raytraced image
	{
		VkImageCreateInfo image_create_info = {
		    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		    .imageType     = VK_IMAGE_TYPE_2D,
		    .format        = VK_FORMAT_R32_UINT,
		    .extent        = VkExtent3D{static_cast<uint32_t>(ceil(float(m_width) / float(RAY_TRACE_NUM_THREADS_X))), static_cast<uint32_t>(ceil(float(m_height) / float(RAY_TRACE_NUM_THREADS_Y))), 1},
		    .mipLevels     = 1,
		    .arrayLayers   = 1,
		    .samples       = VK_SAMPLE_COUNT_1_BIT,
		    .tiling        = VK_IMAGE_TILING_OPTIMAL,
		    .usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		    .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
		    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};
		VmaAllocationCreateInfo allocation_create_info = {
		    .usage = VMA_MEMORY_USAGE_GPU_ONLY,
		};
		vmaCreateImage(context.vma_allocator, &image_create_info, &allocation_create_info, &raytraced_image.vk_image, &raytraced_image.vma_allocation, nullptr);
		VkImageViewCreateInfo view_create_info = {
		    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image            = raytraced_image.vk_image,
		    .viewType         = VK_IMAGE_VIEW_TYPE_2D,
		    .format           = VK_FORMAT_R32_UINT,
		    .components       = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
		    .subresourceRange = {
		        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		        .baseMipLevel   = 0,
		        .levelCount     = 1,
		        .baseArrayLayer = 0,
		        .layerCount     = 1,
		    },
		};
		vkCreateImageView(context.vk_device, &view_create_info, nullptr, &raytraced_image_view);
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) raytraced_image.vk_image, "RayTraced Image");
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) raytraced_image_view, "RayTraced Image View");
	}

	// Ray Traced Pass
	{

	}
}

RayTracedShadow::~RayTracedShadow()
{
}

void RayTracedShadow::init(VkCommandBuffer cmd_buffer)
{

}

void RayTracedShadow::update(const Scene &scene, const BlueNoise &blue_noise, const GBufferPass &gbuffer_pass)
{
}

void RayTracedShadow::draw(VkCommandBuffer cmd_buffer)
{
}

bool RayTracedShadow::draw_ui()
{
	return false;
}
