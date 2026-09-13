#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sosr {
class Menu;

namespace presets {
inline constexpr std::string_view kPresetFormat = "SkyrimVanitySystem.Preset";
inline constexpr int kPresetVersion = 1;

enum class ConditionImportAction : std::uint8_t {
  ReuseExisting,
  Add,
  AddRenamed
};

struct ConditionImportPlanEntry {
  std::string sourceId;
  std::string targetId;
  std::string sourceName;
  std::string targetName;
  ConditionImportAction action{ConditionImportAction::Add};
};

struct ImportPlan {
  std::string sourcePath;
  std::string presetName;
  std::string createdWith;
  std::vector<ConditionImportPlanEntry> conditions;
  std::unordered_map<std::string, std::string> conditionIdMap;

  std::size_t addedConditions{0};
  std::size_t reusedConditions{0};
  std::size_t renamedConditions{0};

  std::size_t sourceWorkbenchRows{0};
  std::size_t resolvedWorkbenchRows{0};
  std::size_t missingWorkbenchRows{0};
  std::size_t sourceOverrideItems{0};
  std::size_t missingOverrideItems{0};
  std::size_t unresolvedConditionReferences{0};

  std::vector<std::string> missingFormIdentifiers;
  std::vector<std::string> unresolvedConditionIds;

  [[nodiscard]] bool CanApply() const {
    return unresolvedConditionReferences == 0;
  }
};

[[nodiscard]] bool ExportAll(Menu &a_menu, std::string_view a_path,
                             std::string_view a_name,
                             std::string &a_error);

[[nodiscard]] bool BuildImportPlan(Menu &a_menu, std::string_view a_path,
                                   ImportPlan &a_plan,
                                   std::string &a_error);
} // namespace presets
} // namespace sosr
