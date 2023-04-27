#define VOLK_IMPLEMENTATION
#define VMA_IMPLEMENTATION
#include "render/context.hpp"
#include "core/log.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include <vector>

static VkDebugUtilsMessengerEXT vkDebugUtilsMessengerEXT;

inline const std::vector<const char *> get_instance_extension_supported(const std::vector<const char *> &extensions)
{
	uint32_t extension_count = 0;
	vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr);

	std::vector<VkExtensionProperties> device_extensions(extension_count);
	vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, device_extensions.data());

	std::vector<const char *> result;

	for (const auto &extension : extensions)
	{
		bool found = false;
		for (const auto &device_extension : device_extensions)
		{
			if (strcmp(extension, device_extension.extensionName) == 0)
			{
				result.emplace_back(extension);
				found = true;
				break;
			}
		}
	}

	return result;
}

inline bool check_layer_supported(const char *layer_name)
{
	uint32_t layer_count;
	vkEnumerateInstanceLayerProperties(&layer_count, nullptr);

	std::vector<VkLayerProperties> layers(layer_count);
	vkEnumerateInstanceLayerProperties(&layer_count, layers.data());

	for (const auto &layer : layers)
	{
		if (strcmp(layer.layerName, layer_name) == 0)
		{
			return true;
		}
	}

	return false;
}

static inline VKAPI_ATTR VkBool32 VKAPI_CALL validation_callback(VkDebugUtilsMessageSeverityFlagBitsEXT msg_severity, VkDebugUtilsMessageTypeFlagsEXT msg_type, const VkDebugUtilsMessengerCallbackDataEXT *callback_data, void *user_data)
{
	if (msg_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)
	{
		LOG_INFO(callback_data->pMessage);
	}
	else if (msg_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
	{
		LOG_WARN(callback_data->pMessage);
	}
	else if (msg_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
	{
		LOG_ERROR(callback_data->pMessage);
	}

	return VK_FALSE;
}

inline uint32_t score_physical_device(VkPhysicalDevice physical_device, const std::vector<const char *> &device_extensions, std::vector<const char *> &support_device_extensions)
{
	uint32_t score = 0;

	// Check extensions
	uint32_t device_extension_properties_count = 0;
	vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &device_extension_properties_count, nullptr);

	std::vector<VkExtensionProperties> extension_properties(device_extension_properties_count);
	vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &device_extension_properties_count, extension_properties.data());

	for (auto &device_extension : device_extensions)
	{
		for (auto &support_extension : extension_properties)
		{
			if (std::strcmp(device_extension, support_extension.extensionName) == 0)
			{
				support_device_extensions.push_back(device_extension);
				score += 100;
				break;
			}
		}
	}

	VkPhysicalDeviceProperties properties = {};

	vkGetPhysicalDeviceProperties(physical_device, &properties);

	// Score discrete gpu
	if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
	{
		score += 1000;
	}

	score += properties.limits.maxImageDimension2D;
	return score;
}

inline VkPhysicalDevice select_physical_device(const std::vector<VkPhysicalDevice> &physical_devices, const std::vector<const char *> &device_extensions)
{
	// Score - GPU
	uint32_t         score  = 0;
	VkPhysicalDevice handle = VK_NULL_HANDLE;
	for (auto &gpu : physical_devices)
	{
		std::vector<const char *> support_extensions;

		uint32_t tmp_score = score_physical_device(gpu, device_extensions, support_extensions);
		if (tmp_score > score)
		{
			score  = tmp_score;
			handle = gpu;
		}
	}

	return handle;
}

