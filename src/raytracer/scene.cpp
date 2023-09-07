#include "scene.hpp"

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <stb/stb_image.h>

#include <glm/gtc/type_ptr.hpp>

#include <spdlog/spdlog.h>

#include <filesystem>
#include <queue>

#define CUBEMAP_SIZE 1024
#define IRRADIANCE_CUBEMAP_SIZE 128
#define IRRADIANCE_WORK_GROUP_SIZE 8
#define SH_INTERMEDIATE_SIZE (IRRADIANCE_CUBEMAP_SIZE / IRRADIANCE_WORK_GROUP_SIZE)
#define CUBEMAP_FACE_NUM 6
#define PREFILTER_MAP_SIZE 256
#define PREFILTER_MIP_LEVELS 5

struct Vertex
{
	glm::vec4 position;        // xyz - position, w - texcoord u
	glm::vec4 normal;          // xyz - normal, w - texcoord v
};

struct Emitter
{
	glm::vec4 p0;
	glm::vec4 p1;
	glm::vec4 p2;
	glm::vec4 n0;
	glm::vec4 n1;
	glm::vec4 n2;
	glm::vec4 intensity;
};

struct Light
{
	glm::vec3 pos;
	uint32_t  instance_id;
};

struct Mesh
{
	uint32_t vertices_offset = 0;
	uint32_t vertices_count  = 0;
	uint32_t indices_offset  = 0;
	uint32_t indices_count   = 0;
	uint32_t material        = ~0u;
	float    area            = 0.f;
};

struct Material
{
	uint32_t  alpha_mode;        // 0 - opaque, 1 - mask, 2 - blend
	uint32_t  double_sided;
	float     cutoff;
	float     metallic_factor;
	float     roughness_factor;
	float     transmission_factor;
	float     clearcoat_factor;
	float     clearcoat_roughness_factor;
	glm::vec4 base_color;
	glm::vec3 emissive_factor;
	int32_t   base_color_texture;
	int32_t   normal_texture;
	int32_t   metallic_roughness_texture;
	glm::vec2 padding;
};

struct Instance
{
	glm::mat4 transform;
	glm::mat4 transform_inv;
	uint32_t  vertices_offset;
	uint32_t  vertices_count;
	uint32_t  indices_offset;
	uint32_t  indices_count;
	uint32_t  mesh;
	uint32_t  material;
	int32_t   emitter;
	float     area;
};

struct AliasTable
{
	float prob;         // The i's column's event i's prob
	int   alias;        // The i's column's another event's idx
	float ori_prob;
	float alias_ori_prob;
};

inline const std::string get_path_dictionary(const std::string &path)
{
	if (std::filesystem::exists(path) &&
	    std::filesystem::is_directory(path))
	{
		return path;
	}

	size_t last_index = path.find_last_of("\\/");

	if (last_index != std::string::npos)
	{
		return path.substr(0, last_index + 1);
	}

	return "";
}

inline std::vector<AliasTable> build_alias_table(std::vector<float> &probs, float total_weight)
{
	std::vector<AliasTable> alias_table(probs.size());
	std::queue<uint32_t>    greater_queue;
	std::queue<uint32_t>    smaller_queue;
	for (uint32_t i = 0; i < probs.size(); i++)
	{
		alias_table[i].ori_prob = probs[i] / total_weight;
		probs[i] *= static_cast<float>(probs.size()) / total_weight;
		if (probs[i] >= 1.f)
		{
			greater_queue.push(i);
		}
		else
		{
			smaller_queue.push(i);
		}
	}
	while (!greater_queue.empty() && !smaller_queue.empty())
	{
		uint32_t g = greater_queue.front();
		uint32_t s = smaller_queue.front();

		greater_queue.pop();
		smaller_queue.pop();

		alias_table[s].prob  = probs[s];
		alias_table[s].alias = g;

		probs[g] = (probs[s] + probs[g]) - 1.f;

		if (probs[g] < 1.f)
		{
			smaller_queue.push(g);
		}
		else
		{
			greater_queue.push(g);
		}
	}
	while (!greater_queue.empty())
	{
		uint32_t g = greater_queue.front();
		greater_queue.pop();
		alias_table[g].prob  = 1.f;
		alias_table[g].alias = g;
	}
	while (!greater_queue.empty())
	{
		uint32_t s = smaller_queue.front();
		smaller_queue.pop();
		alias_table[s].prob  = 1.f;
		alias_table[s].alias = s;
	}

	for (auto &table : alias_table)
	{
		table.alias_ori_prob = alias_table[table.alias].ori_prob;
	}

	return alias_table;
}

