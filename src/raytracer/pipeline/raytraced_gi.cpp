#include "render/pipeline/raytraced_gi.hpp"

RayTracedGI::RayTracedGI(const Context &context, RayTracedScale scale) :
    m_context(&context)
{
	float scale_divisor = std::powf(2.0f, float(scale));

	m_width  = m_context->extent.width / static_cast<uint32_t>(scale_divisor);
	m_height = m_context->extent.height / static_cast<uint32_t>(scale_divisor);

	m_gbuffer_mip = static_cast<uint32_t>(scale);
}

RayTracedGI::~RayTracedGI()
{
	destroy_resource();
}

void RayTracedGI::init(VkCommandBuffer cmd_buffer)
{


}

void RayTracedGI::update(const Scene &scene, const BlueNoise &blue_noise, const GBufferPass &gbuffer_pass)
{
	glm::vec3 min_extent = scene.scene_info.min_extent;
	glm::vec3 max_extent = scene.scene_info.max_extent;

	if (m_scene_min_extent != min_extent ||
	    m_scene_max_extent != max_extent)
	{
		m_scene_min_extent = min_extent;
		m_scene_max_extent = max_extent;

		glm::vec3 scene_length = max_extent - min_extent;

		m_probe_update.params.probe_count  = glm::ivec3(scene_length / m_probe_update.params.probe_distance) + glm::ivec3(2);
		m_probe_update.params.grid_start   = min_extent;
		m_probe_update.params.max_distance = m_probe_update.params.probe_distance * 1.5f;

		create_resource();
	}
}

void RayTracedGI::draw(VkCommandBuffer cmd_buffer)
{
}

bool RayTracedGI::draw_ui()
{
	return false;
}

