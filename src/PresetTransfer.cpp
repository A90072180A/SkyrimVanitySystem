#include "PresetTransfer.h"

#include "Plugin.h"
#include "Utf8Path.h"
#include "ui/Menu.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>

namespace {
std::string ResolvePresetName(const std::filesystem::path &a_path,
                              const std::string_view a_name) {
  if (!a_name.empty()) {
    return std::string(a_name);
  }

  const auto stem = a_path.stem().string();
  return stem.empty() ? std::string{"SVS Preset"} : stem;
}
} // namespace

namespace sosr::presets {
bool ExportAll(Menu &a_menu, const std::string_view a_path,
               const std::string_view a_name, std::string &a_error) {
  try {
    if (a_path.empty()) {
      a_error = "Choose a preset file path.";
      return false;
    }

    const auto path = utf8::PathFromUtf8(a_path);
    if (const auto parentPath = path.parent_path(); !parentPath.empty()) {
      std::filesystem::create_directories(parentPath);
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
      a_error = "Failed to open preset file for writing.";
      return false;
    }

    const nlohmann::json root{
        {"format", kPresetFormat},
        {"version", kPresetVersion},
        {"metadata",
         {{"name", ResolvePresetName(path, a_name)},
          {"createdWith", std::string(Plugin::VERSION_STRING)},
          {"scope", "all"}}},
        {"conditions", a_menu.SerializeConditionState()},
        {"workbench", a_menu.GetWorkbench().SerializeState()}};

    output << root.dump(2) << '\n';
    if (!output.good()) {
      a_error = "Failed to write preset export.";
      return false;
    }

    logger::info("Exported Skyrim Vanity System preset to {}",
                 utf8::PathToUtf8String(path));
    return true;
  } catch (const std::exception &exception) {
    a_error = std::format("Failed to export preset: {}", exception.what());
    return false;
  }
}
} // namespace sosr::presets