inline std::optional<uint32_t> get_queue_family_index(const std::vector<VkQueueFamilyProperties> &queue_family_properties, VkQueueFlagBits queue_flag)
{
	// Dedicated queue for compute
	// Try to find a queue family index that supports compute but not graphics
	if (queue_flag & VK_QUEUE_COMPUTE_BIT)
	{
		for (uint32_t i = 0; i < static_cast<uint32_t>(queue_family_properties.size()); i++)
		{
			if ((queue_family_properties[i].queueFlags & queue_flag) && ((queue_family_properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0))
			{
				return i;
				break;
			}
		}
	}

	// Dedicated queue for transfer
	// Try to find a queue family index that supports transfer but not graphics and compute
	if (queue_flag & VK_QUEUE_TRANSFER_BIT)
	{
		for (uint32_t i = 0; i < static_cast<uint32_t>(queue_family_properties.size()); i++)
		{
			if ((queue_family_properties[i].queueFlags & queue_flag) && ((queue_family_properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) && ((queue_family_properties[i].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0))
			{
				return i;
				break;
			}
		}
	}

	// For other queue types or if no separate compute queue is present, return the first one to support the requested flags
	for (uint32_t i = 0; i < static_cast<uint32_t>(queue_family_properties.size()); i++)
	{
		if (queue_family_properties[i].queueFlags & queue_flag)
		{
			return i;
			break;
		}
	}

	return std::optional<uint32_t>();
}

inline const std::vector<const char *> get_device_extension_support(VkPhysicalDevice physical_device, const std::vector<const char *> &extensions)
{
	std::vector<const char *> result;

	uint32_t device_extension_properties_count = 0;
	vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &device_extension_properties_count, nullptr);

	std::vector<VkExtensionProperties> extension_properties(device_extension_properties_count);
	vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &device_extension_properties_count, extension_properties.data());

	for (auto &device_extension : extensions)
	{
		bool enable = false;
		for (auto &support_extension : extension_properties)
		{
			if (std::strcmp(device_extension, support_extension.extensionName) == 0)
			{
				result.push_back(device_extension);
				enable = true;
				break;
			}
		}
	}

	return result;
}

Context::Context(const ContextConfig &config)
{
	// Init window
	{
		if (!glfwInit())
		{
			return;
		}

		if (config.width == 0 || config.height == 0)
		{
			auto *video_mode = glfwGetVideoMode(glfwGetPrimaryMonitor());

			extent.width  = static_cast<uint32_t>(video_mode->width * 3 / 4);
			extent.height = static_cast<uint32_t>(video_mode->height * 3 / 4);
		}
		else
		{
			extent.width  = config.width;
			extent.height = config.height;
		}

		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
		glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

		window = glfwCreateWindow(extent.width, extent.height, "RayTracer", NULL, NULL);
		if (!window)
		{
			glfwTerminate();
			return;
		}

		glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
	}

	// Init vulkan instance
	{
		// Initialize volk context
		volkInitialize();

		// Config application info
		uint32_t api_version = 0;

		PFN_vkEnumerateInstanceVersion enumerate_instance_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));

		if (enumerate_instance_version)
		{
			enumerate_instance_version(&api_version);
		}
		else
		{
			api_version = VK_VERSION_1_0;
		}

		VkApplicationInfo app_info{
		    .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		    .pApplicationName   = "RayTracer",
		    .applicationVersion = VK_MAKE_VERSION(0, 0, 1),
		    .pEngineName        = "RayTracer",
		    .engineVersion      = VK_MAKE_VERSION(0, 0, 1),
		    .apiVersion         = api_version,
		};

		std::vector<const char *> instance_extensions = get_instance_extension_supported({
#ifdef DEBUG
		    "VK_KHR_surface", "VK_KHR_win32_surface", "VK_EXT_debug_report", "VK_EXT_debug_utils"
#else
		    "VK_KHR_surface", "VK_KHR_win32_surface"
#endif
		});
		VkInstanceCreateInfo create_info{
		    .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		    .pApplicationInfo        = &app_info,
		    .enabledLayerCount       = 0,
		    .enabledExtensionCount   = static_cast<uint32_t>(instance_extensions.size()),
		    .ppEnabledExtensionNames = instance_extensions.data(),
		};

		// Enable validation layers
#ifdef DEBUG
		const std::vector<VkValidationFeatureEnableEXT> validation_extensions =
#	ifdef DEBUG
		    {VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
#	else
		    {};
#	endif        // DEBUG
		VkValidationFeaturesEXT validation_features{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
		validation_features.enabledValidationFeatureCount = static_cast<uint32_t>(validation_extensions.size());
		validation_features.pEnabledValidationFeatures    = validation_extensions.data();

		uint32_t layer_count;
		vkEnumerateInstanceLayerProperties(&layer_count, nullptr);

		std::vector<VkLayerProperties> layers(layer_count);
		vkEnumerateInstanceLayerProperties(&layer_count, layers.data());

		std::vector<const char *> validation_layers = {"VK_LAYER_KHRONOS_validation"};
		for (auto &layer : validation_layers)
		{
			if (check_layer_supported(layer))
			{
				create_info.enabledLayerCount   = static_cast<uint32_t>(validation_layers.size());
				create_info.ppEnabledLayerNames = validation_layers.data();
				create_info.pNext               = &validation_features;
				break;
			}
			else
			{
				LOG_ERROR("Validation layer was required, but not avaliable, disabling debugging");
			}
		}
#endif        // DEBUG

		// Create instance
		if (vkCreateInstance(&create_info, nullptr, &vk_instance) != VK_SUCCESS)
		{
			LOG_ERROR("Failed to create vulkan instance!");
			return;
		}
		else
		{
			// Config to volk
			volkLoadInstance(vk_instance);
		}

		// Initialize instance extension functions
		static PFN_vkCreateDebugUtilsMessengerEXT  vkCreateDebugUtilsMessengerEXT  = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(vk_instance, "vkCreateDebugUtilsMessengerEXT"));
		static PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(vk_instance, "vkDestroyDebugUtilsMessengerEXT"));

		// Enable debugger
#ifdef DEBUG
		if (vkCreateDebugUtilsMessengerEXT)
		{
			VkDebugUtilsMessengerCreateInfoEXT create_info{
			    .sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
			    .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
			    .messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
			    .pfnUserCallback = validation_callback,
			};

			vkCreateDebugUtilsMessengerEXT(vk_instance, &create_info, nullptr, &vkDebugUtilsMessengerEXT);
		}
#endif        // DEBUG
	}

	// Init vulkan device
	{
		const std::vector<const char *> device_extensions = {
		    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
		    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
		    VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
		    VK_KHR_RAY_QUERY_EXTENSION_NAME,
		    VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
		    VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
		    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
		    VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME,
		};

		// Init vulkan physical device
		{
			uint32_t physical_device_count = 0;
			vkEnumeratePhysicalDevices(vk_instance, &physical_device_count, nullptr);

			// Get all physical devices
			std::vector<VkPhysicalDevice> physical_devices(physical_device_count);
			vkEnumeratePhysicalDevices(vk_instance, &physical_device_count, physical_devices.data());

			// Select suitable physical device
			vk_physical_device = select_physical_device(physical_devices, device_extensions);

			vkGetPhysicalDeviceProperties(vk_physical_device, &physical_device_properties);
		}

		// Init vulkan logical device
		{
			uint32_t queue_family_property_count = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(vk_physical_device, &queue_family_property_count, nullptr);
			std::vector<VkQueueFamilyProperties> queue_family_properties(queue_family_property_count);
			vkGetPhysicalDeviceQueueFamilyProperties(vk_physical_device, &queue_family_property_count, queue_family_properties.data());

			graphics_family = get_queue_family_index(queue_family_properties, VK_QUEUE_GRAPHICS_BIT);
			transfer_family = get_queue_family_index(queue_family_properties, VK_QUEUE_TRANSFER_BIT);
			compute_family  = get_queue_family_index(queue_family_properties, VK_QUEUE_COMPUTE_BIT);

			VkQueueFlags support_queues = 0;

			if (graphics_family.has_value())
			{
				graphics_family = graphics_family.value();
				support_queues |= VK_QUEUE_GRAPHICS_BIT;
			}

			if (compute_family.has_value())
			{
				compute_family = compute_family.value();
				support_queues |= VK_QUEUE_COMPUTE_BIT;
			}

			if (transfer_family.has_value())
			{
				transfer_family = transfer_family.value();
				support_queues |= VK_QUEUE_TRANSFER_BIT;
			}

			if (!graphics_family)
			{
				throw std::runtime_error("Failed to find queue graphics family support!");
			}

			// Create device queue
			std::vector<VkDeviceQueueCreateInfo> queue_create_infos;

			uint32_t max_count = 0;
			for (auto &queue_family_property : queue_family_properties)
			{
				max_count = max_count < queue_family_property.queueCount ? queue_family_property.queueCount : max_count;
			}

			std::vector<float> queue_priorities(max_count, 1.f);

			if (support_queues & VK_QUEUE_GRAPHICS_BIT)
			{
				VkDeviceQueueCreateInfo graphics_queue_create_info = {
				    .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
				    .queueFamilyIndex = graphics_family.value(),
				    .queueCount       = queue_family_properties[graphics_family.value()].queueCount,
				    .pQueuePriorities = queue_priorities.data(),
				};
				queue_create_infos.emplace_back(graphics_queue_create_info);
			}
			else
			{
				graphics_family = 0;
			}

			if (support_queues & VK_QUEUE_COMPUTE_BIT && compute_family != graphics_family)
			{
				VkDeviceQueueCreateInfo compute_queue_create_info = {
				    .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
				    .queueFamilyIndex = compute_family.value(),
				    .queueCount       = queue_family_properties[compute_family.value()].queueCount,
				    .pQueuePriorities = queue_priorities.data(),
				};
				queue_create_infos.emplace_back(compute_queue_create_info);
			}
			else
			{
				compute_family = graphics_family;
			}

			if (support_queues & VK_QUEUE_TRANSFER_BIT && transfer_family != graphics_family && transfer_family != compute_family)
			{
				VkDeviceQueueCreateInfo transfer_queue_create_info = {
				    .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
				    .queueFamilyIndex = transfer_family.value(),
				    .queueCount       = queue_family_properties[transfer_family.value()].queueCount,
				    .pQueuePriorities = queue_priorities.data(),
				};
				queue_create_infos.emplace_back(transfer_queue_create_info);
			}
			else
			{
				transfer_family = graphics_family;
			}

			VkPhysicalDeviceFeatures2        physical_device_features          = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
			VkPhysicalDeviceVulkan12Features physical_device_vulkan12_features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
			VkPhysicalDeviceVulkan13Features physical_device_vulkan13_features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};

			physical_device_features.pNext          = &physical_device_vulkan12_features;
			physical_device_vulkan12_features.pNext = &physical_device_vulkan13_features;

			vkGetPhysicalDeviceFeatures2(vk_physical_device, &physical_device_features);

			VkPhysicalDeviceFeatures2        physical_device_features_enable          = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
			VkPhysicalDeviceVulkan12Features physical_device_vulkan12_features_enable = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
			VkPhysicalDeviceVulkan13Features physical_device_vulkan13_features_enable = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};

