#include "integrations/VisualStateService.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <unordered_set>
#include <utility>

namespace {
using SkyrimVanitySystemAPI::VisualPiece001;
using SkyrimVanitySystemAPI::VisualState001;
using sosr::integrations::VisualVariantDefinition;

struct OwnedVisualPiece {
  VisualPiece001 view;
  std::string variantId;
  std::string maleModelPath;
  std::string femaleModelPath;
  std::string actorModelPath;
};

struct VisualStateStorage {
  VisualState001 view;
  std::vector<OwnedVisualPiece> ownedPieces;
  std::vector<VisualPiece001> pieceViews;

  void PrepareViews() {
    pieceViews.clear();
    pieceViews.reserve(ownedPieces.size());
    for (auto &owned : ownedPieces) {
      owned.view.variantId = owned.variantId.c_str();
      owned.view.maleModelPath = owned.maleModelPath.c_str();
      owned.view.femaleModelPath = owned.femaleModelPath.c_str();
      owned.view.actorModelPath = owned.actorModelPath.c_str();
      pieceViews.push_back(owned.view);
    }

    view.pieces = pieceViews.data();
    view.pieceCount = static_cast<std::uint32_t>((std::min)(
        pieceViews.size(),
        static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())));
  }
};

struct WornArmor {
  RE::TESObjectARMO *armor{nullptr};
  RE::TESObjectARMA *armorAddon{nullptr};
};

auto CollectWornArmorAddons(RE::Actor *a_actor) -> std::vector<WornArmor> {
  std::vector<WornArmor> worn;
  if (!a_actor) {
    return worn;
  }

  std::unordered_set<RE::FormID> seenArmorForms;
  for (std::uint32_t slotIndex = 0; slotIndex < 32; ++slotIndex) {
    const auto slot = static_cast<RE::BGSBipedObjectForm::BipedObjectSlot>(
        static_cast<std::uint32_t>(1u << slotIndex));
    auto *armor = a_actor->GetWornArmor(slot);
    if (!armor || !seenArmorForms.insert(armor->GetFormID()).second) {
      continue;
    }

    for (auto *armorAddon : armor->armorAddons) {
      if (armorAddon) {
        worn.push_back({armor, armorAddon});
      }
    }
  }
  return worn;
}

bool MatchesSource(const VisualVariantDefinition &a_definition,
                   const WornArmor &a_worn) {
  if (!a_worn.armor || !a_worn.armorAddon) {
    return false;
  }

  if (!a_definition.slotTriggered) {
    return a_definition.sourceArmorFormID == a_worn.armor->GetFormID();
  }

  const auto sourceSlotMask = a_worn.armorAddon->GetSlotMask().underlying();
  return a_definition.triggerSlotMask != 0 &&
         (sourceSlotMask & a_definition.triggerSlotMask) ==
             a_definition.triggerSlotMask;
}

bool IsConditionActive(const VisualVariantDefinition &a_definition,
                       RE::Actor *a_actor) {
  return a_definition.preview ||
         (a_definition.condition &&
          a_definition.condition->IsTrue(a_actor, a_actor));
}

auto SelectDefinition(
    const WornArmor &a_worn, RE::Actor *a_actor,
    const std::vector<VisualVariantDefinition> &a_persistentDefinitions,
    const std::vector<VisualVariantDefinition> &a_previewDefinitions)
    -> const VisualVariantDefinition * {
  const VisualVariantDefinition *selectedPreview = nullptr;
  for (const auto &definition : a_previewDefinitions) {
    if (!MatchesSource(definition, a_worn)) {
      continue;
    }
    if (!selectedPreview ||
        definition.overrideSequence > selectedPreview->overrideSequence) {
      selectedPreview = std::addressof(definition);
    }
  }
  if (selectedPreview) {
    return selectedPreview;
  }

  const VisualVariantDefinition *selectedPersistent = nullptr;
  for (const auto &definition : a_persistentDefinitions) {
    if (!MatchesSource(definition, a_worn) ||
        !IsConditionActive(definition, a_actor)) {
      continue;
    }
    if (!selectedPersistent ||
        definition.priority > selectedPersistent->priority ||
        (definition.priority == selectedPersistent->priority &&
         definition.variantId > selectedPersistent->variantId)) {
      selectedPersistent = std::addressof(definition);
    }
  }
  return selectedPersistent;
}

std::string CopyModelPath(const RE::TESModel &a_model) {
  const auto *path = a_model.GetModel();
  return path ? std::string(path) : std::string{};
}

