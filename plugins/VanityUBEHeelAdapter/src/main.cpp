#include "Plugin.h"
#include "api/SkyrimVanitySystemAPI.h"

namespace vanity_ube_heel_adapter {
namespace {
using SkyrimVanitySystemAPI::ISkyrimVanitySystemInterface001;
using SkyrimVanitySystemAPI::ListenerHandle;
using SkyrimVanitySystemAPI::VisualPiece001;
using SkyrimVanitySystemAPI::VisualState001;

ISkyrimVanitySystemInterface001* g_svs{nullptr};
ListenerHandle g_listenerHandle{0};
std::atomic_bool g_snapshotQueued{false};
bool g_loggedUnavailable{false};

const char* SafeString(const char* a_value)
{
    return a_value ? a_value : "";
}

void LogSnapshot(const VisualState001* a_state, void*)
{
    if (!a_state) {
        logger::warn("[SVS snapshot] null state");
        return;
    }

    logger::info(
        "[SVS snapshot] revision={} flags=0x{:08X} actor={:08X} pieces={} "
        "interfaceRevision={} stateSize={} pieceSize={}",
        a_state->revision,
        a_state->flags,
        a_state->actorFormID,
        a_state->pieceCount,
        a_state->interfaceRevision,
        a_state->structureSize,
        a_state->pieceStructureSize);

    for (std::uint32_t index = 0; index < a_state->pieceCount; ++index) {
        const VisualPiece001& piece = a_state->pieces[index];
        logger::info(
            "[SVS piece {:02}] flags=0x{:08X} priority={} "
            "variant='{}' actor={:08X} "
            "sourceARMO={:08X} sourceARMA={:08X} "
            "replacementARMO={:08X} replacementARMA={:08X} "
            "triggerSlots=0x{:016X} sourceSlots=0x{:016X} "
            "visualSlots=0x{:016X} model='{}'",
            index,
            piece.flags,
            piece.variantPriority,
            SafeString(piece.variantId),
            piece.actorFormID,
            piece.sourceArmorFormID,
            piece.sourceArmorAddonFormID,
            piece.replacementArmorFormID,
            piece.replacementArmorAddonFormID,
            piece.triggerSlotMask,
            piece.sourceSlotMask,
            piece.visualSlotMask,
            SafeString(piece.actorModelPath));
    }
}

bool TryConnect()
{
    if (!g_svs) {
        g_svs = SkyrimVanitySystemAPI::GetSkyrimVanitySystemInterface001();
    }

    if (!g_svs) {
        if (!g_loggedUnavailable) {
            logger::warn(
                "Skyrim Vanity System visual-state API 001 is not available yet");
            g_loggedUnavailable = true;
        }
        return false;
    }

    g_loggedUnavailable = false;

    if (g_listenerHandle == 0) {
        g_listenerHandle = g_svs->RegisterVisualStateChangedListener(
            [](const VisualState001* a_state, void*) {
                if (a_state) {
                    logger::debug(
                        "SVS visual-state notification: revision={} pieces={} "
                        "flags=0x{:08X}",
                        a_state->revision,
                        a_state->pieceCount,
                        a_state->flags);
                }

                // SVS already queues its notification after requesting a DAVE
                // refresh. Queue one more SKSE task so this POC observes the
                // next game-thread turn rather than consuming the callback
                // synchronously.
                if (!g_snapshotQueued.exchange(true)) {
                    if (auto* tasks = SKSE::GetTaskInterface()) {
                        tasks->AddTask([] {
                            g_snapshotQueued.store(false);
                            if (!TryConnect()) {
                                return;
                            }

                            auto* player = RE::PlayerCharacter::GetSingleton();
                            if (!player) {
                                logger::debug(
                                    "Cannot query SVS visual state: player is "
                                    "not available");
                                return;
                            }

                            if (!g_svs->VisitActorVisualState(
                                    player, LogSnapshot, nullptr)) {
                                logger::warn(
                                    "SVS VisitActorVisualState returned false");
                            }
                        });
                    } else {
                        g_snapshotQueued.store(false);
                    }
                }
            },
            nullptr);

        if (g_listenerHandle == 0) {
            logger::warn("Failed to register SVS visual-state listener");
            return false;
        }

        logger::info(
            "Connected to Skyrim Vanity System visual-state API 001; "
            "listenerHandle={}",
            g_listenerHandle);
    }

    return true;
}

void QueueSnapshot()
{
    if (g_snapshotQueued.exchange(true)) {
        return;
    }

    auto query = [] {
        g_snapshotQueued.store(false);

        if (!TryConnect()) {
            return;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            logger::debug("Cannot query SVS visual state: player is not available");
            return;
        }

        if (!g_svs->VisitActorVisualState(player, LogSnapshot, nullptr)) {
            logger::warn("SVS VisitActorVisualState returned false");
        }
    };

    if (auto* tasks = SKSE::GetTaskInterface()) {
        tasks->AddTask(std::move(query));
    } else {
        query();
    }
}

void HandleSKSEMessage(SKSE::MessagingInterface::Message* a_message)
{
    if (!a_message) {
        return;
    }

    switch (a_message->type) {
    case SKSE::MessagingInterface::kPostPostLoad:
        // SVS installs its query listener during kPostLoad, so kPostPostLoad
        // is the first deterministic point at which the POC asks for API 001.
        TryConnect();
        QueueSnapshot();
        break;

    case SKSE::MessagingInterface::kDataLoaded:
    case SKSE::MessagingInterface::kPostLoadGame:
        TryConnect();
        QueueSnapshot();
        break;

    default:
        break;
    }
}
}  // namespace
}  // namespace vanity_ube_heel_adapter

extern "C" DLLEXPORT bool SKSEAPI
SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
    REL::Module::reset();
    SKSE::Init(a_skse);

    auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging) {
        logger::critical("Failed to acquire SKSE messaging interface");
        return false;
    }

    messaging->RegisterListener(
        "SKSE", vanity_ube_heel_adapter::HandleSKSEMessage);

    logger::info(
        "{} {} loaded (logging-only POC)",
        VanityUBEHeelAdapterPlugin::NAME,
        VanityUBEHeelAdapterPlugin::VERSION.string());

    return true;
}