#define ENABLE_DEVICE_FEATURE(device_feature, device_feature_enable, feature) \
	if (device_feature.feature)                                               \
	{                                                                         \
		device_feature_enable.feature = VK_TRUE;                              \
	}                                                                         \
	else                                                                      \
	{                                                                         \
		LOG_WARN("Device feature {} is not supported", #feature);             \
	}

			ENABLE_DEVICE_FEATURE(physical_device_features.features, physical_device_features_enable.features, multiViewport);
			ENABLE_DEVICE_FEATURE(physical_device_vulkan12_features, physical_device_vulkan12_features_enable, descriptorIndexing);
			ENABLE_DEVICE_FEATURE(physical_device_vulkan12_features, physical_device_vulkan12_features_enable, bufferDeviceAddress);
			ENABLE_DEVICE_FEATURE(physical_device_vulkan12_features, physical_device_vulkan12_features_enable, runtimeDescriptorArray);
			ENABLE_DEVICE_FEATURE(physical_device_vulkan12_features, physical_device_vulkan12_features_enable, descriptorBindingSampledImageUpdateAfterBind);
			ENABLE_DEVICE_FEATURE(physical_device_vulkan12_features, physical_device_vulkan12_features_enable, descriptorBindingPartiallyBound);
			ENABLE_DEVICE_FEATURE(physical_device_vulkan12_features, physical_device_vulkan12_features_enable, shaderOutputViewportIndex);
			ENABLE_DEVICE_FEATURE(physical_device_vulkan12_features, physical_device_vulkan12_features_enable, shaderOutputLayer);
			ENABLE_DEVICE_FEATURE(physical_device_vulkan13_features, physical_device_vulkan13_features_enable, dynamicRendering);
			ENABLE_DEVICE_FEATURE(physical_device_vulkan13_features, physical_device_vulkan13_features_enable, maintenance4);

			auto support_extensions = get_device_extension_support(vk_physical_device, device_extensions);

			VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration_structure_feature = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
			VkPhysicalDeviceRayTracingPipelineFeaturesKHR    ray_tracing_pipeline_feature   = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
			VkPhysicalDeviceRayQueryFeaturesKHR              ray_query_features             = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};

			acceleration_structure_feature.accelerationStructure = VK_TRUE;
			ray_tracing_pipeline_feature.rayTracingPipeline      = VK_TRUE;
			ray_query_features.rayQuery                          = VK_TRUE;

			physical_device_vulkan12_features_enable.pNext = &physical_device_vulkan13_features_enable;
			physical_device_vulkan13_features_enable.pNext = &acceleration_structure_feature;
			acceleration_structure_feature.pNext           = &ray_tracing_pipeline_feature;
			ray_tracing_pipeline_feature.pNext             = &ray_query_features;

