#include "PresetTransfer.h"

#include "Utf8Path.h"
#include "VariantWorkbench.h"
#include "ui/Menu.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <unordered_set>

namespace {
using Json = nlohmann::json;
using ConditionAction = sosr::presets::ConditionImportAction;
using ConditionPlanEntry = sosr::presets::ConditionImportPlanEntry;
using ImportPlan = sosr::presets::ImportPlan;
using ApplyResult = sosr::presets::ApplyResult;

bool LoadPresetRoot(const std::string_view a_path, Json &a_root,
                    std::string &a_error) {
  if (a_path.empty()) {
    a_error = "Preset import plan has no source path.";
    return false;
  }

  std::ifstream input(sosr::utf8::PathFromUtf8(a_path), std::ios::binary);
  if (!input.is_open()) {
    a_error = "Failed to reopen preset file for applying.";
    return false;
  }

  a_root = Json::parse(input, nullptr, false, true);
  if (a_root.is_discarded() || !a_root.is_object()) {
    a_error = "Failed to parse preset JSON while applying.";
    return false;
  }
  if (a_root.value("format", std::string{}) != sosr::presets::kPresetFormat ||
      a_root.value("version", 0) != sosr::presets::kPresetVersion) {
    a_error = "Preset format or version changed since preview.";
    return false;
  }

  const auto conditionsIt = a_root.find("conditions");
  const auto workbenchIt = a_root.find("workbench");
  if (conditionsIt == a_root.end() || !conditionsIt->is_object() ||
      workbenchIt == a_root.end() || !workbenchIt->is_object() ||
      !conditionsIt->contains("conditions") ||
      !conditionsIt->at("conditions").is_array() ||
      !workbenchIt->contains("rows") || !workbenchIt->at("rows").is_array()) {
    a_error = "Preset payload changed or is incomplete.";
    return false;
  }
  return true;
}

const ConditionPlanEntry *FindPlanEntry(const ImportPlan &a_plan,
                                        const std::string_view a_sourceId) {
  const auto it = std::ranges::find_if(
      a_plan.conditions, [&](const ConditionPlanEntry &a_entry) {
        return a_entry.sourceId == a_sourceId;
      });
  return it != a_plan.conditions.end() ? std::addressof(*it) : nullptr;
}

bool PlansMatch(const ImportPlan &a_preview, const ImportPlan &a_fresh) {
  if (a_preview.conditions.size() != a_fresh.conditions.size() ||
      a_preview.conditionIdMap != a_fresh.conditionIdMap ||
      a_preview.sourceWorkbenchRows != a_fresh.sourceWorkbenchRows ||
      a_preview.resolvedWorkbenchRows != a_fresh.resolvedWorkbenchRows ||
      a_preview.missingWorkbenchRows != a_fresh.missingWorkbenchRows ||
      a_preview.sourceOverrideItems != a_fresh.sourceOverrideItems ||
      a_preview.missingOverrideItems != a_fresh.missingOverrideItems ||
      a_preview.unresolvedConditionReferences !=
          a_fresh.unresolvedConditionReferences ||
      a_preview.missingFormIdentifiers != a_fresh.missingFormIdentifiers ||
      a_preview.unresolvedConditionIds != a_fresh.unresolvedConditionIds) {
    return false;
  }

  for (std::size_t index = 0; index < a_preview.conditions.size(); ++index) {
    const auto &left = a_preview.conditions[index];
    const auto &right = a_fresh.conditions[index];
    if (left.sourceId != right.sourceId || left.targetId != right.targetId ||
        left.sourceName != right.sourceName ||
        left.targetName != right.targetName || left.action != right.action) {
      return false;
    }
  }
  return true;
}

std::string RemapConditionId(const ImportPlan &a_plan,
                             const std::string_view a_sourceId) {
  if (a_sourceId.empty()) {
    return {};
  }
  if (const auto it = a_plan.conditionIdMap.find(std::string(a_sourceId));
      it != a_plan.conditionIdMap.end()) {
    return it->second;
  }
  return std::string(a_sourceId);
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

bool BuildMergedConditionState(sosr::Menu &a_menu, const Json &a_root,
                               const ImportPlan &a_plan, Json &a_merged,
                               std::string &a_error) {
  a_merged = a_menu.SerializeConditionState();
  if (!a_merged.is_object() || !a_merged["conditions"].is_array()) {
    a_error = "Current condition state could not be serialized for merge.";
    return false;
  }

  int nextConditionId = (std::max)(1, a_merged.value("nextConditionId", 1));
  std::unordered_set<std::string> targetIds;
  for (const auto &condition : a_merged["conditions"]) {
    if (condition.is_object()) {
      targetIds.insert(condition.value("id", std::string{}));
    }
  }

  for (const auto &sourceCondition : a_root.at("conditions").at("conditions")) {
    if (!sourceCondition.is_object()) {
      a_error = "Preset contains an invalid condition entry.";
      return false;
    }

    const auto sourceId = sourceCondition.value("id", std::string{});
    const auto *entry = FindPlanEntry(a_plan, sourceId);
    if (entry == nullptr) {
      a_error = std::format("Import plan no longer contains condition '{}'.",
                            sourceId);
      return false;
    }
    if (entry->action == ConditionAction::ReuseExisting) {
      continue;
    }
    if (entry->targetId.empty() || entry->targetName.empty() ||
        targetIds.contains(entry->targetId)) {
      a_error = std::format("Condition merge target '{}' is no longer valid.",
                            entry->targetId);
      return false;
    }

    auto importedCondition = sourceCondition;
    importedCondition["id"] = entry->targetId;
    importedCondition["name"] = entry->targetName;
    if (auto clausesIt = importedCondition.find("clauses");
        clausesIt != importedCondition.end() && clausesIt->is_array()) {
      for (auto &clause : *clausesIt) {
        if (!clause.is_object()) {
          continue;
        }
        const auto customId =
            clause.value("customConditionId", std::string{});
        if (!customId.empty()) {
          clause["customConditionId"] = RemapConditionId(a_plan, customId);
        }
      }
    }

    targetIds.insert(entry->targetId);
    nextConditionId =
        (std::max)(nextConditionId, ParseConditionOrdinal(entry->targetId) + 1);
    a_merged["conditions"].push_back(std::move(importedCondition));
  }

  a_merged["nextConditionId"] = nextConditionId;
  return true;
}

void RemapWorkbenchConditions(Json &a_workbench, const ImportPlan &a_plan) {
  for (auto &row : a_workbench["rows"]) {
    if (!row.is_object()) {
      continue;
    }
    const auto conditionIt = row.find("conditionId");
    if (conditionIt == row.end() || conditionIt->is_null() ||
        !conditionIt->is_string()) {
      continue;
    }
    const auto sourceId = conditionIt->get<std::string>();
    if (!sourceId.empty()) {
      row["conditionId"] = RemapConditionId(a_plan, sourceId);
    }
  }
}

std::string BuildSerializedRowIdentity(const Json &a_row) {
  if (!a_row.is_object()) {
    return {};
  }

  const auto type = a_row.value("type", std::string{"armor"});
  std::string source;
  if (type == "slot") {
    const auto slot = a_row.value("slot", 0);
    if (slot <= 0) {
      return {};
    }
    source = "slot:" + std::to_string(slot);
  } else {
    const auto equipped = a_row.value("equipped", std::string{});
    if (equipped.empty()) {
      return {};
    }
    source = "armor:" + equipped;
  }

  std::string condition = "null";
  if (const auto conditionIt = a_row.find("conditionId");
      conditionIt != a_row.end() && conditionIt->is_string()) {
    condition = conditionIt->get<std::string>();
  }
  return source + "|condition:" + condition;
}

void MergeOverrideArrays(Json &a_target, const Json &a_source) {
  if (!a_target.is_array()) {
    a_target = Json::array();
  }

  std::unordered_set<std::string> seen;
  for (const auto &value : a_target) {
    if (value.is_string()) {
      seen.insert(value.get<std::string>());
    }
  }
  if (!a_source.is_array()) {
    return;
  }
  for (const auto &value : a_source) {
    if (!value.is_string()) {
      continue;
    }
    const auto identifier = value.get<std::string>();
    if (seen.insert(identifier).second) {
      a_target.push_back(identifier);
    }
  }
}

bool BuildMergedWorkbenchState(sosr::Menu &a_menu, Json a_importedWorkbench,
                               const ImportPlan &a_plan, Json &a_merged,
                               std::size_t &a_serializedAddedRows,
                               std::size_t &a_mergedRows,
                               std::string &a_error) {
  a_merged = a_menu.GetWorkbench().SerializeState();
  if (!a_merged.is_object() || !a_merged["rows"].is_array()) {
    a_error = "Current workbench state could not be serialized for merge.";
    return false;
  }

  RemapWorkbenchConditions(a_importedWorkbench, a_plan);

  std::unordered_map<std::string, std::size_t> existingRows;
  for (std::size_t index = 0; index < a_merged["rows"].size(); ++index) {
    const auto identity = BuildSerializedRowIdentity(a_merged["rows"][index]);
    if (!identity.empty()) {
      existingRows.emplace(identity, index);
    }
  }

  for (const auto &incoming : a_importedWorkbench["rows"]) {
    const auto identity = BuildSerializedRowIdentity(incoming);
    if (identity.empty()) {
      continue;
    }

    if (const auto existing = existingRows.find(identity);
        existing != existingRows.end()) {
      auto &target = a_merged["rows"][existing->second];
      target["hideEquipped"] = target.value("hideEquipped", false) ||
                               incoming.value("hideEquipped", false);
      MergeOverrideArrays(target["overrides"],
                          incoming.value("overrides", Json::array()));
      ++a_mergedRows;
      continue;
    }

    existingRows.emplace(identity, a_merged["rows"].size());
    a_merged["rows"].push_back(incoming);
    ++a_serializedAddedRows;
  }
  return true;
}

bool DeserializeWorkbench(const Json &a_state,
                          sosr::workbench::VariantWorkbench &a_workbench,
                          std::string &a_error) {
  std::string deserializeError;
  if (!a_workbench.DeserializeState(a_state, std::nullopt,
                                    &deserializeError)) {
    a_error = deserializeError.empty()
                  ? "Failed to validate merged workbench state."
                  : deserializeError;
    return false;
  }
  return true;
}

bool RestoreSnapshot(sosr::Menu &a_menu, const Json &a_conditions,
                     const Json &a_workbench, std::string &a_error) {
  std::string conditionError;
  if (!a_menu.DeserializeConditionState(a_conditions, &conditionError)) {
    a_error = conditionError.empty() ? "Failed to restore condition snapshot."
                                     : conditionError;
    return false;
  }

  sosr::workbench::VariantWorkbench restoredWorkbench;
  std::string workbenchError;
  if (!restoredWorkbench.DeserializeState(a_workbench, std::nullopt,
                                          &workbenchError)) {
    a_error = workbenchError.empty() ? "Failed to restore workbench snapshot."
                                     : workbenchError;
    return false;
  }

  a_menu.GetWorkbench().ReplaceState(std::move(restoredWorkbench));
  a_menu.GetWorkbench().SyncDynamicArmorVariantsExtended(
      a_menu.GetConditions());
  return true;
}
} // namespace

namespace sosr::presets {
bool ApplyImportPlan(Menu &a_menu, const ImportPlan &a_plan,
                     ApplyResult &a_result, std::string &a_error) {
  try {
    a_result = {};
    a_error.clear();

    if (a_plan.sourcePath.empty()) {
      a_error = "Import plan has no preset source path.";
      return false;
    }
    if (!a_plan.CanApply()) {
      a_error = "Import plan has unresolved condition references.";
      return false;
    }

    ImportPlan freshPlan;
    if (!BuildImportPlan(a_menu, a_plan.sourcePath, freshPlan, a_error)) {
      return false;
    }
    if (!freshPlan.CanApply()) {
      a_error = "Preset can no longer be applied because condition references "
                "became unresolved.";
      return false;
    }
    if (!PlansMatch(a_plan, freshPlan)) {
      a_error = "Preset import preview is stale. Rebuild the import plan before "
                "applying it.";
      return false;
    }

    Json root;
    if (!LoadPresetRoot(freshPlan.sourcePath, root, a_error)) {
      return false;
    }

    Json mergedConditions;
    if (!BuildMergedConditionState(a_menu, root, freshPlan, mergedConditions,
                                   a_error)) {
      return false;
    }

    Json mergedWorkbench;
    std::size_t serializedAddedRows = 0;
    std::size_t mergedRows = 0;
    if (!BuildMergedWorkbenchState(a_menu, root.at("workbench"), freshPlan,
                                   mergedWorkbench, serializedAddedRows,
                                   mergedRows, a_error)) {
      return false;
    }

    workbench::VariantWorkbench validatedWorkbench;
    if (!DeserializeWorkbench(mergedWorkbench, validatedWorkbench, a_error)) {
      return false;
    }

    // Snapshot only after all merge planning and workbench validation succeeds.
    // The commit below has two mutations; if either one fails, restore both
    // serialized save-data components and resync DAVE.
    const auto originalConditions = a_menu.SerializeConditionState();
    const auto originalWorkbench = a_menu.GetWorkbench().SerializeState();
    const auto originalResolvedRows = a_menu.GetWorkbench().GetRowCount();

    std::string conditionError;
    if (!a_menu.DeserializeConditionState(mergedConditions, &conditionError)) {
      a_error = conditionError.empty() ? "Failed to commit merged conditions."
                                       : conditionError;
      return false;
    }

    try {
      a_menu.GetWorkbench().ReplaceState(std::move(validatedWorkbench));
      a_menu.GetWorkbench().SyncDynamicArmorVariantsExtended(
          a_menu.GetConditions());
    } catch (const std::exception &exception) {
      std::string rollbackError;
      if (!RestoreSnapshot(a_menu, originalConditions, originalWorkbench,
                           rollbackError)) {
        a_error = std::format(
            "Preset merge failed ({}), and rollback also failed ({}).",
            exception.what(), rollbackError);
      } else {
        a_error =
            std::format("Preset merge failed and was rolled back: {}",
                        exception.what());
      }
      return false;
    }

    a_result.addedConditions = freshPlan.addedConditions;
    a_result.reusedConditions = freshPlan.reusedConditions;
    a_result.renamedConditions = freshPlan.renamedConditions;
    a_result.mergedWorkbenchRows = mergedRows;
    a_result.finalWorkbenchRows = a_menu.GetWorkbench().GetRowCount();
    a_result.addedWorkbenchRows =
        a_result.finalWorkbenchRows > originalResolvedRows
            ? a_result.finalWorkbenchRows - originalResolvedRows
            : 0;
    a_result.missingWorkbenchRows = freshPlan.missingWorkbenchRows;
    a_result.missingOverrideItems = freshPlan.missingOverrideItems;
    a_result.missingFormIdentifiers = freshPlan.missingFormIdentifiers;

    logger::info(
        "Applied SVS preset merge from {}: conditions add={} reuse={} "
        "renamed={}, workbench added={} merged={} final={}, missing rows={}, "
        "missing overrides={}",
        freshPlan.sourcePath, a_result.addedConditions,
        a_result.reusedConditions, a_result.renamedConditions,
        a_result.addedWorkbenchRows, a_result.mergedWorkbenchRows,
        a_result.finalWorkbenchRows, a_result.missingWorkbenchRows,
        a_result.missingOverrideItems);
    return true;
  } catch (const std::exception &exception) {
    a_error = std::format("Failed to apply preset import plan: {}",
                          exception.what());
    return false;
  }
}
} // namespace sosr::presets
