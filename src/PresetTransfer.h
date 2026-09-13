#pragma once

#include <string>
#include <string_view>

namespace sosr {
class Menu;

namespace presets {
inline constexpr std::string_view kPresetFormat = "SkyrimVanitySystem.Preset";
inline constexpr int kPresetVersion = 1;

[[nodiscard]] bool ExportAll(Menu &a_menu, std::string_view a_path,
                             std::string_view a_name,
                             std::string &a_error);
} // namespace presets
} // namespace sosr