auto BuildOwnedPiece(RE::Actor *a_actor, const WornArmor &a_worn,
                     const VisualVariantDefinition &a_definition,
                     RE::TESObjectARMO *a_replacementArmor,
                     RE::TESObjectARMA *a_replacementArmorAddon,
                     const bool a_hidden) -> OwnedVisualPiece {
  OwnedVisualPiece owned;
  owned.variantId = a_definition.variantId;

  auto &piece = owned.view;
  piece.actor = a_actor;
  piece.sourceArmor = a_worn.armor;
  piece.sourceArmorAddon = a_worn.armorAddon;
  piece.replacementArmor = a_replacementArmor;
  piece.replacementArmorAddon = a_replacementArmorAddon;
  piece.actorFormID = a_actor ? a_actor->GetFormID() : 0;
  piece.sourceArmorFormID = a_worn.armor ? a_worn.armor->GetFormID() : 0;
  piece.sourceArmorAddonFormID =
      a_worn.armorAddon ? a_worn.armorAddon->GetFormID() : 0;
  piece.replacementArmorFormID =
      a_replacementArmor ? a_replacementArmor->GetFormID() : 0;
  piece.replacementArmorAddonFormID =
      a_replacementArmorAddon ? a_replacementArmorAddon->GetFormID() : 0;
  piece.triggerSlotMask = a_definition.triggerSlotMask;
  piece.sourceSlotMask =
      a_worn.armorAddon ? a_worn.armorAddon->GetSlotMask().underlying() : 0;
  piece.visualSlotMask =
      a_replacementArmorAddon
          ? a_replacementArmorAddon->GetSlotMask().underlying()
          : 0;
  piece.variantPriority = a_definition.priority;
  if (a_definition.slotTriggered) {
    piece.flags |= SkyrimVanitySystemAPI::kVisualPiece_SlotTriggered;
  }
  if (a_definition.preview) {
    piece.flags |= SkyrimVanitySystemAPI::kVisualPiece_Preview;
  }
  if (a_hidden) {
    piece.flags |= SkyrimVanitySystemAPI::kVisualPiece_Hidden;
  }

  if (a_replacementArmorAddon) {
    owned.maleModelPath =
        CopyModelPath(a_replacementArmorAddon->bipedModels[0]);
    owned.femaleModelPath =
        CopyModelPath(a_replacementArmorAddon->bipedModels[1]);
    const auto *actorBase = a_actor ? a_actor->GetActorBase() : nullptr;
    const auto sexIndex =
        actorBase && actorBase->GetSex() == RE::SEX::kFemale ? 1 : 0;
    owned.actorModelPath =
        sexIndex == 1 ? owned.femaleModelPath : owned.maleModelPath;
  }
  return owned;
}

auto BuildVisualStateStorage(
    RE::Actor *a_actor,
    const std::vector<VisualVariantDefinition> &a_persistentDefinitions,
    const std::vector<VisualVariantDefinition> &a_previewDefinitions,
    const std::uint64_t a_revision) -> VisualStateStorage {
  VisualStateStorage storage;
  storage.view.actor = a_actor;
  storage.view.actorFormID = a_actor ? a_actor->GetFormID() : 0;
  storage.view.revision = a_revision;
  storage.view.flags =
      SkyrimVanitySystemAPI::kVisualState_SvsContributionsOnly |
      SkyrimVanitySystemAPI::kVisualState_PlayerOnly |
      SkyrimVanitySystemAPI::kVisualState_RefreshCompletionUnavailable |
      SkyrimVanitySystemAPI::kVisualState_LiveEvaluation;
  if (!a_previewDefinitions.empty()) {
    storage.view.flags |= SkyrimVanitySystemAPI::kVisualState_PreviewActive;
  }

  auto *race = a_actor ? a_actor->GetRace() : nullptr;
  const auto wornArmorAddons = CollectWornArmorAddons(a_actor);
  for (const auto &worn : wornArmorAddons) {
    if (race && worn.armorAddon && !worn.armorAddon->IsValidRace(race)) {
      continue;
    }

    const auto *definition = SelectDefinition(
        worn, a_actor, a_persistentDefinitions, a_previewDefinitions);
    if (!definition) {
      continue;
    }

    bool emittedReplacement = false;
    if (!definition->hideSource) {
      for (const auto &replacement : definition->replacements) {
        auto *replacementArmor =
            RE::TESForm::LookupByID<RE::TESObjectARMO>(replacement.armorFormID);
        auto *replacementAddon = RE::TESForm::LookupByID<RE::TESObjectARMA>(
            replacement.armorAddonFormID);
        if (!replacementAddon ||
            (race && !replacementAddon->IsValidRace(race))) {
          continue;
        }

        storage.ownedPieces.push_back(
            BuildOwnedPiece(a_actor, worn, *definition, replacementArmor,
                            replacementAddon, false));
        emittedReplacement = true;
      }
    }

    // DAVE treats an active empty/race-incompatible replacement list as a
    // suppressed source addon, the same observable result as explicit hide.
    if (!emittedReplacement) {
      storage.ownedPieces.push_back(
          BuildOwnedPiece(a_actor, worn, *definition, nullptr, nullptr, true));
    }
  }

  storage.PrepareViews();
  return storage;
}

