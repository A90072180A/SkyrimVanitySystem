#pragma once

#include "api/SkyrimVanitySystemAPI.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sosr::integrations {
struct VisualReplacementDefinition {
  RE::FormID armorFormID{0};
  RE::FormID armorAddonFormID{0};

  [[nodiscard]] bool
  operator==(const VisualReplacementDefinition &a_other) const = default;
};

struct VisualVariantDefinition {
  std::string variantId;
  std::string conditionSignature;
  std::shared_ptr<RE::TESCondition> condition;
  std::vector<VisualReplacementDefinition> replacements;
  RE::FormID sourceArmorFormID{0};
  std::uint64_t triggerSlotMask{0};
  std::uint64_t overrideSequence{0};
  std::int32_t priority{0};
  bool slotTriggered{false};
  bool hideSource{false};
  bool preview{false};

  [[nodiscard]] bool
  HasSameContract(const VisualVariantDefinition &a_other) const;
};

class VisualStateService final
    : public SkyrimVanitySystemAPI::ISkyrimVanitySystemInterface001 {
public:
  static VisualStateService &Get();
  static void InstallMessagingListener();

  void
  SetPersistentDefinitions(std::vector<VisualVariantDefinition> a_definitions);
  void
  SetPreviewDefinitions(RE::Actor *a_actor,
                        std::vector<VisualVariantDefinition> a_definitions);
  void ClearPreview(RE::FormID a_actorFormID);
  void ClearAll();

  [[nodiscard]] bool IsReady() const override;
  bool
  VisitActorVisualState(RE::Actor *a_actor,
                        SkyrimVanitySystemAPI::VisualStateVisitor001 a_visitor,
                        void *a_userData) const override;
  SkyrimVanitySystemAPI::ListenerHandle RegisterVisualStateChangedListener(
      SkyrimVanitySystemAPI::VisualStateChangedCallback001 a_callback,
      void *a_userData) override;
  bool UnregisterVisualStateChangedListener(
      SkyrimVanitySystemAPI::ListenerHandle a_listenerHandle) override;

private:
  struct Listener {
    SkyrimVanitySystemAPI::VisualStateChangedCallback001 callback{nullptr};
    void *userData{nullptr};
  };

  VisualStateService() = default;

  static void
  HandleInterfaceRequest(SKSE::MessagingInterface::Message *a_message);
  static void *GetApiFunction(unsigned int a_revisionNumber);

  void QueueNotification(RE::Actor *a_actor);
  void NotifyListeners(RE::Actor *a_actor) const;

  mutable std::mutex mutex_;
  std::vector<VisualVariantDefinition> persistentDefinitions_;
  std::unordered_map<RE::FormID, std::vector<VisualVariantDefinition>>
      previewDefinitionsByActor_;
  std::unordered_map<SkyrimVanitySystemAPI::ListenerHandle, Listener>
      listeners_;
  SkyrimVanitySystemAPI::ListenerHandle nextListenerHandle_{1};
  std::uint64_t revision_{0};
};
} // namespace sosr::integrations
