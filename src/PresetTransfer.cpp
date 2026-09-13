#include "PresetTransfer.h"

#include "ArmorUtils.h"
#include "Plugin.h"
#include "StringUtils.h"
#include "Utf8Path.h"
#include "VariantWorkbench.h"
#include "conditions/Validation.h"
#include "ui/Menu.h"
#include "ui/conditions/DraftValidation.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>
#include <unordered_set>

namespace {
using Json = nlohmann::json;

std::string ResolvePresetName(const std::filesystem::path &a_path,
                              const std::string_view a_name) {
  if (!a_name.empty()) {
    return std::string(a_name);
  }

  const auto stem = a_path.stem().string();
  return stem.empty() ? std::string{"SVS Preset"} : stem;
}

std::string LowerAscii(const std::string_view a_text) {
  std::string lowered(a_text);
  std::ranges::transform(lowered, lowered.begin(), [](const char a_ch) {
    return static_cast<char>(
        std::tolower(static_cast<unsigned char>(a_ch)));
  });
  return lowered;
}

bool LoadPresetRoot(const std::string_view a_path, Json &a_root,
                    std::filesystem::path &a_resolvedPath,
                    std::string &a_error) {
  if (a_path.empty()) {
    a_error = "Choose a preset file path.";
    return false;
  }

  a_resolvedPath = sosr::utf8::PathFromUtf8(a_path);
  std::ifstream input(a_resolvedPath, std::ios::binary);
  if (!input.is_open()) {
    a_error = "Failed to open preset file for reading.";
    return false;
  }

  a_root = Json::parse(input, nullptr, false, true);
  if (a_root.is_discarded() || !a_root.is_object()) {
    a_error = "Failed to parse preset JSON.";
    return false;
  }

  if (a_root.value("format", std::string{}) != sosr::presets::kPresetFormat) {
    a_error = "File is not a Skyrim Vanity System preset.";
    return false;
  }
  if (a_root.value("version", 0) != sosr::presets::kPresetVersion) {
    a_error = "Preset version is not supported.";
    return false;
  }

  const auto conditionsIt = a_root.find("conditions");
  const auto workbenchIt = a_root.find("workbench");
  if (conditionsIt == a_root.end() || !conditionsIt->is_object() ||
      workbenchIt == a_root.end() || !workbenchIt->is_object()) {
    a_error = "Preset is missing condition or workbench data.";
    return false;
  }
  const auto conditionListIt = conditionsIt->find("conditions");
  const auto rowsIt = workbenchIt->find("rows");
  if (conditionListIt == conditionsIt->end() || !conditionListIt->is_array() ||
      rowsIt == workbenchIt->end() || !rowsIt->is_array()) {
    a_error = "Preset condition or workbench data has an invalid shape.";
    return false;
  }

  return true;
}

std::unordered_map<std::string, std::string>
BuildSourceIdToNameMap(const Json &a_conditionState, std::string &a_error) {
  std::unordered_map<std::string, std::string> result;
  std::unordered_set<std::string> names;

  for (const auto &condition : a_conditionState.at("conditions")) {
    if (!condition.is_object()) {
      a_error = "Preset contains a non-object condition entry.";
      return {};
    }
    const auto id = condition.value("id", std::string{});
    const auto name = condition.value("name", std::string{});
    const auto clausesIt = condition.find("clauses");
    if (id.empty() || name.empty() || clausesIt == condition.end() ||
        !clausesIt->is_array()) {
      a_error = "Preset contains an incomplete condition entry.";
      return {};
    }
    if (!result.emplace(id, name).second) {
      a_error = std::format("Preset contains duplicate condition id '{}'.", id);
      return {};
    }
    if (!names.insert(LowerAscii(name)).second) {
      a_error = std::format("Preset contains duplicate condition name '{}'.",
                            name);
      return {};
    }
  }

  return result;
}

std::unordered_map<std::string, std::string>
BuildCurrentIdToNameMap(const sosr::Menu &a_menu) {
  std::unordered_map<std::string, std::string> result;
  for (const auto &condition : a_menu.GetConditions()) {
    if (!condition.id.empty()) {
      result.insert_or_assign(condition.id, condition.name);
    }
  }
  return result;
}

Json BuildSemanticConditionView(
    const Json &a_condition,
    const std::unordered_map<std::string, std::string> &a_idToName) {
  Json result{{"enabled", a_condition.value("enabled", true)},
              {"clauses", Json::array()}};

  const auto clausesIt = a_condition.find("clauses");
  if (clausesIt == a_condition.end() || !clausesIt->is_array()) {
    return result;
  }

  for (const auto &clause : *clausesIt) {
    if (!clause.is_object()) {
      continue;
    }

    const auto customId = clause.value("customConditionId", std::string{});
    std::string customName;
    if (!customId.empty()) {
      if (const auto mapped = a_idToName.find(customId);
          mapped != a_idToName.end()) {
        customName = LowerAscii(mapped->second);
      } else {
        customName = LowerAscii(customId);
      }
    }

    result["clauses"].push_back(
        {{"function", clause.value("function", std::string{})},
         {"customCondition", customName},
         {"arg1", clause.value("arg1", std::string{})},
         {"arg2", clause.value("arg2", std::string{})},
         {"comparator", clause.value("comparator", std::string{"=="})},
         {"value", clause.value("value", std::string{"1"})},
         {"join", clause.value("join", std::string{"AND"})}});
  }

  return result;
}

const Json *FindSerializedConditionById(const Json &a_conditionState,
                                        const std::string_view a_id) {
  const auto conditionsIt = a_conditionState.find("conditions");
  if (conditionsIt == a_conditionState.end() || !conditionsIt->is_array()) {
    return nullptr;
  }
  for (const auto &condition : *conditionsIt) {
    if (condition.is_object() &&
        condition.value("id", std::string{}) == a_id) {
      return std::addressof(condition);
    }
  }
  return nullptr;
}

int ParseConditionOrdinal(const std::string_view a_id) {
  constexpr std::string_view prefix = "condition-";
  if (!a_id.starts_with(prefix)) {
    return 0;
  }
  const auto suffix = a_id.substr(prefix.size());
  int value = 0;
  const auto *begin = suffix.data();
  const auto *end = begin + suffix.size();
  const auto [ptr, error] = std::from_chars(begin, end, value);
  return error == std::errc{} && ptr == end && value > 0 ? value : 0;
}

std::string AllocateConditionId(int &a_nextOrdinal,
                                std::unordered_set<std::string> &a_usedIds) {
  for (;;) {
    a_nextOrdinal = (std::max)(a_nextOrdinal, 1);
    auto candidate = "condition-" + std::to_string(a_nextOrdinal++);
    if (a_usedIds.insert(candidate).second) {
      return candidate;
    }
  }
}

void AddUniqueString(std::vector<std::string> &a_values,
                     const std::string &a_value) {
  if (a_value.empty() ||
      std::ranges::find(a_values, a_value) != a_values.end()) {
    return;
  }
  a_values.push_back(a_value);
}

bool BuildConditionPlan(
    sosr::Menu &a_menu, const Json &a_sourceConditionState,
    sosr::presets::ImportPlan &a_plan,
    const std::unordered_map<std::string, std::string> &a_sourceIdToName,
    std::string &a_error) {
  const auto currentState = a_menu.SerializeConditionState();
  const auto currentIdToName = BuildCurrentIdToNameMap(a_menu);

  std::unordered_set<std::string> usedIds;
  int nextOrdinal = (std::max)(1, currentState.value("nextConditionId", 1));
  for (const auto &condition : a_menu.GetConditions()) {
    if (!condition.id.empty()) {
      usedIds.insert(condition.id);
      nextOrdinal =
          (std::max)(nextOrdinal, ParseConditionOrdinal(condition.id) + 1);
    }
  }

  std::unordered_set<std::string> plannedNames;
  const auto plannedNameConflict = [&](const std::string_view a_candidate) {
    return plannedNames.contains(LowerAscii(a_candidate));
  };

  for (const auto &sourceCondition : a_sourceConditionState.at("conditions")) {
    const auto sourceId = sourceCondition.value("id", std::string{});
    const auto sourceName = sourceCondition.value("name", std::string{});

    sosr::presets::ConditionImportPlanEntry entry;
    entry.sourceId = sourceId;
    entry.sourceName = sourceName;

    const auto *existingDefinition = sosr::conditions::FindDefinitionByName(
        a_menu.GetConditions(), sourceName);
    const Json *existingSerialized = nullptr;
    if (existingDefinition != nullptr && existingDefinition->IsCatalog()) {
      existingSerialized =
          FindSerializedConditionById(currentState, existingDefinition->id);
    }

    const bool equivalent =
        existingSerialized != nullptr &&
        BuildSemanticConditionView(sourceCondition, a_sourceIdToName) ==
            BuildSemanticConditionView(*existingSerialized, currentIdToName);

    if (equivalent) {
      entry.targetId = existingDefinition->id;
      entry.targetName = existingDefinition->name;
      entry.action = sosr::presets::ConditionImportAction::ReuseExisting;
      ++a_plan.reusedConditions;
    } else {
      entry.targetId = AllocateConditionId(nextOrdinal, usedIds);
      entry.targetName = sosr::ui::condition_editor::BuildUniqueConditionName(
          sourceName, a_menu.GetConditions(), plannedNameConflict);
      plannedNames.insert(LowerAscii(entry.targetName));
      if (sosr::strings::EqualsInsensitive(entry.targetName, sourceName)) {
        entry.action = sosr::presets::ConditionImportAction::Add;
        ++a_plan.addedConditions;
      } else {
        entry.action = sosr::presets::ConditionImportAction::AddRenamed;
        ++a_plan.addedConditions;
        ++a_plan.renamedConditions;
      }
    }

    if (!a_plan.conditionIdMap.emplace(sourceId, entry.targetId).second) {
      a_error = std::format("Could not map duplicate condition id '{}'.",
                            sourceId);
      return false;
    }
    a_plan.conditions.push_back(std::move(entry));
  }

  return true;
}

void RemapConditionReferences(
    Json &a_conditionState, Json &a_workbenchState,
    const std::unordered_map<std::string, std::string> &a_idMap,
    const std::vector<sosr::conditions::Definition> &a_existingConditions,
    sosr::presets::ImportPlan &a_plan) {
  const auto resolveReference = [&](std::string &a_id) {
    if (a_id.empty()) {
      return;
    }
    if (const auto mapped = a_idMap.find(a_id); mapped != a_idMap.end()) {
      a_id = mapped->second;
      return;
    }
    if (sosr::conditions::FindDefinitionById(a_existingConditions, a_id) !=
        nullptr) {
      return;
    }

    ++a_plan.unresolvedConditionReferences;
    AddUniqueString(a_plan.unresolvedConditionIds, a_id);
  };

  for (auto &condition : a_conditionState["conditions"]) {
    const auto sourceId = condition.value("id", std::string{});
    if (const auto mapped = a_idMap.find(sourceId); mapped != a_idMap.end()) {
      condition["id"] = mapped->second;
    }
    if (auto clausesIt = condition.find("clauses");
        clausesIt != condition.end() && clausesIt->is_array()) {
      for (auto &clause : *clausesIt) {
        if (!clause.is_object()) {
          continue;
        }
        auto customId = clause.value("customConditionId", std::string{});
        if (!customId.empty()) {
          resolveReference(customId);
          clause["customConditionId"] = std::move(customId);
        }
      }
    }
  }

  for (auto &row : a_workbenchState["rows"]) {
    if (!row.is_object()) {
      continue;
    }
    const auto conditionIt = row.find("conditionId");
    if (conditionIt == row.end() || conditionIt->is_null()) {
      continue;
    }
    if (!conditionIt->is_string()) {
      ++a_plan.unresolvedConditionReferences;
      AddUniqueString(a_plan.unresolvedConditionIds, "<invalid>");
      continue;
    }
    auto conditionId = conditionIt->get<std::string>();
    resolveReference(conditionId);
    row["conditionId"] = std::move(conditionId);
  }
}

void InspectMissingForms(const Json &a_workbenchState,
                         sosr::presets::ImportPlan &a_plan) {
  for (const auto &row : a_workbenchState.at("rows")) {
    if (!row.is_object()) {
      continue;
    }

    if (row.value("type", std::string{"armor"}) != "slot") {
      const auto equipped = row.value("equipped", std::string{});
      if (equipped.empty() ||
          sosr::armor::LookupByIdentifier<RE::TESObjectARMO>(equipped) ==
              nullptr) {
        AddUniqueString(a_plan.missingFormIdentifiers, equipped);
      }
    }

    const auto overridesIt = row.find("overrides");
    if (overridesIt == row.end() || !overridesIt->is_array()) {
      continue;
    }
    a_plan.sourceOverrideItems += overridesIt->size();
    for (const auto &overrideValue : *overridesIt) {
      if (!overrideValue.is_string()) {
        ++a_plan.missingOverrideItems;
        continue;
      }
      const auto identifier = overrideValue.get<std::string>();
      if (sosr::armor::LookupByIdentifier<RE::TESObjectARMO>(identifier) ==
          nullptr) {
        ++a_plan.missingOverrideItems;
        AddUniqueString(a_plan.missingFormIdentifiers, identifier);
      }
    }
  }
}

bool InspectResolvedWorkbench(Json a_workbenchState,
                              sosr::presets::ImportPlan &a_plan,
                              std::string &a_error) {
  a_plan.sourceWorkbenchRows = a_workbenchState.at("rows").size();

  sosr::workbench::VariantWorkbench resolved;
  std::string deserializeError;
  if (!resolved.DeserializeState(a_workbenchState, std::nullopt,
                                 &deserializeError)) {
    a_error = deserializeError.empty()
                  ? "Failed to validate preset workbench data."
                  : deserializeError;
    return false;
  }

  a_plan.resolvedWorkbenchRows = resolved.GetRowCount();
  a_plan.missingWorkbenchRows =
      a_plan.sourceWorkbenchRows > a_plan.resolvedWorkbenchRows
          ? a_plan.sourceWorkbenchRows - a_plan.resolvedWorkbenchRows
          : 0;
  return true;
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

bool BuildImportPlan(Menu &a_menu, const std::string_view a_path,
                     ImportPlan &a_plan, std::string &a_error) {
  try {
    a_plan = {};

    Json root;
    std::filesystem::path path;
    if (!LoadPresetRoot(a_path, root, path, a_error)) {
      return false;
    }

    a_plan.sourcePath = utf8::PathToUtf8String(path);
    if (const auto metadataIt = root.find("metadata");
        metadataIt != root.end() && metadataIt->is_object()) {
      a_plan.presetName = metadataIt->value("name", std::string{});
      a_plan.createdWith = metadataIt->value("createdWith", std::string{});
    }
    if (a_plan.presetName.empty()) {
      a_plan.presetName = ResolvePresetName(path, {});
    }

    auto conditionState = root.at("conditions");
    auto workbenchState = root.at("workbench");
    const auto sourceIdToName = BuildSourceIdToNameMap(conditionState, a_error);
    if (!a_error.empty()) {
      return false;
    }

    if (!BuildConditionPlan(a_menu, conditionState, a_plan, sourceIdToName,
                            a_error)) {
      return false;
    }

    RemapConditionReferences(conditionState, workbenchState,
                             a_plan.conditionIdMap, a_menu.GetConditions(),
                             a_plan);
    InspectMissingForms(workbenchState, a_plan);
    if (!InspectResolvedWorkbench(workbenchState, a_plan, a_error)) {
      return false;
    }

    logger::info(
        "Built SVS preset import plan for {}: conditions add={} reuse={} "
        "renamed={}, rows={}/{}, missing overrides={}, unresolved condition "
        "refs={}",
        a_plan.sourcePath, a_plan.addedConditions, a_plan.reusedConditions,
        a_plan.renamedConditions, a_plan.resolvedWorkbenchRows,
        a_plan.sourceWorkbenchRows, a_plan.missingOverrideItems,
        a_plan.unresolvedConditionReferences);
    return true;
  } catch (const std::exception &exception) {
    a_error = std::format("Failed to build preset import plan: {}",
                          exception.what());
    return false;
  }
}
} // namespace sosr::presets
