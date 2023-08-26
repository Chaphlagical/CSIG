#pragma once

#include <volk.h>

#include <vk_mem_alloc.h>

#include <glm/glm.hpp>

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct GLFWwindow;
struct Context;
struct CommandBufferRecorder;

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
	void           *mapped_data    = nullptr;
};

struct AccelerationStructure
{
	VkAccelerationStructureKHR vk_as = VK_NULL_HANDLE;

	Buffer buffer;

	VkDeviceAddress device_address = 0;
};

struct BarrierBuilder
{
	CommandBufferRecorder             &recorder;
	std::vector<VkImageMemoryBarrier>  image_barriers;
	std::vector<VkBufferMemoryBarrier> buffer_barriers;

	BarrierBuilder(CommandBufferRecorder &recorder);

	BarrierBuilder &add_image_barrier(
	    VkImage                        image,
	    VkAccessFlags                  src_mask,
	    VkAccessFlags                  dst_mask,
	    VkImageLayout                  old_layout,
	    VkImageLayout                  new_layout,
	    const VkImageSubresourceRange &range = {
	        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	        .baseMipLevel   = 0,
	        .levelCount     = 1,
	        .baseArrayLayer = 0,
	        .layerCount     = 1,
	    });

	BarrierBuilder &add_buffer_barrier(
	    VkBuffer      buffer,
	    VkAccessFlags src_mask,
	    VkAccessFlags dst_mask,
	    size_t        size   = VK_WHOLE_SIZE,
	    size_t        offset = 0);

	CommandBufferRecorder &insert(VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
};

struct CommandBufferRecorder
{
	VkCommandBuffer cmd_buffer;
	const Context*        context;

	std::vector<VkRenderingAttachmentInfo>   color_attachments;
	std::optional<VkRenderingAttachmentInfo> depth_stencil_attachment;

	explicit CommandBufferRecorder(const Context& context, VkCommandBuffer cmd_buffer);

	CommandBufferRecorder &begin();

	CommandBufferRecorder &end();

	CommandBufferRecorder &begin_marker(const std::string &name);

	CommandBufferRecorder &end_marker();

	CommandBufferRecorder &add_color_attachment(
	    VkImageView         view,
	    VkAttachmentLoadOp  load_op     = VK_ATTACHMENT_LOAD_OP_CLEAR,
	    VkAttachmentStoreOp store_op    = VK_ATTACHMENT_STORE_OP_STORE,
	    VkClearColorValue   clear_value = {});

	CommandBufferRecorder &add_depth_attachment(
	    VkImageView              view,
	    VkAttachmentLoadOp       load_op     = VK_ATTACHMENT_LOAD_OP_CLEAR,
	    VkAttachmentStoreOp      store_op    = VK_ATTACHMENT_STORE_OP_STORE,
	    VkClearDepthStencilValue clear_value = {});

	CommandBufferRecorder &begin_render_pass(
	    uint32_t      width,
	    uint32_t      height,
	    VkRenderPass  render_pass,
	    VkFramebuffer frame_buffer,
	    VkClearValue  clear_value = {});

	CommandBufferRecorder &end_render_pass();

	CommandBufferRecorder &begin_rendering(
	    uint32_t width,
	    uint32_t height,
	    uint32_t layer = 1);

	CommandBufferRecorder &end_rendering();

	CommandBufferRecorder &update_buffer(
	    VkBuffer buffer,
	    void    *data,
	    size_t   size,
	    size_t   offset = 0);

	CommandBufferRecorder &push_constants(
	    VkPipelineLayout   pipeline_layout,
	    VkShaderStageFlags stages,
	    void              *data,
	    size_t             size);

	CommandBufferRecorder &copy_buffer_to_image(
	    VkBuffer                        buffer,
	    VkImage                         image,
	    const VkExtent3D               &extent,
	    const VkOffset3D               &offset = {0, 0, 0},
	    const VkImageSubresourceLayers &range  = {
	         .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	         .mipLevel       = 0,
	         .baseArrayLayer = 0,
	         .layerCount     = 1,
        });
	CommandBufferRecorder &bind_descriptor_set(
	    VkPipelineBindPoint                 bind_point,
	    VkPipelineLayout                    pipeline_layout,
	    const std::vector<VkDescriptorSet> &descriptor_sets);

