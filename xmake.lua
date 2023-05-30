set_project("raytracer")
set_version("0.0.1")

set_xmakever("2.7.4")

set_warnings("all")
set_languages("c++20")

add_rules("mode.debug", "mode.release", "mode.releasedbg")

set_runtimes("MD")
add_defines("NOMINMAX")
set_warnings("all")

add_requires("glfw", "vulkan-headers", "vulkan-memory-allocator", "spdlog", "stb", "glm", "cgltf", "nativefiledialog")
add_requires("volk", {configs = {header_only = true}})
add_requires("imgui", {configs = {glfw = true}})
add_requires("glslang", {configs = {binaryonly = true}})

package("cgltf")
    on_load(function (package)
        package:set("installdir", path.join(os.scriptdir(), "external/cgltf"))
    end)

    on_fetch(function (package)
        local result = {}
        result.includedirs = package:installdir()
        return result
    end)
package_end()

target("raytracer")
    set_kind("binary")
    add_rules("utils.glsl2spv", {bin2c = true, targetenv="vulkan1.3"})

    set_rundir("$(projectdir)")

    if is_mode("debug") then
        add_defines("DEBUG")
    end

    add_defines("VK_NO_PROTOTYPES")

    if is_plat("windows") then
        add_defines("VK_USE_PLATFORM_WIN32_KHR")
    end

    add_files("src/raytracer/**.cpp")

    add_files(
        "src/shaders/**.comp",
        "src/shaders/**.vert",
        "src/shaders/**.frag")

    add_headerfiles(
        "include/**.hpp",
        "src/shaders/**.glsl",
        "src/shaders/**.hpp",
        "src/shaders/**.comp",
        "src/shaders/**.vert",
        "src/shaders/**.frag")

    add_includedirs("include", {public  = true})
    add_includedirs("src/shaders/")

    add_packages("glfw", "vulkan-headers", "vulkan-memory-allocator", "spdlog", "stb", "glm", "volk", "imgui", "glslang", "cgltf", "nativefiledialog")
target_end()
