#include "VariantWorkbench.h"

#include "ArmorUtils.h"
#include "ConditionMaterializer.h"
#include "ConditionRefreshTargets.h"
#include "conditions/Status.h"
#include "integrations/DynamicArmorVariantsExtendedClient.h"
#include "integrations/VisualStateService.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <unordered_set>

namespace {
auto GetDynamicArmorVariantsExtendedClient()
    -> DynamicArmorVariantsExtendedAPI::
        IDynamicArmorVariantsExtendedInterface001 * {
  auto *dav = sosr::integrations::DynamicArmorVariantsExtendedClient::Get();
  if (dav) {
    return dav;
  }

  sosr::integrations::DynamicArmorVariantsExtendedClient::Refresh();
  return sosr::integrations::DynamicArmorVariantsExtendedClient::Get();
}

std::string BuildPreviewVariantName(const std::string &a_rowKey) {
  return "SOSR|PreviewSelected|" + a_rowKey;
}

auto BuildDavVariantPriority(const std::size_t a_rowIndex,
                             const std::size_t a_rowCount) -> std::int32_t {
  if (a_rowCount == 0 || a_rowIndex >= a_rowCount) {
    return 0;
  }

  return static_cast<std::int32_t>(a_rowCount - a_rowIndex);
}

struct DavReplacementData {
  std::vector<std::string> replacements;
  std::string displayName;
};

struct DavVariantDescriptor {
  std::string name;
  std::string json;
  sosr::integrations::VisualVariantDefinition visualDefinition;
};

struct DavVariantPayload {
  std::string variantJson;
  std::string conditionSignature;
  std::shared_ptr<RE::TESCondition> condition;
  sosr::conditions::RefreshTargets refreshTargets;
  sosr::integrations::VisualVariantDefinition visualDefinition;
};

auto BuildVisualDefinition(
    const sosr::workbench::VariantWorkbenchRow &a_row,
    const std::vector<const RE::TESObjectARMO *> &a_overrideArmors,
    const std::string &a_variantId, const std::int32_t a_priority,
    const bool a_preview) -> sosr::integrations::VisualVariantDefinition {
  sosr::integrations::VisualVariantDefinition definition;
  definition.variantId = a_variantId;
  definition.sourceArmorFormID = a_row.IsSlotRow() ? 0 : a_row.equipped.formID;
  definition.triggerSlotMask = a_row.GetSelectionConflictSlotMask();
  definition.priority = a_priority;
  definition.slotTriggered = a_row.IsSlotRow();
  definition.hideSource = a_row.hideEquipped;
  definition.preview = a_preview;

  if (!definition.hideSource) {
    for (const auto *overrideArmor : a_overrideArmors) {
      if (!overrideArmor) {
        continue;
      }
      for (const auto *overrideAddon : overrideArmor->armorAddons) {
        if (!overrideAddon) {
          continue;
        }
        definition.replacements.push_back(
            {.armorFormID = overrideArmor->GetFormID(),
             .armorAddonFormID = overrideAddon->GetFormID()});
      }
    }
  }
  return definition;
}

auto CollectDavReplacementData(
    const std::vector<const RE::TESObjectARMO *> &a_overrideArmors)
    -> DavReplacementData {
  DavReplacementData data;
  std::unordered_set<std::string> replacementSet;

  for (const auto *overrideArmor : a_overrideArmors) {
    if (!overrideArmor) {
      continue;
    }

    if (!data.displayName.empty()) {
      data.displayName.append(" + ");
    }
    data.displayName.append(sosr::armor::GetDisplayName(overrideArmor));

    for (const auto *overrideAddon : overrideArmor->armorAddons) {
      if (!overrideAddon) {
        continue;
      }

      const auto identifier =
          sosr::armor::GetReplacementIdentifier(overrideArmor, overrideAddon);
      if (identifier.empty()) {
        continue;
      }

      if (replacementSet.insert(identifier).second) {
        data.replacements.push_back(identifier);
      }
    }
  }

  return data;
}

auto BuildDavVariantJson(
    const RE::TESObjectARMO *a_sourceArmor,
    const std::vector<const RE::TESObjectARMO *> &a_overrideArmors,
    bool a_hideEquipped, const std::int32_t a_priority) -> std::string {
  nlohmann::json replaceByForm = nlohmann::json::object();
  const auto replacementData = CollectDavReplacementData(a_overrideArmors);
  if (replacementData.replacements.empty() && !a_hideEquipped) {
    return {};
  }

  for (const auto *sourceAddon : a_sourceArmor->armorAddons) {
    if (!sourceAddon) {
      continue;
    }

    const auto sourceIdentifier = sosr::armor::GetFormIdentifier(sourceAddon);
    if (sourceIdentifier.empty()) {
      continue;
    }

    if (a_hideEquipped) {
      replaceByForm[sourceIdentifier] = nlohmann::json::array();
    } else if (replacementData.replacements.size() == 1) {
      replaceByForm[sourceIdentifier] = replacementData.replacements.front();
    } else {
      replaceByForm[sourceIdentifier] = replacementData.replacements;
    }
  }

  if (replaceByForm.empty()) {
    return {};
  }

  nlohmann::json root{
      {"displayName",
       a_hideEquipped ? "Hidden " + sosr::armor::GetDisplayName(a_sourceArmor)
                      : (replacementData.displayName.empty()
                             ? sosr::armor::GetDisplayName(a_sourceArmor)
                             : replacementData.displayName)},
      {"priority", a_priority},
      {"replaceByForm", replaceByForm}};
  return root.dump();
}

auto BuildDavSlotVariantJson(
    const std::uint64_t a_slotMask,
    const std::vector<const RE::TESObjectARMO *> &a_overrideArmors,
    const bool a_hideEquipped, const std::int32_t a_priority) -> std::string {
  const auto slotNumber = sosr::armor::GetArmorSlotNumber(a_slotMask);
  if (slotNumber == 0) {
    return {};
  }

  const auto replacementData = CollectDavReplacementData(a_overrideArmors);
  if (replacementData.replacements.empty() && !a_hideEquipped) {
    return {};
  }

  nlohmann::json replaceBySlot = nlohmann::json::object();
  if (a_hideEquipped) {
    replaceBySlot[std::to_string(slotNumber)] = nlohmann::json::array();
  } else if (replacementData.replacements.size() == 1) {
    replaceBySlot[std::to_string(slotNumber)] =
        replacementData.replacements.front();
  } else {
    replaceBySlot[std::to_string(slotNumber)] = replacementData.replacements;
  }

  const auto slotLabel =
      sosr::armor::JoinStrings(sosr::armor::GetArmorSlotLabels(a_slotMask));
  nlohmann::json root{
      {"displayName", a_hideEquipped ? "Hidden " + slotLabel
                                     : (replacementData.displayName.empty()
                                            ? slotLabel
                                            : replacementData.displayName)},
      {"priority", a_priority},
      {"replaceBySlot", replaceBySlot}};
  return root.dump();
}

auto BuildDavVariantDescriptor(
    const sosr::workbench::VariantWorkbenchRow &a_row,
    const std::vector<const RE::TESObjectARMO *> &a_overrideArmors,
    const std::int32_t a_priority) -> std::optional<DavVariantDescriptor> {
  DavVariantDescriptor descriptor;
  descriptor.name = "SOSR|Row|" + a_row.key;

  if (a_row.IsSlotRow()) {
    descriptor.json =
        BuildDavSlotVariantJson(a_row.equipped.slotMask, a_overrideArmors,
                                a_row.hideEquipped, a_priority);
  } else {
    const auto *sourceArmor =
        RE::TESForm::LookupByID<RE::TESObjectARMO>(a_row.equipped.formID);
    if (!sourceArmor) {
      return std::nullopt;
    }

    descriptor.json = BuildDavVariantJson(sourceArmor, a_overrideArmors,
                                          a_row.hideEquipped, a_priority);
  }

  if (descriptor.name.empty() || descriptor.json.empty()) {
    return std::nullopt;
  }

  descriptor.visualDefinition = BuildVisualDefinition(
      a_row, a_overrideArmors, descriptor.name, a_priority, false);

  return descriptor;
}
} // namespace

