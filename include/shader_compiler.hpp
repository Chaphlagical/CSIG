#pragma once

#include <volk.h>

#include <string>
#include <unordered_map>
#include <vector>

// Slang -> SPIR-V compiler
class ShaderCompiler
{
  public:
	static std::vector<uint32_t> compile(const std::string &path, VkShaderStageFlagBits stage, const std::string &entry_point = "main", const std::unordered_map<std::string, std::string> &macros = {});

  private:
	ShaderCompiler() = default;

	~ShaderCompiler() = default;

	static ShaderCompiler &get_instance();

	std::vector<uint32_t> _compile(const std::string &path, VkShaderStageFlagBits stage, const std::string &entry_point = "main", const std::unordered_map<std::string, std::string> &macros = {}) const;
};