#ifdef DEBUG
			std::vector<const char *> validation_layers = {"VK_LAYER_KHRONOS_validation"};
#endif

			VkDeviceCreateInfo device_create_info = {
			    .sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
			    .pNext                = &physical_device_vulkan12_features_enable,
			    .queueCreateInfoCount = static_cast<uint32_t>(queue_create_infos.size()),
			    .pQueueCreateInfos    = queue_create_infos.data(),
#ifdef DEBUG
			    .enabledLayerCount   = static_cast<uint32_t>(validation_layers.size()),
			    .ppEnabledLayerNames = validation_layers.data(),
#endif
			    .enabledExtensionCount   = static_cast<uint32_t>(support_extensions.size()),
			    .ppEnabledExtensionNames = support_extensions.data(),
			    .pEnabledFeatures        = &physical_device_features.features,
			};

			if (vkCreateDevice(vk_physical_device, &device_create_info, nullptr, &vk_device) != VK_SUCCESS)
			{
				LOG_ERROR("Failed to create logical device!");
				return;
			}

			// Volk load context
			volkLoadDevice(vk_device);

			vkGetDeviceQueue(vk_device, graphics_family.value(), 0, &graphics_queue);
			vkGetDeviceQueue(vk_device, compute_family.value(), 0, &compute_queue);
			vkGetDeviceQueue(vk_device, transfer_family.value(), 0, &transfer_queue);
		}
	}

	// Init vma
	{
		// Create Vma allocator
		VmaVulkanFunctions vma_vulkan_func{
		    .vkGetInstanceProcAddr               = vkGetInstanceProcAddr,
		    .vkGetDeviceProcAddr                 = vkGetDeviceProcAddr,
		    .vkGetPhysicalDeviceProperties       = vkGetPhysicalDeviceProperties,
		    .vkGetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties,
		    .vkAllocateMemory                    = vkAllocateMemory,
		    .vkFreeMemory                        = vkFreeMemory,
		    .vkMapMemory                         = vkMapMemory,
		    .vkUnmapMemory                       = vkUnmapMemory,
		    .vkFlushMappedMemoryRanges           = vkFlushMappedMemoryRanges,
		    .vkInvalidateMappedMemoryRanges      = vkInvalidateMappedMemoryRanges,
		    .vkBindBufferMemory                  = vkBindBufferMemory,
		    .vkBindImageMemory                   = vkBindImageMemory,
		    .vkGetBufferMemoryRequirements       = vkGetBufferMemoryRequirements,
		    .vkGetImageMemoryRequirements        = vkGetImageMemoryRequirements,
		    .vkCreateBuffer                      = vkCreateBuffer,
		    .vkDestroyBuffer                     = vkDestroyBuffer,
		    .vkCreateImage                       = vkCreateImage,
		    .vkDestroyImage                      = vkDestroyImage,
		    .vkCmdCopyBuffer                     = vkCmdCopyBuffer,
		};

		VmaAllocatorCreateInfo allocator_info = {
		    .flags            = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
		    .physicalDevice   = vk_physical_device,
		    .device           = vk_device,
		    .pVulkanFunctions = &vma_vulkan_func,
		    .instance         = vk_instance,
		    .vulkanApiVersion = VK_API_VERSION_1_3,
		};

		if (vmaCreateAllocator(&allocator_info, &vma_allocator) != VK_SUCCESS)
		{
			LOG_CRITICAL("Failed to create vulkan memory allocator");
			return;
		}
	}

	// Init vulkan swapchain
	{
#ifdef _WIN32
		{
			VkWin32SurfaceCreateInfoKHR createInfo{
			    .sType     = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
			    .hinstance = GetModuleHandle(nullptr),
			    .hwnd      = glfwGetWin32Window(window),
			};
			vkCreateWin32SurfaceKHR(vk_instance, &createInfo, nullptr, &vk_surface);
		}
#endif        // _WIN32

		VkSurfaceCapabilitiesKHR capabilities;
		vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk_physical_device, vk_surface, &capabilities);

		uint32_t                        format_count;
		std::vector<VkSurfaceFormatKHR> formats;
		vkGetPhysicalDeviceSurfaceFormatsKHR(vk_physical_device, vk_surface, &format_count, nullptr);
		if (format_count != 0)
		{
			formats.resize(format_count);
			vkGetPhysicalDeviceSurfaceFormatsKHR(vk_physical_device, vk_surface, &format_count, formats.data());
		}

		VkSurfaceFormatKHR surface_format = {};
		for (const auto &format : formats)
		{
			if (format.format == VK_FORMAT_B8G8R8A8_UNORM &&
			    format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
			{
				surface_format = format;
			}
		}
		if (surface_format.format == VK_FORMAT_UNDEFINED)
		{
			surface_format = formats[0];
		}
		vk_format = surface_format.format;

		if (capabilities.currentExtent.width != UINT32_MAX)
		{
			extent = capabilities.currentExtent;
		}
		else
		{
			VkExtent2D actualExtent = extent;

			actualExtent.width  = std::clamp(actualExtent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
			actualExtent.height = std::clamp(actualExtent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);

			extent = actualExtent;
		}

		assert(capabilities.maxImageCount >= 3);

		uint32_t queue_family_property_count = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(vk_physical_device, &queue_family_property_count, nullptr);
		std::vector<VkQueueFamilyProperties> queue_family_properties(queue_family_property_count);
		vkGetPhysicalDeviceQueueFamilyProperties(vk_physical_device, &queue_family_property_count, queue_family_properties.data());

		for (uint32_t i = 0; i < queue_family_property_count; i++)
		{
			// Check for presentation support
			VkBool32 present_support;
			vkGetPhysicalDeviceSurfaceSupportKHR(vk_physical_device, i, vk_surface, &present_support);

			if (queue_family_properties[i].queueCount > 0 && present_support)
			{
				present_family = i;
				break;
			}
		}

		vkGetDeviceQueue(vk_device, present_family.value(), 0, &present_queue);

		VkSwapchainCreateInfoKHR createInfo = {};
		createInfo.sType                    = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
		createInfo.surface                  = vk_surface;

		createInfo.minImageCount    = 3;
		createInfo.imageFormat      = surface_format.format;
		createInfo.imageColorSpace  = surface_format.colorSpace;
		createInfo.imageExtent      = extent;
		createInfo.imageArrayLayers = 1;
		createInfo.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

		uint32_t queueFamilyIndices[] = {present_family.value()};

		createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		createInfo.preTransform     = capabilities.currentTransform;
		createInfo.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		createInfo.presentMode      = VK_PRESENT_MODE_MAILBOX_KHR;
		createInfo.clipped          = VK_TRUE;

		vkCreateSwapchainKHR(vk_device, &createInfo, nullptr, &vk_swapchain);

		uint32_t image_count = 3;
		vkGetSwapchainImagesKHR(vk_device, vk_swapchain, &image_count, swapchain_images.data());

		// Create image view
		for (size_t i = 0; i < 3; i++)
		{
			VkImageViewCreateInfo create_info = {
			    .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			    .flags    = 0,
			    .image    = swapchain_images[i],
			    .viewType = VK_IMAGE_VIEW_TYPE_2D,
			    .format   = vk_format,
			    .components{VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
			    .subresourceRange = VkImageSubresourceRange{
			        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
			        .baseMipLevel   = 0,
			        .levelCount     = 1,
			        .baseArrayLayer = 0,
			        .layerCount     = 1,
			    },
			};
			vkCreateImageView(vk_device, &create_info, nullptr, &swapchain_image_views[i]);
		}
	}

	// init vulkan resource
	{
		{
			VkSemaphoreCreateInfo create_info = {
			    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
			    .flags = 0,
			};

			vkCreateSemaphore(vk_device, &create_info, nullptr, &render_complete);
			vkCreateSemaphore(vk_device, &create_info, nullptr, &present_complete);
		}

		{
			for (auto &fence : fences)
			{
				VkFenceCreateInfo create_info = {
				    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
				    .flags = VK_FENCE_CREATE_SIGNALED_BIT,
				};
				vkCreateFence(vk_device, &create_info, nullptr, &fence);
			}
		}

		{
			VkCommandPoolCreateInfo create_info = {
			    .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			    .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
			    .queueFamilyIndex = graphics_family.value(),
			};
			vkCreateCommandPool(vk_device, &create_info, nullptr, &graphics_cmd_pool);
		}

		{
			VkCommandPoolCreateInfo create_info = {
			    .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			    .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
			    .queueFamilyIndex = compute_family.value(),
			};
			vkCreateCommandPool(vk_device, &create_info, nullptr, &compute_cmd_pool);
		}

		{
			VkPipelineCacheCreateInfo create_info = {
			    .sType           = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
			    .initialDataSize = 0,
			    .pInitialData    = nullptr,
			};
			vkCreatePipelineCache(vk_device, &create_info, nullptr, &vk_pipeline_cache);
		}

		{
			std::vector<VkDescriptorPoolSize> pool_sizes =
			    {
			        {VK_DESCRIPTOR_TYPE_SAMPLER, 1000},
			        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
			        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000},
			        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000},
			        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000},
			        {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000},
			        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000},
			        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000},
			        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000},
			        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000},
			        {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000},
			    };
			VkDescriptorPoolCreateInfo pool_info = {
			    .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			    .flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT | VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
			    .maxSets       = 1000 * static_cast<uint32_t>(pool_sizes.size()),
			    .poolSizeCount = (uint32_t) static_cast<uint32_t>(pool_sizes.size()),
			    .pPoolSizes    = pool_sizes.data(),
			};
			vkCreateDescriptorPool(vk_device, &pool_info, nullptr, &vk_descriptor_pool);
		}
	}
}