	CommandBufferRecorder &bind_pipeline(
	    VkPipelineBindPoint bind_point,
	    VkPipeline          pipeline);

	CommandBufferRecorder &bind_vertex_buffers(const std::vector<VkBuffer> &vertex_buffers);

	CommandBufferRecorder &bind_index_buffer(
	    VkBuffer    index_buffer,
	    size_t      offset = 0,
	    VkIndexType type   = VK_INDEX_TYPE_UINT32);

	CommandBufferRecorder &dispatch(
	    const glm::uvec3 &thread_num,
	    const glm::uvec3 &group_size);

	CommandBufferRecorder &draw_mesh_task(
	    const glm::uvec3 &thread_num,
	    const glm::uvec3 &group_size);

	CommandBufferRecorder &draw_indexed(
	    uint32_t index_count,
	    uint32_t instance_count = 1,
	    uint32_t first_index    = 0,
	    int32_t  vertex_offset  = 0,
	    uint32_t first_instance = 0);

	CommandBufferRecorder &fill_buffer(
	    VkBuffer buffer,
	    uint32_t data   = 0,
	    size_t   size   = VK_WHOLE_SIZE,
	    size_t   offset = 0);

	CommandBufferRecorder &clear_color_image(
	    VkImage                        image,
	    const VkClearColorValue       &clear_value = {},
	    const VkImageSubresourceRange &range       = {
	              .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	              .baseMipLevel   = 0,
	              .levelCount     = 1,
	              .baseArrayLayer = 0,
	              .layerCount     = 1,
        });

	BarrierBuilder insert_barrier();

	void flush(bool compute = false);

	template <typename T>
	CommandBufferRecorder &push_constants(VkPipelineLayout pipeline_layout, VkShaderStageFlags stages, T data)
	{
		return push_constants(pipeline_layout, stages, &data, sizeof(T));
	}
};

struct DescriptorLayoutBuilder
{
	const Context *context = nullptr;

	std::vector<VkDescriptorSetLayoutBinding> bindings;
	std::vector<VkDescriptorBindingFlags>     binding_flags;
	bool                                      bindless = false;

	explicit DescriptorLayoutBuilder(const Context &context);

	DescriptorLayoutBuilder &add_descriptor_binding(uint32_t binding, VkDescriptorType type, VkShaderStageFlags stage, uint32_t count);
	DescriptorLayoutBuilder &add_descriptor_bindless_binding(uint32_t binding, VkDescriptorType type, VkShaderStageFlags stage, uint32_t count = 1024);

	VkDescriptorSetLayout create();
};

struct DescriptorUpdateBuilder
{
	const Context *context = nullptr;

	std::vector<VkWriteDescriptorSet>   write_sets;
	std::vector<size_t>                 descriptor_index;
	std::vector<VkDescriptorImageInfo>  image_infos;
	std::vector<VkDescriptorBufferInfo> buffer_infos;

	std::vector<VkWriteDescriptorSetAccelerationStructureKHR> as_infos;

	explicit DescriptorUpdateBuilder(const Context &context);

	DescriptorUpdateBuilder &write_storage_images(uint32_t binding, const std::vector<VkImageView> &image_views);
	DescriptorUpdateBuilder &write_sampled_images(uint32_t binding, const std::vector<VkImageView> &image_views);
	DescriptorUpdateBuilder &write_samplers(uint32_t binding, const std::vector<VkSampler> &samplers);
	DescriptorUpdateBuilder &write_uniform_buffers(uint32_t binding, const std::vector<VkBuffer> &buffers);
	DescriptorUpdateBuilder &write_storage_buffers(uint32_t binding, const std::vector<VkBuffer> &buffers);
	DescriptorUpdateBuilder &write_acceleration_structures(uint32_t binding, const std::vector<AccelerationStructure> &as);

	DescriptorUpdateBuilder &update(VkDescriptorSet set);
};

struct GraphicsPipelineBuilder
{
	const Context   *context         = nullptr;
	VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;

