#pragma once

#include "context.hpp"

struct Scene
{
	Scene(const Context &context);

	~Scene();

	void load_scene(const std::string &filename);

	void load_envmap(const std::string &filename);

	void update_view(CommandBufferRecorder &recorder);

	void update();

  public:
	struct
	{
		uint32_t  vertices_count = 0;
		uint32_t  indices_count  = 0;
		uint32_t  instance_count = 0;
		uint32_t  material_count = 0;
		glm::vec3 min_extent     = glm::vec3(std::numeric_limits<float>::max());
		uint32_t  emitter_count  = 0;
		glm::vec3 max_extent     = -glm::vec3(std::numeric_limits<float>::max());
		uint32_t  mesh_count     = 0;
		uint64_t  instance_buffer_addr;
		uint64_t  emitter_buffer_addr;
		uint64_t  material_buffer_addr;
		uint64_t  vertex_buffer_addr;
		uint64_t  index_buffer_addr;
		uint64_t  emitter_alias_table_buffer_addr;
		uint64_t  mesh_alias_table_buffer_addr;
	} scene_info;

	struct
	{
		glm::mat4 view_inv                 = glm::mat4(1.f);
		glm::mat4 projection_inv           = glm::mat4(1.f);
		glm::mat4 view_projection_inv      = glm::mat4(1.f);
		glm::mat4 view_projection          = glm::mat4(1.f);
		glm::mat4 prev_view                = glm::mat4(1.f);
		glm::mat4 prev_projection          = glm::mat4(1.f);
		glm::mat4 prev_view_projection     = glm::mat4(1.f);
		glm::mat4 prev_view_projection_inv = glm::mat4(1.f);
		glm::vec4 cam_pos                  = glm::vec4(0.f);        // xyz - position, w - num_frames
		glm::vec4 prev_cam_pos             = glm::vec4(0.f);        // xyz - position, w - padding
		glm::vec4 jitter                   = glm::vec4(0.f);
	} view_info;

	struct
	{
		VkDescriptorSetLayout layout = VK_NULL_HANDLE;
		VkDescriptorSet       set    = VK_NULL_HANDLE;
	} glsl_descriptor;

	struct
	{
		VkDescriptorSetLayout layout = VK_NULL_HANDLE;
		VkDescriptorSet       set    = VK_NULL_HANDLE;
	} descriptor;

  public:
	AccelerationStructure              tlas;
	std::vector<AccelerationStructure> blas;

	struct
	{
		Buffer instance;
		Buffer light;
		Buffer emitter;
		Buffer material;
		Buffer vertex;
		Buffer index;
		Buffer indirect_draw;
		Buffer view;
		Buffer emitter_alias_table;
		Buffer mesh_alias_table;
		Buffer scene;
	} buffer;

	std::vector<Texture>     textures;
	std::vector<VkImageView> texture_views;

	Texture     ggx_lut;
	VkImageView ggx_lut_view = VK_NULL_HANDLE;

	std::array<Texture, 9>   scrambling_ranking_images;
	std::vector<VkImageView> scrambling_ranking_image_views;

	Texture     sobol_image;
	VkImageView sobol_image_view = VK_NULL_HANDLE;

	VkSampler linear_sampler  = VK_NULL_HANDLE;
	VkSampler nearest_sampler = VK_NULL_HANDLE;

	struct
	{
		Texture texture;
		Texture irradiance_sh;
		Texture prefilter_map;

		VkImageView texture_view       = VK_NULL_HANDLE;
		VkImageView irradiance_sh_view = VK_NULL_HANDLE;
		VkImageView prefilter_map_view = VK_NULL_HANDLE;
	} envmap;

  private:
	void destroy_scene();

	void destroy_envmap();

	const Context *m_context = nullptr;
};