Scene::Scene(const Context &context) :
    m_context(&context)
{
	static const char *scrambling_ranking_textures[] = {
	    PROJECT_DIR "assets/textures/blue_noise/scrambling_ranking_128x128_2d_1spp.png",
	    PROJECT_DIR "assets/textures/blue_noise/scrambling_ranking_128x128_2d_2spp.png",
	    PROJECT_DIR "assets/textures/blue_noise/scrambling_ranking_128x128_2d_4spp.png",
	    PROJECT_DIR "assets/textures/blue_noise/scrambling_ranking_128x128_2d_8spp.png",
	    PROJECT_DIR "assets/textures/blue_noise/scrambling_ranking_128x128_2d_16spp.png",
	    PROJECT_DIR "assets/textures/blue_noise/scrambling_ranking_128x128_2d_32spp.png",
	    PROJECT_DIR "assets/textures/blue_noise/scrambling_ranking_128x128_2d_64spp.png",
	    PROJECT_DIR "assets/textures/blue_noise/scrambling_ranking_128x128_2d_128spp.png",
	    PROJECT_DIR "assets/textures/blue_noise/scrambling_ranking_128x128_2d_256spp.png",
	};

	scrambling_ranking_image_views.resize(9);
	for (size_t i = 0; i < 9; i++)
	{
		scrambling_ranking_images[i]      = m_context->load_texture_2d(scrambling_ranking_textures[i]);
		scrambling_ranking_image_views[i] = m_context->create_texture_view(fmt::format("{} - View", scrambling_ranking_textures[i]), scrambling_ranking_images[i].vk_image, VK_FORMAT_R8G8B8A8_UNORM);
	}

	sobol_image      = m_context->load_texture_2d(PROJECT_DIR "assets/textures/blue_noise/sobol_256_4d.png");
	sobol_image_view = m_context->create_texture_view("Sobel Image view", sobol_image.vk_image, VK_FORMAT_R8G8B8A8_UNORM);

	ggx_lut      = m_context->load_texture_2d(PROJECT_DIR "assets/textures/lut/brdf_lut.png");
	ggx_lut_view = m_context->create_texture_view("LUT view", ggx_lut.vk_image, VK_FORMAT_R8G8B8A8_UNORM);

	buffer.view = m_context->create_buffer("View Buffer", sizeof(view_info), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	m_context->buffer_copy_to_device(buffer.view, &view_info, sizeof(view_info));

	linear_sampler  = m_context->create_sampler(VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT);
	nearest_sampler = m_context->create_sampler(VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT);

	glsl_descriptor.layout = m_context->create_descriptor_layout()
	                             // View Buffer
	                             .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                             // TLAS
	                             .add_descriptor_binding(1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                             // Scene Buffer
	                             .add_descriptor_binding(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                             // Textures
	                             .add_descriptor_bindless_binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                             // Envmap Texture
	                             .add_descriptor_binding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                             // Irradiance SH Texture
	                             .add_descriptor_binding(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                             // Prefilter Map Texture
	                             .add_descriptor_binding(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                             // GGX LUT
	                             .add_descriptor_binding(7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                             // Sobel Image
	                             .add_descriptor_binding(8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                             // Scrambling Ranking Image
	                             .add_descriptor_bindless_binding(9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                             .create();

	glsl_descriptor.set = m_context->allocate_descriptor_set({glsl_descriptor.layout});

	descriptor.layout = m_context->create_descriptor_layout()
	                        // TLAS
	                        .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                        // Instance Buffer
	                        .add_descriptor_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                        // Emitter Buffer
	                        .add_descriptor_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                        // Material Buffer
	                        .add_descriptor_binding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                        // Vertex Buffer
	                        .add_descriptor_binding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                        // Index Buffer
	                        .add_descriptor_binding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                        // View Buffer
	                        .add_descriptor_binding(6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                        // Emitter Alias Table Buffer
	                        .add_descriptor_binding(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                        // Mesh Alias Table Buffer
	                        .add_descriptor_binding(8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                        // Scene Buffer
	                        .add_descriptor_binding(9, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                        // Textures
	                        .add_descriptor_bindless_binding(10, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                        // Samplers
	                        .add_descriptor_bindless_binding(11, VK_DESCRIPTOR_TYPE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                        // Env Map
	                        .add_descriptor_binding(12, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                        // Irradiance SH
	                        .add_descriptor_binding(13, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                        // Prefilter Map
	                        .add_descriptor_binding(14, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                        // GGX Lut
	                        .add_descriptor_binding(15, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                        // Sobel Sequence
	                        .add_descriptor_binding(16, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                        // Scrambling Ranking Tile
	                        .add_descriptor_bindless_binding(17, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_ALL_GRAPHICS)
	                        .create();

	descriptor.set = m_context->allocate_descriptor_set({descriptor.layout});
}

Scene::~Scene()
{
	m_context->wait();
	m_context->destroy(descriptor.layout)
	    .destroy(descriptor.set)
	    .destroy(glsl_descriptor.layout)
	    .destroy(glsl_descriptor.set)
	    .destroy(buffer.view)
	    .destroy(ggx_lut)
	    .destroy(ggx_lut_view)
	    .destroy(scrambling_ranking_images)
	    .destroy(scrambling_ranking_image_views)
	    .destroy(sobol_image)
	    .destroy(sobol_image_view)
	    .destroy(linear_sampler)
	    .destroy(nearest_sampler);
	destroy_scene();
	destroy_envmap();
}

void Scene::load_scene(const std::string &filename)
{
	m_context->wait();
	destroy_scene();

	cgltf_options options  = {};
	cgltf_data   *raw_data = nullptr;
	cgltf_result  result   = cgltf_parse_file(&options, filename.c_str(), &raw_data);
	if (result != cgltf_result_success)
	{
		spdlog::error("Failed to load gltf {}", filename);
		return;
	}
	result = cgltf_load_buffers(&options, raw_data, filename.c_str());
	if (result != cgltf_result_success)
	{
		spdlog::error("Failed to load gltf {}", filename);
		return;
	}

	std::unordered_map<cgltf_texture *, uint32_t>           texture_map;
	std::unordered_map<cgltf_material *, uint32_t>          material_map;
	std::unordered_map<cgltf_mesh *, std::vector<uint32_t>> mesh_map;

	std::vector<Emitter>  emitters;
	std::vector<Light>    lights;
	std::vector<Material> materials;
	std::vector<Mesh>     meshes;
	std::vector<Instance> instances;
	std::vector<uint32_t> indices;
	std::vector<Vertex>   vertices;

	auto load_texture = [&](cgltf_texture *gltf_texture) -> int32_t {
		if (!gltf_texture)
		{
			return -1;
		}
		if (texture_map.find(gltf_texture) != texture_map.end())
		{
			return texture_map.at(gltf_texture);
		}

		uint8_t *raw_data = nullptr;
		size_t   raw_size = 0;
		int32_t  width = 0, height = 0, channel = 0, req_channel = 4;

		std::string name = "";

		if (gltf_texture->image->uri)
		{
			// Load external texture
			std::string path = get_path_dictionary(filename) + gltf_texture->image->uri;
			raw_data         = stbi_load(path.c_str(), &width, &height, &channel, req_channel);
			name             = gltf_texture->image->uri;
		}
		else if (gltf_texture->image->buffer_view)
		{
			// Load internal texture
			uint8_t *data = static_cast<uint8_t *>(gltf_texture->image->buffer_view->buffer->data) + gltf_texture->image->buffer_view->offset;
			size_t   size = gltf_texture->image->buffer_view->size;

			raw_data = stbi_load_from_memory(static_cast<stbi_uc *>(data), static_cast<int32_t>(size), &width, &height, &channel, req_channel);

			name = fmt::format("GLTF Texture #{}", textures.size());
		}

		raw_size = static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(req_channel) * sizeof(uint8_t);

		uint32_t mip_level      = static_cast<uint32_t>(std::floor(std::log2(std::max(width, height))) + 1);
		Texture  image          = m_context->create_texture_2d(name, (uint32_t) width, (uint32_t) height, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, true);
		Buffer   staging_buffer = m_context->create_buffer("Image Staging Buffer", raw_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
		m_context->buffer_copy_to_device(staging_buffer, raw_data, raw_size);
		m_context->record_command()
		    .begin()
		    .insert_barrier()
		    .add_image_barrier(
		        image.vk_image,
		        0, VK_ACCESS_TRANSFER_WRITE_BIT,
		        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		        {
		            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		            .baseMipLevel   = 0,
		            .levelCount     = mip_level,
		            .baseArrayLayer = 0,
		            .layerCount     = 1,
		        })
		    .insert()
		    .copy_buffer_to_image(staging_buffer.vk_buffer, image.vk_image, {(uint32_t) width, (uint32_t) height, 1})
		    .insert_barrier()
		    .add_image_barrier(
		        image.vk_image,
		        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
		        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		        {
		            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		            .baseMipLevel   = 0,
		            .levelCount     = mip_level,
		            .baseArrayLayer = 0,
		            .layerCount     = 1,
		        })
		    .insert()
		    .generate_mipmap(image.vk_image, (uint32_t) width, (uint32_t) height, mip_level)
		    .insert_barrier()
		    .add_image_barrier(
		        image.vk_image,
		        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
		        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		        {
		            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		            .baseMipLevel   = 0,
		            .levelCount     = mip_level,
		            .baseArrayLayer = 0,
		            .layerCount     = 1,
		        })
		    .insert()
		    .end()
		    .flush();
		m_context->destroy(staging_buffer);

		stbi_image_free(raw_data);
		textures.push_back(image);
		texture_map[gltf_texture] = static_cast<uint32_t>(textures.size() - 1);

		texture_views.push_back(m_context->create_texture_view(
		    name + " - View",
		    image.vk_image,
		    VK_FORMAT_R8G8B8A8_UNORM,
		    VK_IMAGE_VIEW_TYPE_2D,
		    {
		        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		        .baseMipLevel   = 0,
		        .levelCount     = mip_level,
		        .baseArrayLayer = 0,
		        .layerCount     = 1,
		    }));

		return texture_map.at(gltf_texture);
	};

	// Load material
	{
		for (size_t i = 0; i < raw_data->materials_count; i++)
		{
			auto    &raw_material = raw_data->materials[i];
			Material material     = {};

			material.normal_texture = load_texture(raw_material.normal_texture.texture);
			material.double_sided   = raw_material.double_sided;
			material.alpha_mode     = raw_material.alpha_mode;
			material.cutoff         = raw_material.alpha_cutoff;
			std::memcpy(glm::value_ptr(material.emissive_factor), raw_material.emissive_factor, sizeof(glm::vec3));
			if (raw_material.has_pbr_metallic_roughness)
			{
				material.metallic_factor  = raw_material.pbr_metallic_roughness.metallic_factor;
				material.roughness_factor = raw_material.pbr_metallic_roughness.roughness_factor;
				std::memcpy(glm::value_ptr(material.base_color), raw_material.pbr_metallic_roughness.base_color_factor, sizeof(glm::vec4));
				material.base_color_texture         = load_texture(raw_material.pbr_metallic_roughness.base_color_texture.texture);
				material.metallic_roughness_texture = load_texture(raw_material.pbr_metallic_roughness.metallic_roughness_texture.texture);
			}
			if (raw_material.has_clearcoat)
			{
				material.clearcoat_factor           = raw_material.clearcoat.clearcoat_factor;
				material.clearcoat_roughness_factor = raw_material.clearcoat.clearcoat_roughness_factor;
			}
			if (raw_material.has_transmission)
			{
				material.transmission_factor = raw_material.transmission.transmission_factor;
			}

			materials.emplace_back(material);
			material_map[&raw_material] = static_cast<uint32_t>(materials.size() - 1);
		}

		// Create material buffer
		{
			buffer.material = m_context->create_buffer("Material Buffer", materials.size() * sizeof(Material), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
			m_context->buffer_copy_to_device(buffer.material, materials.data(), materials.size() * sizeof(Material), true);

			scene_info.material_count       = static_cast<uint32_t>(materials.size());
			scene_info.material_buffer_addr = buffer.material.device_address;
		}
	}

	// Load geometry
	{
		for (uint32_t mesh_id = 0; mesh_id < raw_data->meshes_count; mesh_id++)
		{
			auto &raw_mesh      = raw_data->meshes[mesh_id];
			mesh_map[&raw_mesh] = {};
			for (uint32_t prim_id = 0; prim_id < raw_mesh.primitives_count; prim_id++)
			{
				const cgltf_primitive &primitive = raw_mesh.primitives[prim_id];

				if (!primitive.indices)
				{
					continue;
				}

				Mesh mesh = {
				    .vertices_offset = static_cast<uint32_t>(vertices.size()),
				    .vertices_count  = 0,
				    .indices_offset  = static_cast<uint32_t>(indices.size()),
				    .indices_count   = static_cast<uint32_t>(primitive.indices->count),
				    .material        = material_map.at(primitive.material),
				    .area            = 0.f,
				};

				indices.resize(indices.size() + primitive.indices->count);
				for (size_t i = 0; i < primitive.indices->count; i++)
				{
					indices[mesh.indices_offset + i] = static_cast<uint32_t>(cgltf_accessor_read_index(primitive.indices, i));
				}

				for (size_t attr_id = 0; attr_id < primitive.attributes_count; attr_id++)
				{
					const cgltf_attribute &attribute = primitive.attributes[attr_id];

					const char *attr_name = attribute.name;

					if (strcmp(attr_name, "POSITION") == 0)
					{
						mesh.vertices_count = std::max(mesh.vertices_count, static_cast<uint32_t>(attribute.data->count));
						vertices.resize(mesh.vertices_offset + attribute.data->count);
						for (size_t i = 0; i < attribute.data->count; ++i)
						{
							cgltf_accessor_read_float(attribute.data, i, &vertices[mesh.vertices_offset + i].position.x, 3);
						}
					}
					else if (strcmp(attr_name, "NORMAL") == 0)
					{
						mesh.vertices_count = std::max(mesh.vertices_count, static_cast<uint32_t>(attribute.data->count));
						vertices.resize(mesh.vertices_offset + attribute.data->count);
						for (size_t i = 0; i < attribute.data->count; ++i)
						{
							cgltf_accessor_read_float(attribute.data, i, &vertices[mesh.vertices_offset + i].normal.x, 3);
						}
					}
					else if (strcmp(attr_name, "TEXCOORD_0") == 0)
					{
						mesh.vertices_count = std::max(mesh.vertices_count, static_cast<uint32_t>(attribute.data->count));
						vertices.resize(mesh.vertices_offset + attribute.data->count);
						for (size_t i = 0; i < attribute.data->count; ++i)
						{
							glm::vec2 texcoord = glm::vec2(0);
							cgltf_accessor_read_float(attribute.data, i, &texcoord.x, 2);
							vertices[mesh.vertices_offset + i].position.w = texcoord.x;
							vertices[mesh.vertices_offset + i].normal.w   = texcoord.y;
						}
					}
				}

				meshes.push_back(mesh);
				mesh_map[&raw_mesh].push_back(static_cast<uint32_t>(meshes.size() - 1));
			}
		}

		scene_info.vertices_count = static_cast<uint32_t>(vertices.size());
		scene_info.indices_count  = static_cast<uint32_t>(indices.size());
		scene_info.mesh_count     = static_cast<uint32_t>(meshes.size());

		buffer.vertex = m_context->create_buffer("Vertex Buffer", sizeof(Vertex) * vertices.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
		m_context->buffer_copy_to_device(buffer.vertex, vertices.data(), sizeof(Vertex) * vertices.size(), true);
		scene_info.vertex_buffer_addr = buffer.vertex.device_address;

		buffer.index = m_context->create_buffer("Index Buffer", sizeof(uint32_t) * indices.size(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
		m_context->buffer_copy_to_device(buffer.index, indices.data(), sizeof(uint32_t) * indices.size(), true);
		scene_info.index_buffer_addr = buffer.index.device_address;

		// Build mesh alias table buffer
		{
			std::vector<AliasTable> alias_table;
			for (uint32_t i = 0; i < meshes.size(); i++)
			{
				float              total_weight = 0.f;
				std::vector<float> mesh_probs(meshes[i].indices_count / 3);
				for (uint32_t j = 0; j < meshes[i].indices_count / 3; j++)
				{
					glm::vec3 v0 = vertices[meshes[i].vertices_offset + indices[meshes[i].indices_offset + 3 * j + 0]].position;
					glm::vec3 v1 = vertices[meshes[i].vertices_offset + indices[meshes[i].indices_offset + 3 * j + 1]].position;
					glm::vec3 v2 = vertices[meshes[i].vertices_offset + indices[meshes[i].indices_offset + 3 * j + 2]].position;
					mesh_probs[j] += glm::length(glm::cross(v1 - v0, v2 - v1)) * 0.5f;
					total_weight += mesh_probs[j];
				}
				meshes[i].area = total_weight;

				std::vector<AliasTable> mesh_alias_table = build_alias_table(mesh_probs, total_weight);
				alias_table.insert(alias_table.end(), std::make_move_iterator(mesh_alias_table.begin()), std::make_move_iterator(mesh_alias_table.end()));
			}
			buffer.mesh_alias_table = m_context->create_buffer("Mesh Alias Table Buffer", sizeof(AliasTable) * alias_table.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
			m_context->buffer_copy_to_device(buffer.mesh_alias_table, alias_table.data(), sizeof(AliasTable) * alias_table.size(), true);
			scene_info.mesh_alias_table_buffer_addr = buffer.mesh_alias_table.device_address;
		}

		// Load hierarchy
		{
			for (size_t i = 0; i < raw_data->nodes_count; i++)
			{
				const cgltf_node &node = raw_data->nodes[i];

				cgltf_float matrix[16];
				cgltf_node_transform_world(&node, matrix);
				if (node.mesh)
				{
					for (auto &mesh_id : mesh_map.at(node.mesh))
					{
						const auto &mesh = meshes[mesh_id];

						Instance instance = {
						    .vertices_offset = mesh.vertices_offset,
						    .vertices_count  = mesh.vertices_offset,
						    .indices_offset  = mesh.indices_offset,
						    .indices_count   = mesh.indices_count,
						    .mesh            = mesh_id,
						    .material        = mesh.material,
						    .area            = mesh.area,
						};
						std::memcpy(glm::value_ptr(instance.transform), matrix, sizeof(instance.transform));
						instance.transform_inv = glm::inverse(instance.transform);
						int32_t emitter_offset = static_cast<int32_t>(emitters.size());
						if (materials[mesh.material].emissive_factor != glm::vec3(0.f))
						{
							lights.push_back(Light{
							    .pos         = glm::vec3(instance.transform[3]),
							    .instance_id = mesh_id,
							});
							for (uint32_t tri_idx = 0; tri_idx < mesh.indices_count / 3; tri_idx++)
							{
								const uint32_t i0 = indices[mesh.indices_offset + tri_idx * 3 + 0];
								const uint32_t i1 = indices[mesh.indices_offset + tri_idx * 3 + 1];
								const uint32_t i2 = indices[mesh.indices_offset + tri_idx * 3 + 2];

								glm::mat3 normal_mat = glm::mat3(glm::transpose(glm::inverse(instance.transform)));

								emitters.push_back(Emitter{
								    .p0        = instance.transform * glm::vec4(glm::vec3(vertices[mesh.vertices_offset + i0].position), 1.f),
								    .p1        = instance.transform * glm::vec4(glm::vec3(vertices[mesh.vertices_offset + i1].position), 1.f),
								    .p2        = instance.transform * glm::vec4(glm::vec3(vertices[mesh.vertices_offset + i2].position), 1.f),
								    .n0        = glm::vec4(glm::normalize(normal_mat * glm::vec3(vertices[mesh.vertices_offset + i0].normal)), 0),
								    .n1        = glm::vec4(glm::normalize(normal_mat * glm::vec3(vertices[mesh.vertices_offset + i1].normal)), 0),
								    .n2        = glm::vec4(glm::normalize(normal_mat * glm::vec3(vertices[mesh.vertices_offset + i2].normal)), 0),
								    .intensity = glm::vec4(materials[mesh.material].emissive_factor, 0.f),
								});
							}
							instance.emitter = emitter_offset;
						}
						else
						{
							instance.emitter = -1;
						}
						instances.push_back(instance);
					}
				}
			}
			scene_info.instance_count = static_cast<uint32_t>(instances.size());

			// Compute scene extent
			{
				for (auto &instance : instances)
				{
					const auto &mesh = meshes[instance.mesh];
					for (uint32_t vertex_id = 0; vertex_id < mesh.vertices_count; vertex_id++)
					{
						glm::vec3 v           = vertices[vertex_id + mesh.vertices_offset].position;
						v                     = instance.transform * glm::vec4(v, 1.f);
						scene_info.max_extent = glm::max(scene_info.max_extent, v);
						scene_info.min_extent = glm::min(scene_info.min_extent, v);
					}
				}
			}

			// Build emitter buffer
			{
				buffer.emitter = m_context->create_buffer("Emitter Buffer", std::max(emitters.size(), 1ull) * sizeof(Emitter), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
				if (!emitters.empty())
				{
					m_context->buffer_copy_to_device(buffer.emitter, emitters.data(), emitters.size() * sizeof(Emitter), true);
				}
				scene_info.emitter_count       = static_cast<uint32_t>(emitters.size());
				scene_info.emitter_buffer_addr = buffer.emitter.device_address;
			}

			// Build light buffer
			{
				buffer.light = m_context->create_buffer("Light Buffer", std::max(lights.size(), 1ull) * sizeof(Light), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
				if (!lights.empty())
				{
					m_context->buffer_copy_to_device(buffer.light, lights.data(), lights.size() * sizeof(Light), true);
				}
				//scene_info.light_count = static_cast<uint32_t>(lights.size());
			}

			// Build emitter alias table buffer
			{
				float              total_weight = 0.f;
				std::vector<float> emitter_probs(emitters.size());
				for (uint32_t i = 0; i < emitters.size(); i++)
				{
					const auto &emitter = emitters[i];

					float area = glm::length(glm::cross(glm::vec3(emitter.p1 - emitter.p0), glm::vec3(emitter.p2 - emitter.p1))) * 0.5f;

					emitter_probs[i] = glm::dot(glm::vec3(emitter.intensity), glm::vec3(0.212671f, 0.715160f, 0.072169f)) * area;
					total_weight += emitter_probs[i];
				}

				std::vector<AliasTable> alias_table = build_alias_table(emitter_probs, total_weight);
				buffer.emitter_alias_table          = m_context->create_buffer("Emitter Alias Table", std::max(alias_table.size(), 1ull) * sizeof(AliasTable), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
				if (!alias_table.empty())
				{
					m_context->buffer_copy_to_device(buffer.emitter_alias_table, alias_table.data(), alias_table.size() * sizeof(AliasTable), true);
				}
				scene_info.emitter_alias_table_buffer_addr = buffer.emitter_alias_table.device_address;
			}

			// Build draw indirect command buffer
			{
				std::vector<VkDrawIndexedIndirectCommand> indirect_commands;
				indirect_commands.resize(instances.size());
				for (uint32_t instance_id = 0; instance_id < instances.size(); instance_id++)
				{
					const auto &mesh               = meshes[instances[instance_id].mesh];
					indirect_commands[instance_id] = VkDrawIndexedIndirectCommand{
					    .indexCount    = mesh.indices_count,
					    .instanceCount = 1,
					    .firstIndex    = mesh.indices_offset,
					    .vertexOffset  = static_cast<int32_t>(mesh.vertices_offset),
					    .firstInstance = instance_id,
					};
				}
				buffer.indirect_draw = m_context->create_buffer("Indirect Draw Buffer", indirect_commands.size() * sizeof(VkDrawIndexedIndirectCommand), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
				m_context->buffer_copy_to_device(buffer.indirect_draw, indirect_commands.data(), indirect_commands.size() * sizeof(VkDrawIndexedIndirectCommand), true);
			}

			// Create instance buffer
			buffer.instance = m_context->create_buffer("Instance Buffer", instances.size() * sizeof(Instance), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
			m_context->buffer_copy_to_device(buffer.instance, instances.data(), instances.size() * sizeof(Instance), true);
			scene_info.instance_buffer_addr = buffer.instance.device_address;
		}

		// Build acceleration structure
		{
			std::vector<Buffer> scratch_buffers;
			// Build bottom level acceleration structure
			{
				blas.reserve(meshes.size());
				for (uint32_t mesh_id = 0; mesh_id < meshes.size(); mesh_id++)
				{
					const auto &mesh = meshes[mesh_id];

					VkAccelerationStructureGeometryKHR as_geometry = {
					    .sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
					    .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
					    .geometry     = {
					            .triangles = {
					                .sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
					                .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
					                .vertexData   = {
					                      .deviceAddress = buffer.vertex.device_address,
                                },
					                .vertexStride = sizeof(Vertex),
					                .maxVertex    = mesh.vertices_count,
					                .indexType    = VK_INDEX_TYPE_UINT32,
					                .indexData    = {
					                       .deviceAddress = buffer.index.device_address,
                                },
					                .transformData = {
					                    .deviceAddress = 0,
                                },
                            },
                        },
					    .flags = VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR,
					};

					VkAccelerationStructureBuildRangeInfoKHR range_info = {
					    .primitiveCount  = mesh.indices_count / 3,
					    .primitiveOffset = mesh.indices_offset * sizeof(uint32_t),
					    .firstVertex     = mesh.vertices_offset,
					    .transformOffset = 0,
					};

					VkAccelerationStructureBuildGeometryInfoKHR build_geometry_info = {
					    .sType                    = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
					    .type                     = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
					    .flags                    = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
					    .mode                     = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
					    .srcAccelerationStructure = VK_NULL_HANDLE,
					    .geometryCount            = 1,
					    .pGeometries              = &as_geometry,
					    .ppGeometries             = nullptr,
					    .scratchData              = {
					                     .deviceAddress = 0,
                        },
					};

					VkAccelerationStructureBuildSizesInfoKHR build_sizes_info = {
					    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
					};

					vkGetAccelerationStructureBuildSizesKHR(
					    m_context->vk_device,
					    VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
					    &build_geometry_info,
					    &range_info.primitiveCount,
					    &build_sizes_info);

					auto [as, scratch_buffer] = m_context->create_acceleration_structure(fmt::format("BLAS #{}", mesh_id), VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, as_geometry, range_info);

					blas.push_back(as);
					scratch_buffers.push_back(scratch_buffer);
				}
			}

			// Build top level acceleration structure
			{
				std::vector<VkAccelerationStructureInstanceKHR> vk_instances;
				vk_instances.reserve(instances.size());
				for (uint32_t instance_id = 0; instance_id < instances.size(); instance_id++)
				{
					const auto &instance  = instances[instance_id];
					auto        transform = glm::mat3x4(glm::transpose(instance.transform));

					VkTransformMatrixKHR transform_matrix = {};
					std::memcpy(&transform_matrix, &transform, sizeof(VkTransformMatrixKHR));

					VkAccelerationStructureInstanceKHR vk_instance = {
					    .transform                              = transform_matrix,
					    .instanceCustomIndex                    = instance_id,
					    .mask                                   = 0xFF,
					    .instanceShaderBindingTableRecordOffset = 0,
					    .flags                                  = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR,
					    .accelerationStructureReference         = blas.at(instance.mesh).device_address,
					};

					const Material &material = materials[instance.material];

					if (material.alpha_mode == 0 ||
					    (material.base_color.w == 1.f &&
					     material.base_color_texture == ~0u))
					{
						vk_instance.flags |= VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
					}

					if (material.double_sided == 1)
					{
						vk_instance.flags |= VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
					}

					vk_instances.emplace_back(vk_instance);
				}

				Buffer instance_buffer = m_context->create_buffer("Instance Stratch Buffer", vk_instances.size() * sizeof(VkAccelerationStructureInstanceKHR) + 16, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
				m_context->buffer_copy_to_device(instance_buffer, vk_instances.data(), vk_instances.size() * sizeof(VkAccelerationStructureInstanceKHR), true, 16 - instance_buffer.device_address % 16);

				VkAccelerationStructureGeometryKHR as_geometry = {
				    .sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
				    .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
				    .geometry     = {
				            .instances = {
				                .sType           = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
				                .arrayOfPointers = VK_FALSE,
				                .data            = instance_buffer.device_address + (16 - instance_buffer.device_address % 16),
                        },
                    },
				    .flags = 0,
				};

				VkAccelerationStructureBuildRangeInfoKHR range_info = {
				    .primitiveCount = static_cast<uint32_t>(vk_instances.size()),
				};

				Buffer scratch_buffer;
				std::tie(tlas, scratch_buffer) = m_context->create_acceleration_structure("TLAS", VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, as_geometry, range_info);
				scratch_buffers.push_back(instance_buffer);
				scratch_buffers.push_back(scratch_buffer);
			}

			m_context->destroy(scratch_buffers);
		}

		buffer.scene = m_context->create_buffer("Scene Buffer", sizeof(scene_info), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
		m_context->buffer_copy_to_device(buffer.scene, &scene_info, sizeof(scene_info), true);
	}
}

void Scene::load_envmap(const std::string &filename)
{
	m_context->wait();
	destroy_envmap();

	int32_t width = 0, height = 0, channel = 0, req_channel = 4;

	float *raw_data = stbi_loadf(filename.c_str(), &width, &height, &channel, req_channel);
	size_t raw_size = static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(req_channel) * sizeof(float);

	Texture hdr_texture = m_context->create_texture_2d(
	    "HDRTexture",
	    (uint32_t) width, (uint32_t) height,
	    VK_FORMAT_R32G32B32A32_SFLOAT,
	    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

	VkImageView hdr_texture_view = m_context->create_texture_view(
	    "HDRTexture View",
	    hdr_texture.vk_image,
	    VK_FORMAT_R32G32B32A32_SFLOAT);

	Buffer staging_buffer = m_context->create_buffer(
	    "Staging Buffer",
	    raw_size,
	    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	    VMA_MEMORY_USAGE_CPU_TO_GPU);

	m_context->buffer_copy_to_device(staging_buffer, raw_data, raw_size);

	envmap.texture = m_context->create_texture_cube(
	    "Envmap Texture",
	    CUBEMAP_SIZE, CUBEMAP_SIZE,
	    VK_FORMAT_R32G32B32A32_SFLOAT,
	    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
	    5);

	envmap.texture_view = m_context->create_texture_view(
	    "Envmap Texture View",
	    envmap.texture.vk_image,
	    VK_FORMAT_R32G32B32A32_SFLOAT,
	    VK_IMAGE_VIEW_TYPE_CUBE,
	    {
	        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	        .baseMipLevel   = 0,
	        .levelCount     = 5,
	        .baseArrayLayer = 0,
	        .layerCount     = 6,
	    });

	Texture sh_intermediate = m_context->create_texture_2d_array(
	    "SH Intermediate",
	    SH_INTERMEDIATE_SIZE * 9, SH_INTERMEDIATE_SIZE, 6,
	    VK_FORMAT_R32G32B32A32_SFLOAT,
	    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);

	VkImageView sh_intermediate_view = m_context->create_texture_view(
	    "Envmap Texture View",
	    sh_intermediate.vk_image,
	    VK_FORMAT_R32G32B32A32_SFLOAT,
	    VK_IMAGE_VIEW_TYPE_2D_ARRAY,
	    {
	        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	        .baseMipLevel   = 0,
	        .levelCount     = 1,
	        .baseArrayLayer = 0,
	        .layerCount     = 6,
	    });

	envmap.irradiance_sh = m_context->create_texture_2d(
	    "Irradiance SH",
	    9, 1,
	    VK_FORMAT_R32G32B32A32_SFLOAT,
	    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);

	envmap.irradiance_sh_view = m_context->create_texture_view(
	    "Irradiance SH View",
	    envmap.irradiance_sh.vk_image,
	    VK_FORMAT_R32G32B32A32_SFLOAT);

	envmap.prefilter_map = m_context->create_texture_cube(
	    "Envmap Prefilter Map",
	    PREFILTER_MAP_SIZE, PREFILTER_MAP_SIZE,
	    VK_FORMAT_R32G32B32A32_SFLOAT,
	    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
	    PREFILTER_MIP_LEVELS);

	envmap.prefilter_map_view = m_context->create_texture_view(
	    "Prefilter Map View",
	    envmap.prefilter_map.vk_image,
	    VK_FORMAT_R32G32B32A32_SFLOAT,
	    VK_IMAGE_VIEW_TYPE_CUBE,
	    {
	        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	        .baseMipLevel   = 0,
	        .levelCount     = PREFILTER_MIP_LEVELS,
	        .baseArrayLayer = 0,
	        .layerCount     = CUBEMAP_FACE_NUM,
	    });

	std::array<VkImageView, PREFILTER_MIP_LEVELS> prefilter_map_views;
	for (uint32_t i = 0; i < PREFILTER_MIP_LEVELS; i++)
	{
		prefilter_map_views[i] = m_context->create_texture_view(
		    fmt::format("Prefilter Map View Array 2D - {}", i),
		    envmap.prefilter_map.vk_image,
		    VK_FORMAT_R32G32B32A32_SFLOAT,
		    VK_IMAGE_VIEW_TYPE_2D_ARRAY,
		    {
		        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		        .baseMipLevel   = i,
		        .levelCount     = 1,
		        .baseArrayLayer = 0,
		        .layerCount     = CUBEMAP_FACE_NUM,
		    });
	}

	// Equirectangular to cubemap
	struct
	{
		VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
		VkDescriptorSet       descriptor_set        = VK_NULL_HANDLE;
		VkPipelineLayout      pipeline_layout       = VK_NULL_HANDLE;
		VkPipeline            pipeline              = VK_NULL_HANDLE;
	} equirectangular_to_cubemap;

	equirectangular_to_cubemap.descriptor_set_layout = m_context->create_descriptor_layout()
	                                                       .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
	                                                       .create();
	equirectangular_to_cubemap.descriptor_set  = m_context->allocate_descriptor_set(equirectangular_to_cubemap.descriptor_set_layout);
	equirectangular_to_cubemap.pipeline_layout = m_context->create_pipeline_layout({equirectangular_to_cubemap.descriptor_set_layout});
	equirectangular_to_cubemap.pipeline        = m_context->create_graphics_pipeline(equirectangular_to_cubemap.pipeline_layout)
	                                          .add_shader(VK_SHADER_STAGE_VERTEX_BIT, "equirectangular_to_cubemap.slang", "vs_main")
	                                          .add_shader(VK_SHADER_STAGE_FRAGMENT_BIT, "equirectangular_to_cubemap.slang", "fs_main")
	                                          .add_color_attachment(VK_FORMAT_R32G32B32A32_SFLOAT)
	                                          .add_viewport({
	                                              .x        = 0,
	                                              .y        = 0,
	                                              .width    = (float) CUBEMAP_SIZE,
	                                              .height   = (float) CUBEMAP_SIZE,
	                                              .minDepth = 0.f,
	                                              .maxDepth = 1.f,
	                                          })
	                                          .add_scissor({.offset = {0, 0}, .extent = {CUBEMAP_SIZE, CUBEMAP_SIZE}})
	                                          .create();

	m_context->update_descriptor()
	    .write_combine_sampled_images(0, linear_sampler, {hdr_texture_view})
	    .update(equirectangular_to_cubemap.descriptor_set);

	// Cubemap sh projection
	struct
	{
		VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
		VkDescriptorSet       descriptor_set        = VK_NULL_HANDLE;
		VkPipelineLayout      pipeline_layout       = VK_NULL_HANDLE;
		VkPipeline            pipeline              = VK_NULL_HANDLE;
	} cubemap_sh_projection;

	cubemap_sh_projection.descriptor_set_layout = m_context->create_descriptor_layout()
	                                                  .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT)
	                                                  .add_descriptor_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                                                  .create();
	cubemap_sh_projection.descriptor_set  = m_context->allocate_descriptor_set(cubemap_sh_projection.descriptor_set_layout);
	cubemap_sh_projection.pipeline_layout = m_context->create_pipeline_layout({cubemap_sh_projection.descriptor_set_layout});
	cubemap_sh_projection.pipeline        = m_context->create_compute_pipeline("cubemap_sh_projection.slang", cubemap_sh_projection.pipeline_layout);

	m_context->update_descriptor()
	    .write_combine_sampled_images(0, linear_sampler, {envmap.texture_view})
	    .write_storage_images(1, {sh_intermediate_view})
	    .update(cubemap_sh_projection.descriptor_set);

	// Cubemap sh add pass
	struct
	{
		VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
		VkDescriptorSet       descriptor_set        = VK_NULL_HANDLE;
		VkPipelineLayout      pipeline_layout       = VK_NULL_HANDLE;
		VkPipeline            pipeline              = VK_NULL_HANDLE;
	} cubemap_sh_add;

	cubemap_sh_add.descriptor_set_layout = m_context->create_descriptor_layout()
	                                           .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                                           .add_descriptor_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                                           .create();
	cubemap_sh_add.descriptor_set  = m_context->allocate_descriptor_set(cubemap_sh_add.descriptor_set_layout);
	cubemap_sh_add.pipeline_layout = m_context->create_pipeline_layout({cubemap_sh_add.descriptor_set_layout});
	cubemap_sh_add.pipeline        = m_context->create_compute_pipeline("cubemap_sh_add.slang", cubemap_sh_add.pipeline_layout);

	m_context->update_descriptor()
	    .write_sampled_images(0, {sh_intermediate_view})
	    .write_storage_images(1, {envmap.irradiance_sh_view})
	    .update(cubemap_sh_add.descriptor_set);

	// Cubemap prefilter
	struct
	{
		VkDescriptorSetLayout                             descriptor_set_layout = VK_NULL_HANDLE;
		std::array<VkDescriptorSet, PREFILTER_MIP_LEVELS> descriptor_sets;
		VkPipelineLayout                                  pipeline_layout = VK_NULL_HANDLE;
		VkPipeline                                        pipeline        = VK_NULL_HANDLE;
	} cubemap_prefilter;

	cubemap_prefilter.descriptor_set_layout = m_context->create_descriptor_layout()
	                                              .add_descriptor_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT)
	                                              .add_descriptor_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
	                                              .create();
	cubemap_prefilter.descriptor_sets = m_context->allocate_descriptor_sets<PREFILTER_MIP_LEVELS>(cubemap_prefilter.descriptor_set_layout);
	cubemap_prefilter.pipeline_layout = m_context->create_pipeline_layout({cubemap_prefilter.descriptor_set_layout}, sizeof(int32_t), VK_SHADER_STAGE_COMPUTE_BIT);
	cubemap_prefilter.pipeline        = m_context->create_compute_pipeline("cubemap_prefilter.slang", cubemap_prefilter.pipeline_layout);

	for (uint32_t i = 0; i < PREFILTER_MIP_LEVELS; i++)
	{
		m_context->update_descriptor()
		    .write_combine_sampled_images(0, linear_sampler, {envmap.texture_view})
		    .write_storage_images(1, {prefilter_map_views[i]})
		    .update(cubemap_prefilter.descriptor_sets[i]);
	}

	m_context->record_command()
	    .begin()
	    // Copy buffer to texture
	    .begin_marker("Copy HDR Texture Data to Device")
	    .insert_barrier()
	    .add_image_barrier(
	        hdr_texture.vk_image,
	        0, VK_ACCESS_TRANSFER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
	    .insert()
	    .copy_buffer_to_image(staging_buffer.vk_buffer, hdr_texture.vk_image, {(uint32_t) width, (uint32_t) height, 1})
	    .end_marker()
	    // Equirectangular to cubemap
	    .begin_marker("Equirectangular to Cubemap")
	    .insert_barrier()
	    .add_image_barrier(
	        hdr_texture.vk_image,
	        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .add_image_barrier(
	        envmap.texture.vk_image,
	        0, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	        {
	            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	            .baseMipLevel   = 0,
	            .levelCount     = 5,
	            .baseArrayLayer = 0,
	            .layerCount     = 6,
	        })
	    .insert()
	    .bind_pipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, equirectangular_to_cubemap.pipeline)
	    .bind_descriptor_set(VK_PIPELINE_BIND_POINT_GRAPHICS, equirectangular_to_cubemap.pipeline_layout, {equirectangular_to_cubemap.descriptor_set})
	    .add_color_attachment(envmap.texture_view)
	    .begin_rendering(CUBEMAP_SIZE, CUBEMAP_SIZE, 6)
	    .draw(3, 6)
	    .end_rendering()
	    .insert_barrier()
	    .add_image_barrier(
	        envmap.texture.vk_image,
	        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
	        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	        {
	            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	            .baseMipLevel   = 0,
	            .levelCount     = 5,
	            .baseArrayLayer = 0,
	            .layerCount     = 6,
	        })
	    .add_image_barrier(
	        sh_intermediate.vk_image,
	        0, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
	        {
	            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	            .baseMipLevel   = 0,
	            .levelCount     = 1,
	            .baseArrayLayer = 0,
	            .layerCount     = 6,
	        })
	    .insert()
	    .generate_mipmap(envmap.texture.vk_image, CUBEMAP_SIZE, CUBEMAP_SIZE, 5, 6)
	    .insert_barrier()
	    .add_image_barrier(
	        envmap.texture.vk_image,
	        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        {
	            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	            .baseMipLevel   = 0,
	            .levelCount     = 5,
	            .baseArrayLayer = 0,
	            .layerCount     = 6,
	        })
	    .insert()
	    .end_marker()
	    // Cubemap sh projection
	    .begin_marker("Cubemap SH Projection")
	    .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, cubemap_sh_projection.pipeline)
	    .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, cubemap_sh_projection.pipeline_layout, {cubemap_sh_projection.descriptor_set})
	    .dispatch({IRRADIANCE_CUBEMAP_SIZE, IRRADIANCE_CUBEMAP_SIZE, 6}, {IRRADIANCE_WORK_GROUP_SIZE, IRRADIANCE_WORK_GROUP_SIZE, 1})
	    .insert_barrier()
	    .add_image_barrier(
	        envmap.irradiance_sh.vk_image,
	        0, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)
	    .add_image_barrier(
	        sh_intermediate.vk_image,
	        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        {
	            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	            .baseMipLevel   = 0,
	            .levelCount     = 1,
	            .baseArrayLayer = 0,
	            .layerCount     = 6,
	        })
	    .insert()
	    .end_marker()
	    // Cubemap sh add
	    .begin_marker("Cubemap SH Add")
	    .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, cubemap_sh_add.pipeline)
	    .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, cubemap_sh_add.pipeline_layout, {cubemap_sh_add.descriptor_set})
	    .dispatch({9, 1, 1}, {1, 1, 1})
	    .insert_barrier()
	    .add_image_barrier(
	        envmap.irradiance_sh.vk_image,
	        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	    .add_image_barrier(
	        envmap.prefilter_map.vk_image,
	        0, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
	        {
	            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	            .baseMipLevel   = 0,
	            .levelCount     = PREFILTER_MIP_LEVELS,
	            .baseArrayLayer = 0,
	            .layerCount     = 6,
	        })
	    .insert()
	    .end_marker()
	    // Cubemap prefilter
	    .begin_marker("Cubemap Prefilter")
	    .bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, cubemap_prefilter.pipeline)
	    .execute([&](CommandBufferRecorder &recorder) {
		    for (int32_t i = 0; i < PREFILTER_MIP_LEVELS; i++)
		    {
			    uint32_t mip_size = static_cast<uint32_t>(PREFILTER_MAP_SIZE * std::pow(0.5f, i));
			    recorder.push_constants(cubemap_prefilter.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, i)
			        .bind_descriptor_set(VK_PIPELINE_BIND_POINT_COMPUTE, cubemap_prefilter.pipeline_layout, {cubemap_prefilter.descriptor_sets[i]})
			        .dispatch({mip_size, mip_size, 6}, {8, 8, 1});
		    }
	    })
	    .insert_barrier()
	    .add_image_barrier(
	        envmap.prefilter_map.vk_image,
	        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        {
	            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	            .baseMipLevel   = 0,
	            .levelCount     = PREFILTER_MIP_LEVELS,
	            .baseArrayLayer = 0,
	            .layerCount     = 6,
	        })
	    .insert()
	    .end_marker()
	    .end()
	    .flush();

	m_context->destroy(hdr_texture)
	    .destroy(hdr_texture_view)
	    .destroy(staging_buffer)
	    .destroy(sh_intermediate)
	    .destroy(sh_intermediate_view)
	    .destroy(prefilter_map_views)
	    .destroy(equirectangular_to_cubemap.descriptor_set_layout)
	    .destroy(equirectangular_to_cubemap.descriptor_set)
	    .destroy(equirectangular_to_cubemap.pipeline_layout)
	    .destroy(equirectangular_to_cubemap.pipeline)
	    .destroy(cubemap_sh_projection.descriptor_set_layout)
	    .destroy(cubemap_sh_projection.descriptor_set)
	    .destroy(cubemap_sh_projection.pipeline_layout)
	    .destroy(cubemap_sh_projection.pipeline)
	    .destroy(cubemap_sh_add.descriptor_set_layout)
	    .destroy(cubemap_sh_add.descriptor_set)
	    .destroy(cubemap_sh_add.pipeline_layout)
	    .destroy(cubemap_sh_add.pipeline)
	    .destroy(cubemap_prefilter.descriptor_set_layout)
	    .destroy(cubemap_prefilter.descriptor_sets)
	    .destroy(cubemap_prefilter.pipeline_layout)
	    .destroy(cubemap_prefilter.pipeline);
}

void Scene::update_view(CommandBufferRecorder &recorder)
{
	recorder.begin_marker("Update View Buffer")
	    .insert_barrier()
	    .add_buffer_barrier(
	        buffer.view.vk_buffer,
	        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT)
	    .insert()
	    .update_buffer(buffer.view.vk_buffer, &view_info, sizeof(view_info))
	    .insert_barrier()
	    .add_buffer_barrier(
	        buffer.view.vk_buffer,
	        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
	    .insert()
	    .end_marker();
}

void Scene::update()
{
	m_context->update_descriptor()
	    .write_uniform_buffers(0, {buffer.view.vk_buffer})
	    .write_acceleration_structures(1, {tlas})
	    .write_uniform_buffers(2, {buffer.scene.vk_buffer})
	    .write_combine_sampled_images(3, linear_sampler, texture_views)
	    .write_combine_sampled_images(4, linear_sampler, {envmap.texture_view})
	    .write_combine_sampled_images(5, linear_sampler, {envmap.irradiance_sh_view})
	    .write_combine_sampled_images(6, linear_sampler, {envmap.prefilter_map_view})
	    .write_combine_sampled_images(7, linear_sampler, {ggx_lut_view})
	    .write_combine_sampled_images(8, nearest_sampler, {sobol_image_view})
	    .write_combine_sampled_images(9, nearest_sampler, scrambling_ranking_image_views)
	    .update(glsl_descriptor.set);

	m_context->update_descriptor()
	    .write_acceleration_structures(0, {tlas})
	    .write_storage_buffers(1, {buffer.instance.vk_buffer})
	    .write_storage_buffers(2, {buffer.emitter.vk_buffer})
	    .write_storage_buffers(3, {buffer.material.vk_buffer})
	    .write_storage_buffers(4, {buffer.vertex.vk_buffer})
	    .write_storage_buffers(5, {buffer.index.vk_buffer})
	    .write_uniform_buffers(6, {buffer.view.vk_buffer})
	    .write_storage_buffers(7, {buffer.emitter_alias_table.vk_buffer})
	    .write_storage_buffers(8, {buffer.mesh_alias_table.vk_buffer})
	    .write_uniform_buffers(9, {buffer.scene.vk_buffer})
	    .write_sampled_images(10, texture_views)
	    .write_samplers(11, {linear_sampler, nearest_sampler})
	    .write_sampled_images(12, {envmap.texture_view})
	    .write_sampled_images(13, {envmap.irradiance_sh_view})
	    .write_sampled_images(14, {envmap.prefilter_map_view})
	    .write_sampled_images(15, {ggx_lut_view})
	    .write_sampled_images(16, {sobol_image_view})
	    .write_sampled_images(17, {scrambling_ranking_image_views})
	    .update(descriptor.set);
}

void Scene::destroy_scene()
{
	m_context->destroy(blas)
	    .destroy(tlas)
	    .destroy(buffer.instance)
	    .destroy(buffer.light)
	    .destroy(buffer.emitter)
	    .destroy(buffer.material)
	    .destroy(buffer.vertex)
	    .destroy(buffer.index)
	    .destroy(buffer.indirect_draw)
	    .destroy(buffer.emitter_alias_table)
	    .destroy(buffer.mesh_alias_table)
	    .destroy(buffer.scene)
	    .destroy(textures)
	    .destroy(texture_views);
}

void Scene::destroy_envmap()
{
	m_context->destroy(envmap.texture)
	    .destroy(envmap.irradiance_sh)
	    .destroy(envmap.prefilter_map)
	    .destroy(envmap.texture_view)
	    .destroy(envmap.irradiance_sh_view)
	    .destroy(envmap.prefilter_map_view);
}