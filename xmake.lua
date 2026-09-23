-- set minimum xmake version
set_xmakever("3.0.0")

includes("lib/commonlibsse-ng")

local build_version = os.getenv("SVS_BUILD_VERSION") or "1.4.10"
local build_version_string = os.getenv("SVS_BUILD_VERSION_STRING") or build_version
local major, minor, patch = build_version:match("^(%d+)%.(%d+)%.(%d+)$")
if not major then
    error("SVS_BUILD_VERSION must be in major.minor.patch format, got " .. build_version)
end

set_project("SkyrimVanitySystem")
set_version(build_version)
set_license("GPL-3.0")

set_languages("c++23")
set_warnings("allextra")

set_policy("package.requires_lock", true)

add_rules("mode.release", "mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

add_requires("nlohmann_json v3.12.0")
add_requires("rapidcsv v8.92")

target("SkyrimVanitySystem")
    set_version(build_version)

    add_deps("commonlibsse-ng")
    add_packages("nlohmann_json")

    add_rules("commonlibsse-ng.plugin", {
        name = "Skyrim Vanity System",
        author = "PenguinToast",
        description = "SKSE64 plugin using CommonLibSSE-NG and Dear ImGui"
    })

    add_defines("SVS_VERSION_MAJOR=" .. major)
    add_defines("SVS_VERSION_MINOR=" .. minor)
    add_defines("SVS_VERSION_PATCH=" .. patch)
    add_defines('SVS_VERSION_STRING=\"' .. build_version_string .. '\"')

    add_files("src/**.cpp")
    add_files(
        "lib/imgui/imgui.cpp",
        "lib/imgui/imgui_draw.cpp",
        "lib/imgui/imgui_tables.cpp",
        "lib/imgui/imgui_widgets.cpp",
        "lib/imgui/backends/imgui_impl_dx11.cpp",
        "lib/imgui/backends/imgui_impl_win32.cpp"
    )
    add_headerfiles("src/**.h")
    add_includedirs("src", "lib/imgui", "lib/imgui/backends")
    add_syslinks("d3d11", "dxgi")
    set_pcxxheader("src/pch.h")

target("VanityUBEHeelAdapter")
    set_version("0.17.0")
    -- Must be defined before the plugin PCH includes Windows headers.
    -- Standalone algorithm tests previously missed the DLL's min/max collision.
    add_defines("NOMINMAX")

    add_deps("commonlibsse-ng")
    add_packages("nlohmann_json")

    add_rules("commonlibsse-ng.plugin", {
        name = "Vanity UBE Heel Adapter",
        author = "A90072180A",
        description = "POC consumer for Skyrim Vanity System visual-state API"
    })

    add_files("plugins/VanityUBEHeelAdapter/src/**.cpp")
    add_headerfiles("plugins/VanityUBEHeelAdapter/src/**.h")
    add_includedirs("src", "plugins/VanityUBEHeelAdapter/src")
    set_pcxxheader("plugins/VanityUBEHeelAdapter/src/pch.h")

-- Exercises the actual asynchronous-cache implementation with synthetic files.
-- No game runtime or CommonLib is required by this standalone executable.
target("HeightProfileCacheTests")
    set_kind("binary")
    add_packages("nlohmann_json")
    add_defines("NOMINMAX")
    add_files("plugins/VanityUBEHeelAdapter/tests/HeightProfileCacheTests.cpp")
    add_includedirs("plugins/VanityUBEHeelAdapter/src")

-- Configuration and report workers built without any Skyrim types.
target("RuntimeConfigurationTests")
    set_kind("binary")
    set_languages("c++20")
    add_defines("NOMINMAX")
    add_packages("nlohmann_json")
    add_files("plugins/VanityUBEHeelAdapter/tests/RuntimeConfigurationTests.cpp")
target_end()
target("ConfigurationTests")
    set_kind("binary")
    set_languages("c++20")
    add_defines("NOMINMAX")
    add_packages("nlohmann_json")
    add_files("plugins/VanityUBEHeelAdapter/tests/ConfigurationTests.cpp")
target_end()