bool SameDefinitionSet(const std::vector<VisualVariantDefinition> &a_left,
                       const std::vector<VisualVariantDefinition> &a_right) {
  if (a_left.size() != a_right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < a_left.size(); ++index) {
    if (!a_left[index].HasSameContract(a_right[index])) {
      return false;
    }
  }
  return true;
}

void SortDefinitions(std::vector<VisualVariantDefinition> &a_definitions) {
  std::ranges::sort(a_definitions, [](const auto &a_left, const auto &a_right) {
    return a_left.variantId < a_right.variantId;
  });
}
} // namespace

namespace sosr::integrations {
bool VisualVariantDefinition::HasSameContract(
    const VisualVariantDefinition &a_other) const {
  return variantId == a_other.variantId &&
         conditionSignature == a_other.conditionSignature &&
         replacements == a_other.replacements &&
         sourceArmorFormID == a_other.sourceArmorFormID &&
         triggerSlotMask == a_other.triggerSlotMask &&
         overrideSequence == a_other.overrideSequence &&
         priority == a_other.priority &&
         slotTriggered == a_other.slotTriggered &&
         hideSource == a_other.hideSource && preview == a_other.preview;
}

VisualStateService &VisualStateService::Get() {
  static VisualStateService singleton;
  return singleton;
}

void VisualStateService::InstallMessagingListener() {
  static std::atomic_bool installed{false};
  if (installed.exchange(true)) {
    return;
  }

  if (auto *messaging = SKSE::GetMessagingInterface(); messaging) {
    messaging->RegisterListener(nullptr, HandleInterfaceRequest);
  }
}

void VisualStateService::SetPersistentDefinitions(
    std::vector<VisualVariantDefinition> a_definitions) {
  SortDefinitions(a_definitions);
  bool changed = false;
  {
    std::scoped_lock lock(mutex_);
    changed = !SameDefinitionSet(persistentDefinitions_, a_definitions);
    persistentDefinitions_ = std::move(a_definitions);
    if (changed) {
      ++revision_;
    }
  }

  if (changed) {
    QueueNotification(RE::PlayerCharacter::GetSingleton());
  }
}

void VisualStateService::SetPreviewDefinitions(
    RE::Actor *a_actor, std::vector<VisualVariantDefinition> a_definitions) {
  if (!a_actor) {
    return;
  }

  SortDefinitions(a_definitions);
  bool changed = false;
  {
    std::scoped_lock lock(mutex_);
    auto &current = previewDefinitionsByActor_[a_actor->GetFormID()];
    changed = !SameDefinitionSet(current, a_definitions);
    current = std::move(a_definitions);
    if (changed) {
      ++revision_;
    }
  }

  if (changed) {
    QueueNotification(a_actor);
  }
}

void VisualStateService::ClearPreview(const RE::FormID a_actorFormID) {
  bool changed = false;
  {
    std::scoped_lock lock(mutex_);
    changed = previewDefinitionsByActor_.erase(a_actorFormID) != 0;
    if (changed) {
      ++revision_;
    }
  }

  if (changed) {
    QueueNotification(RE::TESForm::LookupByID<RE::Actor>(a_actorFormID));
  }
}

void VisualStateService::ClearAll() {
  bool changed = false;
  {
    std::scoped_lock lock(mutex_);
    changed =
        !persistentDefinitions_.empty() || !previewDefinitionsByActor_.empty();
    persistentDefinitions_.clear();
    previewDefinitionsByActor_.clear();
    if (changed) {
      ++revision_;
    }
  }

  if (changed) {
    QueueNotification(RE::PlayerCharacter::GetSingleton());
  }
}

bool VisualStateService::IsReady() const {
  return RE::PlayerCharacter::GetSingleton() != nullptr;
}

bool VisualStateService::VisitActorVisualState(
    RE::Actor *a_actor,
    const SkyrimVanitySystemAPI::VisualStateVisitor001 a_visitor,
    void *a_userData) const {
  auto *player = RE::PlayerCharacter::GetSingleton();
  if (!a_actor || !player || a_actor != player || !a_visitor) {
    return false;
  }

  std::vector<VisualVariantDefinition> persistentDefinitions;
  std::vector<VisualVariantDefinition> previewDefinitions;
  std::uint64_t revision = 0;
  {
    std::scoped_lock lock(mutex_);
    persistentDefinitions = persistentDefinitions_;
    if (const auto it = previewDefinitionsByActor_.find(a_actor->GetFormID());
        it != previewDefinitionsByActor_.end()) {
      previewDefinitions = it->second;
    }
    revision = revision_;
  }

  auto storage = BuildVisualStateStorage(a_actor, persistentDefinitions,
                                         previewDefinitions, revision);
  a_visitor(std::addressof(storage.view), a_userData);
  return true;
}

SkyrimVanitySystemAPI::ListenerHandle
VisualStateService::RegisterVisualStateChangedListener(
    const SkyrimVanitySystemAPI::VisualStateChangedCallback001 a_callback,
    void *a_userData) {
  if (!a_callback) {
    return 0;
  }

  std::scoped_lock lock(mutex_);
  const auto handle = nextListenerHandle_++;
  listeners_.emplace(handle, Listener{a_callback, a_userData});
  return handle;
}

bool VisualStateService::UnregisterVisualStateChangedListener(
    const SkyrimVanitySystemAPI::ListenerHandle a_listenerHandle) {
  std::scoped_lock lock(mutex_);
  return listeners_.erase(a_listenerHandle) != 0;
}

void VisualStateService::QueueNotification(RE::Actor *a_actor) {
  if (!a_actor) {
    return;
  }

  const auto handle = a_actor->GetHandle();
  if (auto *tasks = SKSE::GetTaskInterface(); tasks) {
    tasks->AddTask([handle] {
      if (auto actor = handle.get()) {
        VisualStateService::Get().NotifyListeners(actor.get());
      }
    });
    return;
  }

  NotifyListeners(a_actor);
}

void VisualStateService::NotifyListeners(RE::Actor *a_actor) const {
  if (!a_actor) {
    return;
  }

  std::vector<Listener> listeners;
  std::vector<VisualVariantDefinition> persistentDefinitions;
  std::vector<VisualVariantDefinition> previewDefinitions;
  std::uint64_t revision = 0;
  {
    std::scoped_lock lock(mutex_);
    listeners.reserve(listeners_.size());
    for (const auto &[_, listener] : listeners_) {
      listeners.push_back(listener);
    }
    persistentDefinitions = persistentDefinitions_;
    if (const auto it = previewDefinitionsByActor_.find(a_actor->GetFormID());
        it != previewDefinitionsByActor_.end()) {
      previewDefinitions = it->second;
    }
    revision = revision_;
  }

  if (listeners.empty()) {
    return;
  }

  auto storage = BuildVisualStateStorage(a_actor, persistentDefinitions,
                                         previewDefinitions, revision);
  for (const auto &listener : listeners) {
    listener.callback(std::addressof(storage.view), listener.userData);
  }
}

void *VisualStateService::GetApiFunction(const unsigned int a_revisionNumber) {
  return a_revisionNumber == SkyrimVanitySystemAPI::kInterfaceRevision001
             ? static_cast<
                   SkyrimVanitySystemAPI::ISkyrimVanitySystemInterface001 *>(
                   std::addressof(Get()))
             : nullptr;
}

void VisualStateService::HandleInterfaceRequest(
    SKSE::MessagingInterface::Message *a_message) {
  if (!a_message ||
      a_message->type != SkyrimVanitySystemAPI::SkyrimVanitySystemMessage::
                             kMessage_QueryInterface ||
      !a_message->data ||
      a_message->dataLen <
          sizeof(SkyrimVanitySystemAPI::SkyrimVanitySystemMessage)) {
    return;
  }

  auto *request =
      static_cast<SkyrimVanitySystemAPI::SkyrimVanitySystemMessage *>(
          a_message->data);
  request->GetApiFunction = GetApiFunction;
  logger::info("Provided Skyrim Vanity System visual-state API to {}",
               a_message->sender ? a_message->sender : "<unknown>");
}
} // namespace sosr::integrations