Context::~Context()
{
	vkDeviceWaitIdle(vk_device);

	// Destroy window
	glfwDestroyWindow(window);
	glfwTerminate();

	for (auto &view : swapchain_image_views)
	{
		vkDestroyImageView(vk_device, view, nullptr);
	}

	for (auto &fence : fences)
	{
		vkDestroyFence(vk_device, fence, nullptr);
	}

	vkDestroyDescriptorPool(vk_device, vk_descriptor_pool, nullptr);
	vkDestroyPipelineCache(vk_device, vk_pipeline_cache, nullptr);

	vkDestroyCommandPool(vk_device, graphics_cmd_pool, nullptr);
	vkDestroyCommandPool(vk_device, compute_cmd_pool, nullptr);

	vkDestroySemaphore(vk_device, render_complete, nullptr);
	vkDestroySemaphore(vk_device, present_complete, nullptr);

	vkDestroySwapchainKHR(vk_device, vk_swapchain, nullptr);
	vkDestroySurfaceKHR(vk_instance, vk_surface, nullptr);
	vmaDestroyAllocator(vma_allocator);
	vkDestroyDevice(vk_device, nullptr);
#ifdef DEBUG
	vkDestroyDebugUtilsMessengerEXT(vk_instance, vkDebugUtilsMessengerEXT, nullptr);
#endif        // DEBUG
	vkDestroyInstance(vk_instance, nullptr);
}