namespace sosr::workbench {
bool VariantWorkbench::ApplyCatalogPreview(
    std::string_view a_selectionKey, const std::vector<RE::FormID> &a_formIDs,
    RE::Actor *a_actor, const std::vector<int> *a_candidateRowIndices) {
  std::vector<PlannedCatalogAssignment> assignments;
  if (!PlanCatalogAssignments(a_formIDs, assignments, a_candidateRowIndices)) {
    ClearPreview();
    return false;
  }

  std::unordered_map<std::string, std::vector<const RE::TESObjectARMO *>>
      previewOverridesByRow;
  previewOverridesByRow.reserve(assignments.size());

  for (const auto &assignment : assignments) {
    const auto &targetRow =
        rows_[static_cast<std::size_t>(assignment.rowIndex)];
    const auto *overrideArmor =
        RE::TESForm::LookupByID<RE::TESObjectARMO>(assignment.armorFormID);
    if (!overrideArmor) {
      continue;
    }

    previewOverridesByRow[targetRow.key].push_back(overrideArmor);
  }

  if (previewOverridesByRow.empty()) {
    ClearPreview();
    return false;
  }

  std::unordered_map<std::string, std::string> desiredPreviewVariants;
  std::unordered_map<std::string, sosr::integrations::VisualVariantDefinition>
      desiredPreviewVisualDefinitions;
  desiredPreviewVariants.reserve(previewOverridesByRow.size());
  desiredPreviewVisualDefinitions.reserve(previewOverridesByRow.size());

  for (const auto &[rowKey, overrideArmors] : previewOverridesByRow) {
    const auto rowIt =
        std::ranges::find(rows_, rowKey, [](const VariantWorkbenchRow &a_row) {
          return a_row.key;
        });
    if (rowIt == rows_.end()) {
      continue;
    }

    const auto rowIndex =
        static_cast<std::size_t>(std::distance(rows_.begin(), rowIt));

    const auto *sourceArmor =
        RE::TESForm::LookupByID<RE::TESObjectARMO>(rowIt->equipped.formID);
    if (!sourceArmor) {
      continue;
    }

    const auto variantJson =
        BuildDavVariantJson(sourceArmor, overrideArmors, false,
                            BuildDavVariantPriority(rowIndex, rows_.size()));
    if (variantJson.empty()) {
      continue;
    }

    const auto variantName = BuildPreviewVariantName(rowKey);
    const auto priority = BuildDavVariantPriority(rowIndex, rows_.size());
    desiredPreviewVariants.emplace(variantName, variantJson);
    desiredPreviewVisualDefinitions.emplace(
        variantName, BuildVisualDefinition(*rowIt, overrideArmors, variantName,
                                           priority, true));
  }

  if (desiredPreviewVariants.empty()) {
    ClearPreview();
    return false;
  }

  auto *dav = GetDynamicArmorVariantsExtendedClient();
  if (!(dav = sosr::integrations::DynamicArmorVariantsExtendedClient::
            GetReady())) {
    ClearPreview();
    return false;
  }

  if (!a_actor) {
    a_actor = RE::PlayerCharacter::GetSingleton();
  }
  if (!a_actor) {
    ClearPreview();
    return false;
  }

  if (previewSelectionKey_ == a_selectionKey &&
      previewActorFormID_ == a_actor->GetFormID() &&
      previewDavVariants_ == desiredPreviewVariants) {
    return true;
  }

  ClearPreview();

  std::unordered_map<std::string, std::string> appliedPreviewVariants;
  appliedPreviewVariants.reserve(desiredPreviewVariants.size());
  std::vector<sosr::integrations::VisualVariantDefinition>
      appliedPreviewVisualDefinitions;
  appliedPreviewVisualDefinitions.reserve(desiredPreviewVariants.size());
  std::uint64_t previewOverrideSequence = 0;
  for (const auto &[variantName, variantJson] : desiredPreviewVariants) {
    if (!dav->RegisterVariantJson(variantName.c_str(), variantJson.c_str())) {
      logger::warn("Failed to register SOSR preview variant {}", variantName);
      for (const auto &[appliedVariantName, _] : appliedPreviewVariants) {
        dav->RemoveVariantOverride(a_actor, appliedVariantName.c_str());
        dav->DeleteVariant(appliedVariantName.c_str());
      }
      ClearPreview();
      return false;
    }

    if (!dav->ApplyVariantOverride(a_actor, variantName.c_str(), true)) {
      logger::warn("Failed to apply SOSR preview variant override {}",
                   variantName);
      dav->DeleteVariant(variantName.c_str());
      for (const auto &[appliedVariantName, _] : appliedPreviewVariants) {
        dav->RemoveVariantOverride(a_actor, appliedVariantName.c_str());
        dav->DeleteVariant(appliedVariantName.c_str());
      }
      ClearPreview();
      return false;
    }

    appliedPreviewVariants.emplace(variantName, variantJson);
    if (const auto definitionIt =
            desiredPreviewVisualDefinitions.find(variantName);
        definitionIt != desiredPreviewVisualDefinitions.end()) {
      auto definition = definitionIt->second;
      definition.overrideSequence = ++previewOverrideSequence;
      appliedPreviewVisualDefinitions.push_back(std::move(definition));
    }
  }

  previewSelectionKey_ = a_selectionKey;
  previewActorFormID_ = a_actor->GetFormID();
  previewDavVariants_ = std::move(desiredPreviewVariants);
  sosr::integrations::VisualStateService::Get().SetPreviewDefinitions(
      a_actor, std::move(appliedPreviewVisualDefinitions));
  return true;
}

bool VariantWorkbench::PreviewKitLayout(
    std::string_view a_selectionKey, const KitEntry::Layout &a_layout,
    RE::Actor *a_actor, const std::vector<int> *a_candidateRowIndices) {
  if (!a_actor) {
    a_actor = RE::PlayerCharacter::GetSingleton();
  }
  if (!a_actor) {
    ClearPreview();
    return false;
  }

  std::unordered_map<std::string, std::string> desiredPreviewVariants;
  std::unordered_map<std::string, sosr::integrations::VisualVariantDefinition>
      desiredPreviewVisualDefinitions;
  const auto resolveOverrideArmorsFromItems =
      [](const std::vector<EquipmentWidgetItem> &a_overrides) {
        std::vector<const RE::TESObjectARMO *> overrideArmors;
        overrideArmors.reserve(a_overrides.size());
        for (const auto &overrideItem : a_overrides) {
          if (const auto *overrideArmor =
                  RE::TESForm::LookupByID<RE::TESObjectARMO>(
                      overrideItem.formID)) {
            overrideArmors.push_back(overrideArmor);
          }
        }
        return overrideArmors;
      };

  const auto projection = ProjectKitLayoutRows(
      a_layout, a_candidateRowIndices, KitLayoutFallbackMode::SlotTargetsOnly,
      std::nullopt, true);
  std::size_t priorityCount = rows_.size();
  for (const auto &projectedRow : projection.rows) {
    priorityCount =
        (std::max)(priorityCount, projectedRow.priorityRowIndex + 1);
  }

  for (const auto &projectedRow : projection.rows) {
    const auto &row = projectedRow.row;
    const auto overrideArmors = resolveOverrideArmorsFromItems(row.overrides);
    const auto descriptor = BuildDavVariantDescriptor(
        row, overrideArmors,
        BuildDavVariantPriority(projectedRow.priorityRowIndex, priorityCount));
    if (!descriptor.has_value()) {
      continue;
    }

    const auto variantName = BuildPreviewVariantName(descriptor->name);
    desiredPreviewVariants.emplace(variantName, descriptor->json);
    auto visualDefinition = descriptor->visualDefinition;
    visualDefinition.variantId = variantName;
    visualDefinition.preview = true;
    desiredPreviewVisualDefinitions.emplace(variantName,
                                            std::move(visualDefinition));
  }

  if (desiredPreviewVariants.empty()) {
    ClearPreview();
    return false;
  }

  auto *dav = GetDynamicArmorVariantsExtendedClient();
  if (!(dav = sosr::integrations::DynamicArmorVariantsExtendedClient::
            GetReady())) {
    ClearPreview();
    return false;
  }

  if (previewSelectionKey_ == a_selectionKey &&
      previewActorFormID_ == a_actor->GetFormID() &&
      previewDavVariants_ == desiredPreviewVariants) {
    return true;
  }

  ClearPreview();

  std::unordered_map<std::string, std::string> appliedPreviewVariants;
  appliedPreviewVariants.reserve(desiredPreviewVariants.size());
  std::vector<sosr::integrations::VisualVariantDefinition>
      appliedPreviewVisualDefinitions;
  appliedPreviewVisualDefinitions.reserve(desiredPreviewVariants.size());
  std::uint64_t previewOverrideSequence = 0;
  for (const auto &[variantName, variantJson] : desiredPreviewVariants) {
    if (!dav->RegisterVariantJson(variantName.c_str(), variantJson.c_str())) {
      logger::warn("Failed to register SOSR preview variant {}", variantName);
      for (const auto &[appliedVariantName, _] : appliedPreviewVariants) {
        dav->RemoveVariantOverride(a_actor, appliedVariantName.c_str());
        dav->DeleteVariant(appliedVariantName.c_str());
      }
      ClearPreview();
      return false;
    }

    if (!dav->ApplyVariantOverride(a_actor, variantName.c_str(), true)) {
      logger::warn("Failed to apply SOSR preview variant override {}",
                   variantName);
      dav->DeleteVariant(variantName.c_str());
      for (const auto &[appliedVariantName, _] : appliedPreviewVariants) {
        dav->RemoveVariantOverride(a_actor, appliedVariantName.c_str());
        dav->DeleteVariant(appliedVariantName.c_str());
      }
      ClearPreview();
      return false;
    }

    appliedPreviewVariants.emplace(variantName, variantJson);
    if (const auto definitionIt =
            desiredPreviewVisualDefinitions.find(variantName);
        definitionIt != desiredPreviewVisualDefinitions.end()) {
      auto definition = definitionIt->second;
      definition.overrideSequence = ++previewOverrideSequence;
      appliedPreviewVisualDefinitions.push_back(std::move(definition));
    }
  }

  previewSelectionKey_ = a_selectionKey;
  previewActorFormID_ = a_actor->GetFormID();
  previewDavVariants_ = std::move(desiredPreviewVariants);
  sosr::integrations::VisualStateService::Get().SetPreviewDefinitions(
      a_actor, std::move(appliedPreviewVisualDefinitions));
  return true;
}

void VariantWorkbench::ClearPreview() {
  if (previewSelectionKey_.empty() && previewDavVariants_.empty()) {
    return;
  }

  const auto previewActorFormID = previewActorFormID_;
  auto *dav = GetDynamicArmorVariantsExtendedClient();
  auto *actor = previewActorFormID_ != 0
                    ? RE::TESForm::LookupByID<RE::Actor>(previewActorFormID_)
                    : RE::PlayerCharacter::GetSingleton();
  bool variantsChanged = false;

  if (dav) {
    if (actor) {
      for (const auto &[variantName, _] : previewDavVariants_) {
        if (!dav->RemoveVariantOverride(actor, variantName.c_str())) {
          logger::warn("Failed to remove SOSR preview variant override {}",
                       variantName);
        }
      }
    }
    for (const auto &[variantName, _] : previewDavVariants_) {
      if (!dav->DeleteVariant(variantName.c_str())) {
        logger::warn("Failed to delete SOSR preview variant {}", variantName);
      } else {
        variantsChanged = true;
      }
    }
    if (variantsChanged && actor) {
      dav->RefreshActor(actor);
    }
  }

  previewSelectionKey_.clear();
  previewActorFormID_ = 0;
  previewDavVariants_.clear();
  if (previewActorFormID != 0) {
    sosr::integrations::VisualStateService::Get().ClearPreview(
        previewActorFormID);
  }
}

void VariantWorkbench::SyncDynamicArmorVariantsExtended(
    std::vector<conditions::Definition> &a_conditions) {
  auto *dav =
      sosr::integrations::DynamicArmorVariantsExtendedClient::GetReady();
  if (!dav) {
    return;
  }

  std::unordered_map<std::string, DavVariantPayload> desiredVariants;
  desiredVariants.reserve(rows_.size());

  for (std::size_t rowIndex = 0; rowIndex < rows_.size(); ++rowIndex) {
    const auto &row = rows_[rowIndex];
    std::vector<const RE::TESObjectARMO *> overrideArmors;
    overrideArmors.reserve(row.overrides.size());
    for (const auto &overrideItem : row.overrides) {
      if (const auto *overrideArmor =
              RE::TESForm::LookupByID<RE::TESObjectARMO>(overrideItem.formID)) {
        overrideArmors.push_back(overrideArmor);
      }
    }

    if (overrideArmors.empty() && !row.hideEquipped) {
      continue;
    }
    if (!row.conditionId.has_value()) {
      continue;
    }
    const auto *condition =
        sosr::conditions::FindDefinitionById(a_conditions, *row.conditionId);
    if (condition == nullptr ||
        !sosr::conditions::IsWorkbenchSelectable(*condition)) {
      continue;
    }
    if (!sosr::conditions::EvaluateDefinitionStatus(*condition, a_conditions)
             .IsActive()) {
      continue;
    }

    const auto descriptor = BuildDavVariantDescriptor(
        row, overrideArmors, BuildDavVariantPriority(rowIndex, rows_.size()));
    if (!descriptor.has_value()) {
      continue;
    }

    const auto materializedCondition =
        sosr::conditions::MaterializeConditionById(*row.conditionId,
                                                   a_conditions);
    if (!materializedCondition.has_value()) {
      continue;
    }

    auto visualDefinition = descriptor->visualDefinition;
    visualDefinition.conditionSignature = materializedCondition->signature;
    visualDefinition.condition = materializedCondition->condition;

    desiredVariants.insert_or_assign(
        descriptor->name,
        DavVariantPayload{
            .variantJson = std::move(descriptor->json),
            .conditionSignature = materializedCondition->signature,
            .condition = materializedCondition->condition,
            .refreshTargets = materializedCondition->refreshTargets,
            .visualDefinition = std::move(visualDefinition)});
  }

  std::unordered_map<std::string, ActiveDavVariantState> syncedVariants;
  syncedVariants.reserve(desiredVariants.size());
  bool variantsChanged = false;
  sosr::conditions::RefreshTargets refreshTargets;

  for (const auto &[variantName, payload] : desiredVariants) {
    if (const auto activeIt = activeDavVariants_.find(variantName);
        activeIt != activeDavVariants_.end() &&
        activeIt->second.variantJson == payload.variantJson &&
        activeIt->second.conditionSignature == payload.conditionSignature) {
      syncedVariants.emplace(variantName,
                             ActiveDavVariantState{payload.variantJson,
                                                   payload.conditionSignature,
                                                   payload.refreshTargets});
      continue;
    }

    const auto activeIt = activeDavVariants_.find(variantName);
    if (!dav->RegisterVariantJson(variantName.c_str(),
                                  payload.variantJson.c_str())) {
      logger::warn("Failed to register DAV variant {}", variantName);
      continue;
    }

    if (!dav->SetCondition(variantName.c_str(), payload.condition)) {
      logger::warn("Failed to set DAV condition for {}", variantName);
      dav->DeleteVariant(variantName.c_str());
      continue;
    }

    syncedVariants.emplace(variantName,
                           ActiveDavVariantState{payload.variantJson,
                                                 payload.conditionSignature,
                                                 payload.refreshTargets});
    if (activeIt != activeDavVariants_.end()) {
      sosr::conditions::MergeRefreshTargets(refreshTargets,
                                            activeIt->second.refreshTargets);
    }
    sosr::conditions::MergeRefreshTargets(refreshTargets,
                                          payload.refreshTargets);
    variantsChanged = true;
  }

  for (const auto &[variantName, state] : activeDavVariants_) {
    if (syncedVariants.contains(variantName)) {
      continue;
    }

    if (!dav->DeleteVariant(variantName.c_str())) {
      logger::warn("Failed to delete DAV variant {}", variantName);
    } else {
      sosr::conditions::MergeRefreshTargets(refreshTargets,
                                            state.refreshTargets);
      variantsChanged = true;
    }
  }

  std::vector<sosr::integrations::VisualVariantDefinition>
      syncedVisualDefinitions;
  syncedVisualDefinitions.reserve(syncedVariants.size());
  for (const auto &[variantName, payload] : desiredVariants) {
    if (syncedVariants.contains(variantName)) {
      syncedVisualDefinitions.push_back(payload.visualDefinition);
    }
  }

  activeDavVariants_ = std::move(syncedVariants);

  if (variantsChanged) {
    sosr::conditions::RefreshActors(*dav, refreshTargets);
  }
  sosr::integrations::VisualStateService::Get().SetPersistentDefinitions(
      std::move(syncedVisualDefinitions));
}

void VariantWorkbench::Revert() {
  ClearPreview();

  auto *dav = GetDynamicArmorVariantsExtendedClient();
  bool variantsChanged = false;
  sosr::conditions::RefreshTargets refreshTargets;

  if (dav) {
    for (const auto &[variantName, state] : activeDavVariants_) {
      if (!dav->DeleteVariant(variantName.c_str())) {
        logger::warn("Failed to delete DAV variant {} during SOSR revert",
                     variantName);
      } else {
        sosr::conditions::MergeRefreshTargets(refreshTargets,
                                              state.refreshTargets);
        variantsChanged = true;
      }
    }
    if (variantsChanged) {
      sosr::conditions::RefreshActors(*dav, refreshTargets);
    }
  }

  rows_.clear();
  RebuildRowOrder();
  activeDavVariants_.clear();
  sosr::integrations::VisualStateService::Get().ClearAll();
  MarkChanged();
}
} // namespace sosr::workbench
