#pragma once

#include <volk.h>

#include <vk_mem_alloc.h>

#include <array>
#include <optional>

struct GLFWwindow;

struct ContextConfig
{
	bool fullscreen = false;

	uint32_t width  = 1920;
	uint32_t height = 1080;
};

struct Texture
{
	VkImage       vk_image       = VK_NULL_HANDLE;
	VmaAllocation vma_allocation = VK_NULL_HANDLE;
};

struct Buffer
{
	VkBuffer        vk_buffer      = VK_NULL_HANDLE;
	VmaAllocation   vma_allocation = VK_NULL_HANDLE;
	VkDeviceAddress device_address = 0;
};

struct AccelerationStructure
{
	VkAccelerationStructureKHR vk_as = VK_NULL_HANDLE;

	Buffer buffer;
	Buffer instance_buffer;

	VkDeviceAddress device_address = 0;
};

struct Context
{
	GLFWwindow      *window             = nullptr;
	VkInstance       vk_instance        = VK_NULL_HANDLE;
	VkPhysicalDevice vk_physical_device = VK_NULL_HANDLE;
	VkDevice         vk_device          = VK_NULL_HANDLE;
	VmaAllocator     vma_allocator      = VK_NULL_HANDLE;
	VkSurfaceKHR     vk_surface         = VK_NULL_HANDLE;
	VkSwapchainKHR   vk_swapchain       = VK_NULL_HANDLE;
	VkPipelineCache  vk_pipeline_cache  = VK_NULL_HANDLE;
	VkDescriptorPool vk_descriptor_pool = VK_NULL_HANDLE;

	VkFormat vk_format = VK_FORMAT_UNDEFINED;

	VkCommandPool graphics_cmd_pool = VK_NULL_HANDLE;
	VkCommandPool compute_cmd_pool  = VK_NULL_HANDLE;

	std::optional<uint32_t> graphics_family;
	std::optional<uint32_t> compute_family;
	std::optional<uint32_t> transfer_family;
	std::optional<uint32_t> present_family;

	VkQueue graphics_queue = VK_NULL_HANDLE;
	VkQueue compute_queue  = VK_NULL_HANDLE;
	VkQueue transfer_queue = VK_NULL_HANDLE;
	VkQueue present_queue  = VK_NULL_HANDLE;

	std::array<VkImage, 3>     swapchain_images      = {VK_NULL_HANDLE};
	std::array<VkImageView, 3> swapchain_image_views = {VK_NULL_HANDLE};

	VkSemaphore render_complete  = VK_NULL_HANDLE;
	VkSemaphore present_complete = VK_NULL_HANDLE;

	std::array<VkFence, 3> fences = {VK_NULL_HANDLE};

	VkExtent2D extent = {};

	VkPhysicalDeviceProperties physical_device_properties;

	explicit Context(const ContextConfig &config);

	~Context();

	void set_object_name(VkObjectType type, uint64_t handle, const char* name) const;
	void begin_marker(VkCommandBuffer cmd_buffer, const char* name) const;
	void end_marker(VkCommandBuffer cmd_buffer) const;
};