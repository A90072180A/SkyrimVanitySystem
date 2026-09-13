#include "PresetTransferUi.h"

#include "PresetTransfer.h"
#include "ui/Menu.h"

#include "imgui.h"

#include <array>
#include <cstdio>
#include <format>
#include <string>

namespace {
struct PresetTransferUiState {
  bool openRequested{false};
  bool initialized{false};
  std::array<char, 768> path{};
  std::array<char, 160> name{};
  sosr::presets::ImportPlan plan{};
  sosr::presets::ApplyResult result{};
  bool hasPlan{false};
  bool hasResult{false};
  std::string status;
  bool statusIsError{false};
};

PresetTransferUiState &GetState() {
  static PresetTransferUiState state;
  return state;
}

void InitializeState(PresetTransferUiState &a_state) {
  if (a_state.initialized) {
    return;
  }

  constexpr auto defaultPath =
      "Data/SKSE/Plugins/SkyrimVanitySystem/Presets/SVS-Preset.svspreset";
  std::snprintf(a_state.path.data(), a_state.path.size(), "%s", defaultPath);
  a_state.initialized = true;
}

const char *ActionLabel(const sosr::presets::ConditionImportAction a_action) {
  switch (a_action) {
  case sosr::presets::ConditionImportAction::ReuseExisting:
    return "Reuse";
  case sosr::presets::ConditionImportAction::Add:
    return "Add";
  case sosr::presets::ConditionImportAction::AddRenamed:
    return "Add as copy";
  }
  return "Unknown";
}

void DrawPlanSummary(const sosr::presets::ImportPlan &a_plan) {
  ImGui::SeparatorText("Import preview");
  if (!a_plan.presetName.empty()) {
    ImGui::Text("Preset: %s", a_plan.presetName.c_str());
  }
  if (!a_plan.createdWith.empty()) {
    ImGui::TextDisabled("Created with %s", a_plan.createdWith.c_str());
  }

  if (ImGui::BeginTable("##preset-plan-summary", 2,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_SizingStretchProp)) {
    const auto row = [](const char *a_label, const std::size_t a_value) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(a_label);
      ImGui::TableSetColumnIndex(1);
      ImGui::Text("%zu", a_value);
    };
    row("Conditions to add", a_plan.addedConditions);
    row("Conditions reused", a_plan.reusedConditions);
    row("Conditions renamed", a_plan.renamedConditions);
    row("Workbench rows resolved", a_plan.resolvedWorkbenchRows);
    row("Workbench rows in preset", a_plan.sourceWorkbenchRows);
    row("Workbench rows skipped", a_plan.missingWorkbenchRows);
    row("Override items missing", a_plan.missingOverrideItems);
    row("Unresolved condition references",
        a_plan.unresolvedConditionReferences);
    ImGui::EndTable();
  }

  if (!a_plan.conditions.empty() &&
      ImGui::CollapsingHeader("Condition mapping",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    if (ImGui::BeginChild("##preset-condition-map", ImVec2(0.0f, 210.0f),
                          ImGuiChildFlags_Borders)) {
      if (ImGui::BeginTable("##preset-condition-map-table", 4,
                            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_Resizable |
                                ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Source");
        ImGui::TableSetupColumn("Action");
        ImGui::TableSetupColumn("Target");
        ImGui::TableSetupColumn("Target ID");
        ImGui::TableHeadersRow();
        for (const auto &entry : a_plan.conditions) {
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          ImGui::TextUnformatted(entry.sourceName.c_str());
          ImGui::TableSetColumnIndex(1);
          ImGui::TextUnformatted(ActionLabel(entry.action));
          ImGui::TableSetColumnIndex(2);
          ImGui::TextUnformatted(entry.targetName.c_str());
          ImGui::TableSetColumnIndex(3);
          ImGui::TextUnformatted(entry.targetId.c_str());
        }
        ImGui::EndTable();
      }
    }
    ImGui::EndChild();
  }

  if (!a_plan.missingFormIdentifiers.empty() &&
      ImGui::CollapsingHeader("Missing forms / mods")) {
    ImGui::TextWrapped(
        "These entries are not currently resolvable and will be skipped where "
        "possible. This usually means the source save used a mod that is not "
        "loaded now.");
    if (ImGui::BeginChild("##preset-missing-forms", ImVec2(0.0f, 130.0f),
                          ImGuiChildFlags_Borders)) {
      for (const auto &identifier : a_plan.missingFormIdentifiers) {
        ImGui::BulletText("%s", identifier.c_str());
      }
    }
    ImGui::EndChild();
  }

  if (!a_plan.unresolvedConditionIds.empty() &&
      ImGui::CollapsingHeader("Unresolved condition IDs")) {
    for (const auto &identifier : a_plan.unresolvedConditionIds) {
      ImGui::BulletText("%s", identifier.c_str());
    }
  }
}

void DrawApplyResult(const sosr::presets::ApplyResult &a_result) {
  ImGui::SeparatorText("Last merge result");
  if (ImGui::BeginTable("##preset-apply-result", 2,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_SizingStretchProp)) {
    const auto row = [](const char *a_label, const std::size_t a_value) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(a_label);
      ImGui::TableSetColumnIndex(1);
      ImGui::Text("%zu", a_value);
    };
    row("Conditions added", a_result.addedConditions);
    row("Conditions reused", a_result.reusedConditions);
    row("Conditions renamed", a_result.renamedConditions);
    row("Workbench rows added", a_result.addedWorkbenchRows);
    row("Workbench rows merged", a_result.mergedWorkbenchRows);
    row("Final workbench rows", a_result.finalWorkbenchRows);
    row("Workbench rows skipped", a_result.missingWorkbenchRows);
    row("Override items missing", a_result.missingOverrideItems);
    ImGui::EndTable();
  }
}
} // namespace

