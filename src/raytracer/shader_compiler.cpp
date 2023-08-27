#include "shader_compiler.hpp"

#include <spdlog/spdlog.h>

#include <slang-com-ptr.h>
#include <slang.h>

#include <filesystem>
#include <fstream>
#include <sstream>

inline void diagnoseIfNeeded(slang::IBlob *diagnosticsBlob)
{
	if (diagnosticsBlob != nullptr)
	{
		spdlog::error("{}", (const char *) diagnosticsBlob->getBufferPointer());
	}
}

SlangStage get_slang_stage(VkShaderStageFlagBits stage)
{
	switch (stage)
	{
		case VK_SHADER_STAGE_VERTEX_BIT:
			return SLANG_STAGE_VERTEX;
		case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
			return SLANG_STAGE_HULL;
		case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
			return SLANG_STAGE_DOMAIN;
		case VK_SHADER_STAGE_GEOMETRY_BIT:
			return SLANG_STAGE_GEOMETRY;
		case VK_SHADER_STAGE_FRAGMENT_BIT:
			return SLANG_STAGE_PIXEL;
		case VK_SHADER_STAGE_COMPUTE_BIT:
			return SLANG_STAGE_COMPUTE;
		case VK_SHADER_STAGE_RAYGEN_BIT_KHR:
			return SLANG_STAGE_RAY_GENERATION;
		case VK_SHADER_STAGE_ANY_HIT_BIT_KHR:
			return SLANG_STAGE_ANY_HIT;
		case VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR:
			return SLANG_STAGE_CLOSEST_HIT;
		case VK_SHADER_STAGE_MISS_BIT_KHR:
			return SLANG_STAGE_MISS;
		case VK_SHADER_STAGE_INTERSECTION_BIT_KHR:
			return SLANG_STAGE_INTERSECTION;
		case VK_SHADER_STAGE_CALLABLE_BIT_KHR:
			return SLANG_STAGE_CALLABLE;
		case VK_SHADER_STAGE_TASK_BIT_EXT:
			return SLANG_STAGE_AMPLIFICATION;
		case VK_SHADER_STAGE_MESH_BIT_EXT:
			return SLANG_STAGE_MESH;
		default:
			break;
	}
	return SLANG_STAGE_NONE;
}

std::vector<uint32_t> ShaderCompiler::compile(const std::string &path, VkShaderStageFlagBits stage, const std::string &entry_point, const std::unordered_map<std::string, std::string> &macros)
{
	return get_instance()._compile(path, stage, entry_point, macros);
}

ShaderCompiler &ShaderCompiler::get_instance()
{
	static ShaderCompiler compiler;
	return compiler;
}

std::vector<uint32_t> ShaderCompiler::_compile(const std::string &path, VkShaderStageFlagBits stage, const std::string &entry_point, const std::unordered_map<std::string, std::string> &macros) const
{
	SlangSession        *session = spCreateSession();
	SlangCompileRequest *request = spCreateCompileRequest(session);
	spSetCodeGenTarget(request, SLANG_SPIRV);
	int32_t translationUnitIndex = spAddTranslationUnit(request, SLANG_SOURCE_LANGUAGE_SLANG, "");
	spAddTranslationUnitSourceFile(request, translationUnitIndex, (SHADER_DIR + path).c_str());
	spAddTargetCapability(request, 0, session->findCapability("spirv_1_4"));
	spAddPreprocessorDefine(request, "HLSL", "");
	spSetMatrixLayoutMode(request, SLANG_MATRIX_LAYOUT_COLUMN_MAJOR);
	for (auto &[key, value] : macros)
	{
		spAddPreprocessorDefine(request, key.c_str(), value.c_str());
	}
	int32_t entryPointIndex = spAddEntryPoint(request, translationUnitIndex, entry_point.c_str(), get_slang_stage(stage));
	int32_t anyErrors       = spCompile(request);
	if (anyErrors != 0)
	{
		std::string error_info = spGetDiagnosticOutput(request);
		spdlog::error(error_info);
	}
	size_t      dataSize = 0;
	void const *data     = spGetEntryPointCode(request, entryPointIndex, &dataSize);

	std::vector<uint32_t> spirv(dataSize);
	std::memcpy(spirv.data(), data, dataSize);

	return spirv;
}