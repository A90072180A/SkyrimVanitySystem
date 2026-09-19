#pragma once

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <cstdint>
#include <memory>

namespace SkyrimVanitySystemAPI {
// This must match the SKSE plugin declaration name, not the DLL filename.
inline constexpr auto kPluginName = "Skyrim Vanity System";
inline constexpr std::uint32_t kInterfaceRevision001 = 1;

enum VisualPieceFlags : std::uint32_t {
  kVisualPiece_None = 0,
  kVisualPiece_SlotTriggered = 1u << 0,
  kVisualPiece_Preview = 1u << 1,
  kVisualPiece_Hidden = 1u << 2,
};

enum VisualStateFlags : std::uint32_t {
  kVisualState_None = 0,
  // Revision 001 intentionally reports only replacements authored by SVS.
  // DAVE may apply higher-priority replacements registered by other plugins.
  kVisualState_SvsContributionsOnly = 1u << 0,
  kVisualState_PlayerOnly = 1u << 1,
  // DAVE interface 001 does not expose a refresh-completed callback. A
  // snapshot is a live resolution, not proof that 3D attachment has finished.
  kVisualState_RefreshCompletionUnavailable = 1u << 2,
  kVisualState_PreviewActive = 1u << 3,
  // Conditions and worn forms were evaluated when this snapshot was built.
  kVisualState_LiveEvaluation = 1u << 4,
};

struct VisualPiece001 {
  std::uint32_t structureSize{sizeof(VisualPiece001)};
  std::uint32_t flags{kVisualPiece_None};

  RE::Actor *actor{nullptr};
  RE::TESObjectARMO *sourceArmor{nullptr};
  RE::TESObjectARMA *sourceArmorAddon{nullptr};
  RE::TESObjectARMO *replacementArmor{nullptr};
  RE::TESObjectARMA *replacementArmorAddon{nullptr};

  RE::FormID actorFormID{0};
  RE::FormID sourceArmorFormID{0};
  RE::FormID sourceArmorAddonFormID{0};
  RE::FormID replacementArmorFormID{0};
  RE::FormID replacementArmorAddonFormID{0};

  // The row/slot which caused the replacement, the source ARMA slots, and
  // the slots on the resolved replacement ARMA respectively.
  std::uint64_t triggerSlotMask{0};
  std::uint64_t sourceSlotMask{0};
  std::uint64_t visualSlotMask{0};
  std::int32_t variantPriority{0};

  // Strings are UTF-8 and valid only for the duration of the callback.
  const char *variantId{nullptr};
  const char *maleModelPath{nullptr};
  const char *femaleModelPath{nullptr};
  const char *actorModelPath{nullptr};
};

struct VisualState001 {
  std::uint32_t interfaceRevision{kInterfaceRevision001};
  std::uint32_t structureSize{sizeof(VisualState001)};
  std::uint32_t pieceStructureSize{sizeof(VisualPiece001)};
  std::uint32_t flags{kVisualState_None};
  RE::Actor *actor{nullptr};
  RE::FormID actorFormID{0};
  std::uint64_t revision{0};
  const VisualPiece001 *pieces{nullptr};
  std::uint32_t pieceCount{0};
  std::uint32_t reserved{0};
};

using VisualStateVisitor001 = void (*)(const VisualState001 *a_state,
                                       void *a_userData);
using VisualStateChangedCallback001 = VisualStateVisitor001;
using ListenerHandle = std::uint64_t;

struct ISkyrimVanitySystemInterface001 {
  virtual bool IsReady() const = 0;

  // Builds a synchronous, live snapshot from the SVS variants successfully
  // committed to DAVE. Revision 001 accepts the player actor only.
  virtual bool VisitActorVisualState(RE::Actor *a_actor,
                                     VisualStateVisitor001 a_visitor,
                                     void *a_userData) const = 0;

  // Notifications cover SVS definition and preview commits. Consumers should
  // also query on any equipment/game-condition events they already observe.
  virtual ListenerHandle
  RegisterVisualStateChangedListener(VisualStateChangedCallback001 a_callback,
                                     void *a_userData) = 0;
  virtual bool
  UnregisterVisualStateChangedListener(ListenerHandle a_listenerHandle) = 0;
};

struct SkyrimVanitySystemMessage {
  // "SVSQ", written explicitly to avoid implementation-defined
  // multi-character literals in the public ABI.
  enum : std::uint32_t { kMessage_QueryInterface = 0x53565351u };

  void *(*GetApiFunction)(unsigned int a_revisionNumber) = nullptr;
};

inline auto GetSkyrimVanitySystemInterface001()
    -> ISkyrimVanitySystemInterface001 * {
  SkyrimVanitySystemMessage message{};
  auto *messaging = SKSE::GetMessagingInterface();
  if (!messaging) {
    return nullptr;
  }

  messaging->Dispatch(SkyrimVanitySystemMessage::kMessage_QueryInterface,
                      std::addressof(message),
                      sizeof(SkyrimVanitySystemMessage), kPluginName);
  return message.GetApiFunction
             ? static_cast<ISkyrimVanitySystemInterface001 *>(
                   message.GetApiFunction(kInterfaceRevision001))
             : nullptr;
}
} // namespace SkyrimVanitySystemAPI