	std::vector<VkPipelineShaderStageCreateInfo>     shader_states;
	std::vector<VkFormat>                            color_attachments;
	std::optional<VkFormat>                          depth_attachment;
	VkPipelineInputAssemblyStateCreateInfo           input_assembly_state;
	VkPipelineRasterizationStateCreateInfo           rasterization_state;
	std::vector<VkPipelineColorBlendAttachmentState> color_blend_attachment_states;
	VkPipelineDepthStencilStateCreateInfo            depth_stencil_state;
	VkPipelineMultisampleStateCreateInfo             multisample_state;
	std::vector<VkVertexInputAttributeDescription>   vertex_input_attributes;
	std::vector<VkVertexInputBindingDescription>     vertex_input_bindings;
	std::vector<VkViewport>                          viewports;
	std::vector<VkRect2D>                            scissors;

	explicit GraphicsPipelineBuilder(const Context &context, VkPipelineLayout layout);

	GraphicsPipelineBuilder &add_shader(VkShaderStageFlagBits stage, const std::string &shader_path, const std::string &entry_point = "main", const std::unordered_map<std::string, std::string> &macros = {});
	GraphicsPipelineBuilder &add_shader(VkShaderStageFlagBits stage, const uint32_t *spirv_code, size_t size);
	GraphicsPipelineBuilder &add_shader(VkShaderStageFlagBits stage, VkShaderModule shader);
	GraphicsPipelineBuilder &add_color_attachment(VkFormat format, VkPipelineColorBlendAttachmentState blend_state = {.blendEnable = false, .colorWriteMask = 0xf});
	GraphicsPipelineBuilder &add_depth_stencil(VkFormat format, bool depth_test = true, bool depth_write = true, VkCompareOp compare = VK_COMPARE_OP_GREATER, bool stencil_test = false, VkStencilOpState front = {}, VkStencilOpState back = {});
	GraphicsPipelineBuilder &add_viewport(const VkViewport &viewport);
	GraphicsPipelineBuilder &add_scissor(const VkRect2D &scissor);
	GraphicsPipelineBuilder &set_input_assembly(VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	GraphicsPipelineBuilder &set_multisample(VkSampleCountFlagBits sample_count = VK_SAMPLE_COUNT_1_BIT);
	GraphicsPipelineBuilder &set_rasterization(VkPolygonMode polygon = VK_POLYGON_MODE_FILL, VkCullModeFlags cull = VK_CULL_MODE_NONE, VkFrontFace front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE, float line_width = 1.f, float depth_bias = 0.f, float depth_bias_slope = 0.f, float depth_bias_clamp = 0.f);
	GraphicsPipelineBuilder &add_vertex_input_attribute(uint32_t location, uint32_t binding, VkFormat format, uint32_t offset);
	GraphicsPipelineBuilder &add_vertex_input_binding(uint32_t binding, uint32_t stride, VkVertexInputRate input_rate = VK_VERTEX_INPUT_RATE_VERTEX);

	VkPipeline create();
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

	VkExtent2D extent      = {};
	uint32_t   image_index = 0;
	bool       ping_pong   = false;

	VkPhysicalDeviceProperties physical_device_properties;

	VkSampler default_sampler = VK_NULL_HANDLE;

	explicit Context(uint32_t width, uint32_t height);

	~Context();

	CommandBufferRecorder record_command(bool compute = false) const;

	Buffer create_buffer(const std::string &name, size_t size, VkBufferUsageFlags buffer_usage, VmaMemoryUsage memory_usage) const;

	Buffer create_buffer(const std::string &name, void *data, size_t size, VkBufferUsageFlags buffer_usage, VmaMemoryUsage memory_usage) const;

	template <typename T>
	Buffer create_buffer(const std::string &name, VkBufferUsageFlags buffer_usage, VmaMemoryUsage memory_usage) const
	{
		return create_buffer(name, sizeof(T), buffer_usage, memory_usage);
	}

	template <typename T>
	Buffer create_buffer(const std::string &name, const T &data, VkBufferUsageFlags buffer_usage, VmaMemoryUsage memory_usage) const
	{
		return create_buffer(name, (void *) &data, sizeof(data), buffer_usage, memory_usage);
	}

	template <typename T>
	Buffer create_buffer(const std::string &name, const std::vector<T> &data, VkBufferUsageFlags buffer_usage, VmaMemoryUsage memory_usage) const
	{
		return create_buffer(name, (void *) data.data(), sizeof(T) * data.size(), buffer_usage, memory_usage);
	}

	std::pair<AccelerationStructure, Buffer> create_acceleration_structure(const std::string &name, VkAccelerationStructureTypeKHR type, const VkAccelerationStructureGeometryKHR &geometry, const VkAccelerationStructureBuildRangeInfoKHR &range) const;

	void buffer_copy_to_device(void *data, size_t size, const Buffer &buffer, bool staging = false) const;

	template <typename T>
	void buffer_copy_to_device(const T &data, const Buffer &buffer, bool staging = false) const
	{
		buffer_copy_to_device(&data, sizeof(data), buffer, staging);
	}

	template <typename T>
	void buffer_copy_to_device(const std::vector<T> &data, const Buffer &buffer, bool staging = false) const
	{
		buffer_copy_to_device((void *) data.data(), sizeof(T) * data.size(), buffer, staging);
	}

	void buffer_copy_to_host(void *data, size_t size, const Buffer &buffer, bool staging = false) const;

	Texture load_texture_2d(const std::string &filename, bool mipmap = false) const;

	Texture load_texture_cube(const std::string &filename, bool mipmap = false) const;

	Texture create_texture_2d(const std::string &name, uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage, bool mipmap = false) const;

	Texture create_texture_2d_array(const std::string &name, uint32_t width, uint32_t height, uint32_t layer, VkFormat format, VkImageUsageFlags usage) const;

	VkImageView create_texture_view(
	    const std::string             &name,
	    VkImage                        image,
	    VkFormat                       format,
	    VkImageViewType                type  = VK_IMAGE_VIEW_TYPE_2D,
	    const VkImageSubresourceRange &range = {
	        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	        .baseMipLevel   = 0,
	        .levelCount     = 1,
	        .baseArrayLayer = 0,
	        .layerCount     = 1,
	    }) const;

	VkShaderModule load_spirv_shader(const uint32_t *spirv_code, size_t size) const;

	VkShaderModule load_hlsl_shader(const std::string &path, VkShaderStageFlagBits stage, const std::string &entry_point = "main", const std::unordered_map<std::string, std::string> &macros = {}) const;

	VkShaderModule load_glsl_shader(const std::string &path, VkShaderStageFlagBits stage, const std::string &entry_point = "main", const std::unordered_map<std::string, std::string> &macros = {}) const;

	DescriptorLayoutBuilder create_descriptor_layout() const;

	VkDescriptorSet allocate_descriptor_set(const std::vector<VkDescriptorSetLayout> &layouts) const;

	VkPipelineLayout create_pipeline_layout(const std::vector<VkDescriptorSetLayout> &layouts, VkShaderStageFlags stages = VK_SHADER_STAGE_ALL, uint32_t push_data_size = 0) const;

	VkPipeline create_compute_pipeline(VkShaderModule shader, VkPipelineLayout layout) const;

	VkPipeline create_compute_pipeline(const std::string &shader_path, VkPipelineLayout layout, const std::string &entry_point = "main", const std::unordered_map<std::string, std::string> &macros = {}) const;

	VkPipeline create_compute_pipeline(const uint32_t *spirv_code, size_t size, VkPipelineLayout layout) const;

	GraphicsPipelineBuilder create_graphics_pipeline(VkPipelineLayout layout) const;

	DescriptorUpdateBuilder update_descriptor() const;

	void present(VkCommandBuffer cmd_buffer, VkImage image, VkExtent2D extent = {0, 0}) const;

	template <typename T>
	const Context &destroy(T data) const;

	template <uint32_t N>
	std::array<VkDescriptorSet, N> allocate_descriptor_sets(const std::vector<VkDescriptorSetLayout> &layouts) const
	{
		std::array<VkDescriptorSet, N> descriptor_sets;
		VkDescriptorSetAllocateInfo    allocate_info = {
		       .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		       .pNext              = nullptr,
		       .descriptorPool     = vk_descriptor_pool,
		       .descriptorSetCount = N,
		       .pSetLayouts        = layouts.data(),
        };
		vkAllocateDescriptorSets(vk_device, &allocate_info, descriptor_sets.data());
		return descriptor_sets;
	}

  private:
	void set_object_name(VkObjectType type, uint64_t handle, const char *name) const;

	Buffer create_scratch_buffer(size_t size) const;
};