void RayTracedGI::create_resource()
{
	vkDeviceWaitIdle(m_context->vk_device);

	destroy_resource();

	uint32_t total_probes = m_probe_update.params.probe_count.x * m_probe_update.params.probe_count.y * m_probe_update.params.probe_count.z;

	// Radiance image
	{
		VkImageCreateInfo image_create_info = {
		    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		    .imageType     = VK_IMAGE_TYPE_2D,
		    .format        = VK_FORMAT_R16G16B16A16_SFLOAT,
		    .extent        = VkExtent3D{m_raytraced.params.rays_per_probe, total_probes, 1},
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
		vmaCreateImage(m_context->vma_allocator, &image_create_info, &allocation_create_info, &radiance_image.vk_image, &radiance_image.vma_allocation, nullptr);
		VkImageViewCreateInfo view_create_info = {
		    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image            = radiance_image.vk_image,
		    .viewType         = VK_IMAGE_VIEW_TYPE_2D,
		    .format           = VK_FORMAT_R16G16B16A16_SFLOAT,
		    .components       = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
		    .subresourceRange = {
		        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		        .baseMipLevel   = 0,
		        .levelCount     = 1,
		        .baseArrayLayer = 0,
		        .layerCount     = 1,
		    },
		};
		vkCreateImageView(m_context->vk_device, &view_create_info, nullptr, &radiance_view);
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) radiance_image.vk_image, "DDGI Radiance Image");
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) radiance_view, "DDGI Radiance View");
	}

	// Direction depth image
	{
		VkImageCreateInfo image_create_info = {
		    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		    .imageType     = VK_IMAGE_TYPE_2D,
		    .format        = VK_FORMAT_R16G16B16A16_SFLOAT,
		    .extent        = VkExtent3D{m_raytraced.params.rays_per_probe, total_probes, 1},
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
		vmaCreateImage(m_context->vma_allocator, &image_create_info, &allocation_create_info, &direction_depth_image.vk_image, &direction_depth_image.vma_allocation, nullptr);
		VkImageViewCreateInfo view_create_info = {
		    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image            = direction_depth_image.vk_image,
		    .viewType         = VK_IMAGE_VIEW_TYPE_2D,
		    .format           = VK_FORMAT_R16G16B16A16_SFLOAT,
		    .components       = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
		    .subresourceRange = {
		        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		        .baseMipLevel   = 0,
		        .levelCount     = 1,
		        .baseArrayLayer = 0,
		        .layerCount     = 1,
		    },
		};
		vkCreateImageView(m_context->vk_device, &view_create_info, nullptr, &direction_depth_view);
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) direction_depth_image.vk_image, "DDGI Direction Depth Image");
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) direction_depth_view, "DDGI Direction Depth View");
	}

	// Probe grid irradiance image
	{
		const uint32_t irradiance_width  = (m_probe_update.params.irradiance_oct_size + 2) * m_probe_update.params.probe_count.x * m_probe_update.params.probe_count.y + 2;
		const uint32_t irradiance_height = (m_probe_update.params.irradiance_oct_size + 2) * m_probe_update.params.probe_count.z + 2;

		for (uint32_t i = 0; i < 2; i++)
		{
			VkImageCreateInfo image_create_info = {
			    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			    .imageType     = VK_IMAGE_TYPE_2D,
			    .format        = VK_FORMAT_R16G16B16A16_SFLOAT,
			    .extent        = VkExtent3D{irradiance_width, irradiance_height, 1},
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
			vmaCreateImage(m_context->vma_allocator, &image_create_info, &allocation_create_info, &probe_grid_irradiance_image[i].vk_image, &probe_grid_irradiance_image[i].vma_allocation, nullptr);
			VkImageViewCreateInfo view_create_info = {
			    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			    .image            = probe_grid_irradiance_image[i].vk_image,
			    .viewType         = VK_IMAGE_VIEW_TYPE_2D,
			    .format           = VK_FORMAT_R16G16B16A16_SFLOAT,
			    .components       = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
			    .subresourceRange = {
			        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
			        .baseMipLevel   = 0,
			        .levelCount     = 1,
			        .baseArrayLayer = 0,
			        .layerCount     = 1,
			    },
			};
			vkCreateImageView(m_context->vk_device, &view_create_info, nullptr, &probe_grid_irradiance_view[i]);
			m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) probe_grid_irradiance_image[i].vk_image, (std::string("DDGI Probe Grid Irradiance Image - ") + std::to_string(i)).c_str());
			m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) probe_grid_irradiance_view[i], (std::string("DDGI Probe Grid Irradiance View - ") + std::to_string(i)).c_str());
		}
	}

	// Probe grid depth image
	{
		const uint32_t depth_width  = (m_probe_update.params.depth_oct_size + 2) * m_probe_update.params.probe_count.x * m_probe_update.params.probe_count.y + 2;
		const uint32_t depth_height = (m_probe_update.params.depth_oct_size + 2) * m_probe_update.params.probe_count.z + 2;

		for (uint32_t i = 0; i < 2; i++)
		{
			VkImageCreateInfo image_create_info = {
			    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			    .imageType     = VK_IMAGE_TYPE_2D,
			    .format        = VK_FORMAT_R16G16B16A16_SFLOAT,
			    .extent        = VkExtent3D{depth_width, depth_height, 1},
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
			vmaCreateImage(m_context->vma_allocator, &image_create_info, &allocation_create_info, &probe_grid_depth_image[i].vk_image, &probe_grid_depth_image[i].vma_allocation, nullptr);
			VkImageViewCreateInfo view_create_info = {
			    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			    .image            = probe_grid_depth_image[i].vk_image,
			    .viewType         = VK_IMAGE_VIEW_TYPE_2D,
			    .format           = VK_FORMAT_R16G16B16A16_SFLOAT,
			    .components       = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
			    .subresourceRange = {
			        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
			        .baseMipLevel   = 0,
			        .levelCount     = 1,
			        .baseArrayLayer = 0,
			        .layerCount     = 1,
			    },
			};
			vkCreateImageView(m_context->vk_device, &view_create_info, nullptr, &probe_grid_depth_view[i]);
			m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) probe_grid_depth_image[i].vk_image, (std::string("DDGI Probe Grid Depth Image - ") + std::to_string(i)).c_str());
			m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) probe_grid_depth_view[i], (std::string("DDGI Probe Grid Depth View - ") + std::to_string(i)).c_str());
		}
	}

	// Sample probe grid
	{
		VkImageCreateInfo image_create_info = {
		    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		    .imageType     = VK_IMAGE_TYPE_2D,
		    .format        = VK_FORMAT_R16G16B16A16_SFLOAT,
		    .extent        = VkExtent3D{m_width, m_height, 1},
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
		vmaCreateImage(m_context->vma_allocator, &image_create_info, &allocation_create_info, &sample_probe_grid_image.vk_image, &sample_probe_grid_image.vma_allocation, nullptr);
		VkImageViewCreateInfo view_create_info = {
		    .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image            = sample_probe_grid_image.vk_image,
		    .viewType         = VK_IMAGE_VIEW_TYPE_2D,
		    .format           = VK_FORMAT_R16G16B16A16_SFLOAT,
		    .components       = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
		    .subresourceRange = {
		        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		        .baseMipLevel   = 0,
		        .levelCount     = 1,
		        .baseArrayLayer = 0,
		        .layerCount     = 1,
		    },
		};
		vkCreateImageView(m_context->vk_device, &view_create_info, nullptr, &sample_probe_grid_view);
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE, (uint64_t) sample_probe_grid_image.vk_image, "DDGI Sample Probe Grid Image");
		m_context->set_object_name(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t) sample_probe_grid_view, "DDGI Sample Probe Grid View");
	}

	// Uniform buffer
	{
		VkBufferCreateInfo buffer_create_info = {
		    .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		    .size        = sizeof(m_ubo),
		    .usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};
		VmaAllocationCreateInfo allocation_create_info = {
		    .usage = VMA_MEMORY_USAGE_CPU_TO_GPU};
		VmaAllocationInfo allocation_info = {};
		vmaCreateBuffer(m_context->vma_allocator, &buffer_create_info, &allocation_create_info, &uniform_buffer.vk_buffer, &uniform_buffer.vma_allocation, &allocation_info);
	}
}