namespace sosr::ui::preset_transfer {
void RequestOpen() { GetState().openRequested = true; }

void DrawDialog(Menu &a_menu) {
  auto &state = GetState();
  InitializeState(state);

  constexpr auto popupId = "SVS Presets##preset-transfer";
  constexpr auto confirmId = "Confirm preset merge##preset-transfer";

  if (state.openRequested) {
    state.openRequested = false;
    ImGui::OpenPopup(popupId);
  }

  ImGui::SetNextWindowSize(ImVec2(720.0f, 680.0f), ImGuiCond_FirstUseEver);
  bool keepOpen = true;
  if (ImGui::BeginPopupModal(popupId, &keepOpen,
                             ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::TextWrapped(
        "Export the current save's SVS conditions and workbench rules, or "
        "preview and merge a preset created by another save. Import uses "
        "additive merge semantics and does not replace the current setup.");
    ImGui::Spacing();

    ImGui::TextUnformatted("Preset file");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputText("##preset-path", state.path.data(),
                         state.path.size())) {
      state.hasPlan = false;
    }

    ImGui::TextUnformatted("Preset name (optional when exporting)");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##preset-name", state.name.data(), state.name.size());

    if (ImGui::Button("Export current setup")) {
      std::string error;
      if (sosr::presets::ExportAll(a_menu, state.path.data(), state.name.data(),
                                   error)) {
        state.status = std::format("Exported preset to {}", state.path.data());
        state.statusIsError = false;
      } else {
        state.status = std::move(error);
        state.statusIsError = true;
      }
    }

    ImGui::SameLine();
    if (ImGui::Button("Preview import")) {
      state.plan = {};
      state.hasPlan = false;
      state.hasResult = false;
      std::string error;
      if (sosr::presets::BuildImportPlan(a_menu, state.path.data(), state.plan,
                                         error)) {
        state.hasPlan = true;
        state.status = "Import preview is ready. Review it before applying.";
        state.statusIsError = false;
      } else {
        state.status = std::move(error);
        state.statusIsError = true;
      }
    }

    if (!state.status.empty()) {
      ImGui::Spacing();
      if (state.statusIsError) {
        ImGui::TextWrapped("Error: %s", state.status.c_str());
      } else {
        ImGui::TextWrapped("%s", state.status.c_str());
      }
    }

    if (state.hasPlan) {
      DrawPlanSummary(state.plan);
      ImGui::Spacing();

      const bool canApply = state.plan.CanApply();
      if (!canApply) {
        ImGui::TextWrapped(
            "This preset cannot be applied until all condition references can "
            "be resolved.");
      }
      if (!canApply) {
        ImGui::BeginDisabled();
      }
      if (ImGui::Button("Apply merge...")) {
        ImGui::OpenPopup(confirmId);
      }
      if (!canApply) {
        ImGui::EndDisabled();
      }
    }

    if (state.hasResult) {
      DrawApplyResult(state.result);
    }

    ImGui::Separator();
    if (ImGui::Button("Close")) {
      ImGui::CloseCurrentPopup();
    }

    if (ImGui::BeginPopupModal(confirmId, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings)) {
      ImGui::TextWrapped(
          "Merge this preset into the current save? Existing SVS conditions "
          "and workbench rows are preserved. Matching rows are merged and "
          "missing mod forms are skipped. A rollback snapshot is used if the "
          "commit fails.");
      ImGui::Spacing();
      if (ImGui::Button("Apply merge", ImVec2(130.0f, 0.0f))) {
        state.result = {};
        std::string error;
        if (sosr::presets::ApplyImportPlan(a_menu, state.plan, state.result,
                                           error)) {
          state.hasResult = true;
          state.status = "Preset merged successfully.";
          state.statusIsError = false;
          // Refresh the preview so the dialog reflects the newly committed
          // state and repeated apply attempts do not operate on a stale plan.
          sosr::presets::ImportPlan refreshedPlan;
          std::string refreshError;
          if (sosr::presets::BuildImportPlan(a_menu, state.path.data(),
                                             refreshedPlan, refreshError)) {
            state.plan = std::move(refreshedPlan);
            state.hasPlan = true;
          } else {
            state.hasPlan = false;
          }
        } else {
          state.hasResult = false;
          state.status = std::move(error);
          state.statusIsError = true;
        }
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f))) {
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }

    ImGui::EndPopup();
  }
}
} // namespace sosr::ui::preset_transfer
