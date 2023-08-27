#include "scene.hpp"

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <stb/stb_image.h>

#include <glm/gtc/type_ptr.hpp>

#include <spdlog/spdlog.h>

#include <filesystem>
#include <queue>

struct Vertex
{
	glm::vec4 position;        // xyz - position, w - texcoord u
	glm::vec4 normal;          // xyz - normal, w - texcoord v
};

struct Emitter
{
	glm::vec3 p0;
	glm::vec3 p1;
	glm::vec3 p2;
	glm::vec3 n0;
	glm::vec3 n1;
	glm::vec3 n2;
	glm::vec3 intensity;
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
}

Scene::~Scene()
{
	m_context->wait();
	destroy_scene();
}

void Scene::load_scene(const std::string &filename)
{
	m_context->wait();

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

			scene_info.material_count = static_cast<uint32_t>(materials.size());
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

		buffer.index = m_context->create_buffer("Index Buffer", sizeof(uint32_t) * indices.size(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
		m_context->buffer_copy_to_device(buffer.index, indices.data(), sizeof(uint32_t) * indices.size(), true);

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
								    .n0        = glm::normalize(normal_mat * glm::vec3(vertices[mesh.vertices_offset + i0].normal)),
								    .n1        = glm::normalize(normal_mat * glm::vec3(vertices[mesh.vertices_offset + i1].normal)),
								    .n2        = glm::normalize(normal_mat * glm::vec3(vertices[mesh.vertices_offset + i2].normal)),
								    .intensity = materials[mesh.material].emissive_factor,
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
				scene_info.emitter_count = static_cast<uint32_t>(emitters.size());
			}

			// Build emitter alias table buffer
			{
				float              total_weight = 0.f;
				std::vector<float> emitter_probs(emitters.size());
				for (uint32_t i = 0; i < emitters.size(); i++)
				{
					const auto &emitter = emitters[i];

					float area = glm::length(glm::cross(emitter.p1 - emitter.p0, emitter.p2 - emitter.p1)) * 0.5f;

					emitter_probs[i] = glm::dot(glm::vec3(emitter.intensity), glm::vec3(0.212671f, 0.715160f, 0.072169f)) * area;
					total_weight += emitter_probs[i];
				}

				std::vector<AliasTable> alias_table = build_alias_table(emitter_probs, total_weight);
				buffer.emitter_alias_table          = m_context->create_buffer("Emitter Alias Table", std::max(alias_table.size(), 1ull) * sizeof(AliasTable), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
				if (!alias_table.empty())
				{
					m_context->buffer_copy_to_device(buffer.emitter_alias_table, alias_table.data(), alias_table.size() * sizeof(AliasTable), true);
				}
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
				buffer.indirect_draw = m_context->create_buffer("Indirect Draw Buffer", indirect_commands.size() * sizeof(VkDrawIndexedIndirectCommand), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
				m_context->buffer_copy_to_device(buffer.indirect_draw, indirect_commands.data(), indirect_commands.size() * sizeof(VkDrawIndexedIndirectCommand), true);
			}

			// Create instance buffer
			buffer.instance = m_context->create_buffer("Instance Buffer", instances.size() * sizeof(Instance), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
			m_context->buffer_copy_to_device(buffer.instance, instances.data(), instances.size() * sizeof(Instance), true);
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

				Buffer instance_buffer = m_context->create_buffer("Instance Stratch Buffer", vk_instances.size() * sizeof(VkAccelerationStructureInstanceKHR), VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
				m_context->buffer_copy_to_device(instance_buffer, vk_instances.data(), vk_instances.size() * sizeof(VkAccelerationStructureInstanceKHR), true);

			    VkAccelerationStructureGeometryKHR as_geometry = {
			        .sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
			        .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
			        .geometry     = {
			                .instances = {
			                    .sType           = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
			                    .arrayOfPointers = VK_FALSE,
			                    .data            = instance_buffer.device_address,
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
}

void Scene::destroy_scene()
{
	m_context->destroy(blas)
	    .destroy(tlas)
	    .destroy(buffer.instance)
	    .destroy(buffer.emitter)
	    .destroy(buffer.material)
	    .destroy(buffer.vertex)
	    .destroy(buffer.index)
	    .destroy(buffer.indirect_draw)
	    .destroy(buffer.global)
	    .destroy(buffer.emitter_alias_table)
	    .destroy(buffer.mesh_alias_table)
	    .destroy(buffer.scene)
	    .destroy(textures)
	    .destroy(texture_views);
}

void Scene::destroy_envmap()
{
}