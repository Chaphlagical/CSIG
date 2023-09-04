#ifndef SCENE_GLSL
#define SCENE_GLSL

#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_ray_query : enable
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_scalar_block_layout : enable
#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_buffer_reference : enable
#extension GL_EXT_buffer_reference2 : enable

#include "common.glsl"

#define PREFILTER_MIP_LEVELS 5

layout(set = 0, binding = 0, scalar) uniform GlobalUBO
{
    GlobalData ubo;
};
layout(set = 0, binding = 1) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 2, scalar) uniform SceneBuffer
{
	SceneData scene_data;
};
layout(set = 0, binding = 3) uniform sampler2D textures[];
layout(set = 0, binding = 4) uniform samplerCube skybox;
layout(set = 0, binding = 5) uniform sampler2D irradiance_sh;
layout(set = 0, binding = 6) uniform samplerCube prefilter_map;
layout(set = 0, binding = 7) uniform sampler2D ggx_lut;
layout(set = 0, binding = 8) uniform sampler2D sobol_sequence;
layout(set = 0, binding = 9) uniform sampler2D scrambling_ranking_tile[];

layout(buffer_reference, scalar) readonly buffer InstanceBuffer
{
    Instance instances[];
};

layout(buffer_reference, scalar) readonly buffer VertexBuffer
{
    Vertex vertices[];
};

layout(buffer_reference, scalar) readonly buffer IndexBuffer
{
    uint indices[];
};

layout(buffer_reference, scalar) readonly buffer MaterialBuffer
{
	Material materials[];
};

layout(buffer_reference, scalar) readonly buffer EmitterBuffer
{
	Emitter emitters[];
};

layout(buffer_reference, scalar) readonly buffer EmitterAliasTableBuffer
{
	AliasTable emitter_alias_tables[];
};

layout(buffer_reference, scalar) readonly buffer MeshAliasTableBuffer
{
	AliasTable mesh_alias_tables[];
};

InstanceBuffer instance_buffer = InstanceBuffer(scene_data.instance_buffer_addr);
VertexBuffer vertex_buffer = VertexBuffer(scene_data.vertex_buffer_addr);
IndexBuffer index_buffer = IndexBuffer(scene_data.index_buffer_addr);
MaterialBuffer material_buffer = MaterialBuffer(scene_data.material_buffer_addr);
EmitterBuffer emitter_buffer = EmitterBuffer(scene_data.emitter_buffer_addr);
EmitterAliasTableBuffer emitter_alias_table_buffer = EmitterAliasTableBuffer(scene_data.emitter_alias_table_buffer_addr);
MeshAliasTableBuffer mesh_alias_table_buffer = MeshAliasTableBuffer(scene_data.mesh_alias_table_buffer_addr);

Instance get_instance(uint id)
{
    return instance_buffer.instances[id];
}

Vertex get_vertex(uint id)
{
    return vertex_buffer.vertices[id];
}

uint get_index(uint id)
{
    return index_buffer.indices[id];
}

Material get_material(uint id)
{
    return material_buffer.materials[id];
}

Emitter get_emitter(uint id)
{
    return emitter_buffer.emitters[id];
}

void sample_emitter_alias_table(vec2 rnd, out int index, out float pdf) 
{
	int selected_column = min(int(float(scene_data.emitter_count) * rnd.x), int(scene_data.emitter_count - 1));
	AliasTable col = emitter_alias_table_buffer.emitter_alias_tables[selected_column];
	if (col.prob > rnd.y) 
	{
		index = selected_column;
		pdf = col.ori_prob;
	} 
	else 
	{
		index = col.alias;
		pdf = col.ori_prob;
	}
}

float emitter_pdf(int index)
{
    AliasTable col = emitter_alias_table_buffer.emitter_alias_tables[index];
    return col.ori_prob;
}

void sample_mesh_alias_table(vec2 rnd, Instance instance, out int index, out float pdf) 
{
	int selected_column = int(instance.indices_offset / 3) + min(int(float(instance.indices_count / 3) * rnd.x), int(instance.indices_count / 3 - 1));
	AliasTable col = mesh_alias_table_buffer.mesh_alias_tables[selected_column];
	if (col.prob > rnd.y) 
	{
		index = selected_column - int(instance.indices_offset / 3);
		pdf = col.ori_prob;
	} 
	else 
	{
		index = col.alias;
		pdf = col.ori_prob;
	}
}

float sample_blue_noise(ivec2 coord, int sample_index, int sample_dimension, sampler2D sobol_sequence_tex, sampler2D scrambling_ranking_tex)
{
	coord.x = coord.x % 128;
	coord.y = coord.y % 128;

	sample_index = sample_index % 256;
	sample_dimension = sample_dimension % 4;

	int ranked_sample_index = sample_index ^ int(clamp(texelFetch(scrambling_ranking_tex, ivec2(coord.x, coord.y), 0).b * 256.0, 0.0, 255.0));
	int value = int(clamp(texelFetch(sobol_sequence_tex, ivec2(ranked_sample_index, 0), 0)[sample_dimension] * 256.0, 0.0, 255.0));

	value = value ^ int(clamp(texelFetch(scrambling_ranking_tex, ivec2(coord.x, coord.y), 0)[sample_dimension % 2] * 256.0, 0.0, 255.0));

	float v = (0.5 + value) / 256.0;
	return v;
}

vec2 next_sample(ivec2 coord, uint num_frames, uint rank)
{
    return vec2(sample_blue_noise(coord, int(num_frames), 0, sobol_sequence, scrambling_ranking_tile[rank]),
                sample_blue_noise(coord, int(num_frames), 1, sobol_sequence, scrambling_ranking_tile[rank]));
}

#endif