VkCommandBuffer Context::create_command_buffer(bool compute) const
{
	VkCommandBuffer             cmd_buffer = VK_NULL_HANDLE;
	VkCommandBufferAllocateInfo allocate_info =
	    {
	        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	        .commandPool        = compute ? compute_cmd_pool : graphics_cmd_pool,
	        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	        .commandBufferCount = 1,
	    };
	vkAllocateCommandBuffers(vk_device, &allocate_info, &cmd_buffer);
	return cmd_buffer;
}

void Context::flush_command_buffer(VkCommandBuffer cmd_buffer, bool compute) const
{
	VkFence           fence       = VK_NULL_HANDLE;
	VkFenceCreateInfo create_info = {
	    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
	    .flags = 0,
	};
	vkCreateFence(vk_device, &create_info, nullptr, &fence);
	VkSubmitInfo submit_info = {
	    .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .waitSemaphoreCount   = 0,
	    .pWaitSemaphores      = nullptr,
	    .pWaitDstStageMask    = 0,
	    .commandBufferCount   = 1,
	    .pCommandBuffers      = &cmd_buffer,
	    .signalSemaphoreCount = 0,
	    .pSignalSemaphores    = nullptr,
	};
	vkQueueSubmit(compute ? compute_queue : graphics_queue, 1, &submit_info, fence);

	// Wait
	vkWaitForFences(vk_device, 1, &fence, VK_TRUE, UINT64_MAX);
	vkResetFences(vk_device, 1, &fence);

	// Release resource
	vkDestroyFence(vk_device, fence, nullptr);
	vkFreeCommandBuffers(vk_device, compute ? compute_cmd_pool : graphics_cmd_pool, 1, &cmd_buffer);
}

void Context::set_object_name(VkObjectType type, uint64_t handle, const char *name) const
{
#ifdef DEBUG
	VkDebugUtilsObjectNameInfoEXT info = {
	    .sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
	    .objectType   = type,
	    .objectHandle = (uint64_t) handle,
	    .pObjectName  = name,
	};
	vkSetDebugUtilsObjectNameEXT(vk_device, &info);
#endif        // DEBUG
}

void Context::begin_marker(VkCommandBuffer cmd_buffer, const char *name) const
{
#ifdef DEBUG
	VkDebugUtilsLabelEXT label = {
	    .sType      = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
	    .pLabelName = name,
	    .color      = {0, 1, 0, 0},
	};
	vkCmdBeginDebugUtilsLabelEXT(cmd_buffer, &label);
#endif        // DEBUG
}

void Context::end_marker(VkCommandBuffer cmd_buffer) const
{
#ifdef DEBUG
	vkCmdEndDebugUtilsLabelEXT(cmd_buffer);
#endif        // DEBUG
}