void RayTracedGI::destroy_resource()
{
	if (radiance_image.vk_image &&
	    radiance_image.vma_allocation &&
	    radiance_view)
	{
		vkDestroyImageView(m_context->vk_device, radiance_view, nullptr);
		vmaDestroyImage(m_context->vma_allocator, radiance_image.vk_image, radiance_image.vma_allocation);
		radiance_view                 = VK_NULL_HANDLE;
		radiance_image.vk_image       = VK_NULL_HANDLE;
		radiance_image.vma_allocation = VK_NULL_HANDLE;
	}

	if (direction_depth_image.vk_image &&
	    direction_depth_image.vma_allocation &&
	    direction_depth_view)
	{
		vkDestroyImageView(m_context->vk_device, direction_depth_view, nullptr);
		vmaDestroyImage(m_context->vma_allocator, direction_depth_image.vk_image, direction_depth_image.vma_allocation);
		direction_depth_view                 = VK_NULL_HANDLE;
		direction_depth_image.vk_image       = VK_NULL_HANDLE;
		direction_depth_image.vma_allocation = VK_NULL_HANDLE;
	}

	for (uint32_t i = 0; i < 2; i++)
	{
		if (probe_grid_irradiance_image[i].vk_image &&
		    probe_grid_irradiance_image[i].vma_allocation &&
		    probe_grid_irradiance_view[i])
		{
			vkDestroyImageView(m_context->vk_device, probe_grid_irradiance_view[i], nullptr);
			vmaDestroyImage(m_context->vma_allocator, probe_grid_irradiance_image[i].vk_image, probe_grid_irradiance_image[i].vma_allocation);
			probe_grid_irradiance_view[i]                 = VK_NULL_HANDLE;
			probe_grid_irradiance_image[i].vk_image       = VK_NULL_HANDLE;
			probe_grid_irradiance_image[i].vma_allocation = VK_NULL_HANDLE;
		}

		if (probe_grid_depth_image[i].vk_image &&
		    probe_grid_depth_image[i].vma_allocation &&
		    probe_grid_depth_view[i])
		{
			vkDestroyImageView(m_context->vk_device, probe_grid_depth_view[i], nullptr);
			vmaDestroyImage(m_context->vma_allocator, probe_grid_depth_image[i].vk_image, probe_grid_depth_image[i].vma_allocation);
			probe_grid_depth_view[i]                      = VK_NULL_HANDLE;
			probe_grid_depth_image[i].vk_image            = VK_NULL_HANDLE;
			probe_grid_depth_image[i].vma_allocation      = VK_NULL_HANDLE;
		}
	}

	if (sample_probe_grid_image.vk_image &&
	    sample_probe_grid_image.vma_allocation &&
	    sample_probe_grid_view)
	{
		vkDestroyImageView(m_context->vk_device, sample_probe_grid_view, nullptr);
		vmaDestroyImage(m_context->vma_allocator, sample_probe_grid_image.vk_image, sample_probe_grid_image.vma_allocation);
		sample_probe_grid_view                 = VK_NULL_HANDLE;
		sample_probe_grid_image.vk_image       = VK_NULL_HANDLE;
		sample_probe_grid_image.vma_allocation = VK_NULL_HANDLE;
	}

	if (uniform_buffer.vk_buffer && uniform_buffer.vma_allocation)
	{
		vmaDestroyBuffer(m_context->vma_allocator, uniform_buffer.vk_buffer, uniform_buffer.vma_allocation);
		uniform_buffer.vk_buffer      = VK_NULL_HANDLE;
		uniform_buffer.vma_allocation = VK_NULL_HANDLE;
		uniform_buffer.device_address = 0;
	}
}
