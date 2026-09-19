#include "Plugin.h"
#include "api/SkyrimVanitySystemAPI.h"

#include <nlohmann/json.hpp>

namespace vanity_ube_heel_adapter {
namespace {
using SkyrimVanitySystemAPI::ISkyrimVanitySystemInterface001;
using SkyrimVanitySystemAPI::ListenerHandle;
using SkyrimVanitySystemAPI::VisualPiece001;
using SkyrimVanitySystemAPI::VisualState001;

constexpr auto kConfigPath =
    "Data/SKSE/Plugins/VanityUBEHeelAdapter.json"sv;

struct ManualConfig {
    std::unordered_set<RE::FormID> stockings;
    std::unordered_map<RE::FormID, float> heelNoHeelByArmor;
};

struct GeometryCandidate {
    RE::FormID armorAddonFormID{0};
    std::uint64_t visualSlotMask{0};
    std::string modelPath;

    bool operator==(const GeometryCandidate& a_other) const
    {
        return armorAddonFormID == a_other.armorAddonFormID &&
               visualSlotMask == a_other.visualSlotMask &&
               modelPath == a_other.modelPath;
    }
};

struct LogicalVisualItem {
    RE::FormID armorFormID{0};
    std::uint32_t flags{0};
    std::int32_t maxPriority{0};
    std::vector<std::uint64_t> triggerSlotMasks;
    std::vector<std::string> variants;
    std::vector<GeometryCandidate> geometries;
};

ISkyrimVanitySystemInterface001* g_svs{nullptr};
ListenerHandle g_listenerHandle{0};
std::atomic_bool g_snapshotQueued{false};
bool g_loggedUnavailable{false};
ManualConfig g_config{};

const char* SafeString(const char* a_value)
{
    return a_value ? a_value : "";
}

std::string FormatFormID(const RE::FormID a_formID)
{
    return std::format("{:08X}", a_formID);
}

std::string StableIdentifier(const RE::FormID a_formID)
{
    const auto* form = RE::TESForm::LookupByID(a_formID);
    if (!form) {
        return {};
    }

    const auto* file = form->GetFile(0);
    if (!file || !file->GetFilename()) {
        return {};
    }

    return std::format(
        "{}|{:08X}", file->GetFilename(), form->GetLocalFormID());
}

std::optional<RE::FormID> ParseRuntimeFormID(std::string_view a_text)
{
    constexpr auto kPrefix = "runtime:"sv;
    if (a_text.starts_with(kPrefix)) {
        a_text.remove_prefix(kPrefix.size());
    }
    if (a_text.starts_with("0x") || a_text.starts_with("0X")) {
        a_text.remove_prefix(2);
    }
    if (a_text.empty() || a_text.size() > 8) {
        return std::nullopt;
    }

    std::uint32_t value = 0;
    const auto* begin = a_text.data();
    const auto* end = begin + a_text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value, 16);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return static_cast<RE::FormID>(value);
}

bool LoadConfig()
{
    g_config = {};

    std::ifstream stream(std::filesystem::path{kConfigPath});
    if (!stream.is_open()) {
        logger::warn(
            "Manual config not found at {}; classification will be disabled",
            kConfigPath);
        return false;
    }

    try {
        nlohmann::json root;
        stream >> root;

        if (const auto it = root.find("stockings");
            it != root.end() && it->is_array()) {
            for (const auto& entry : *it) {
                if (!entry.is_string()) {
                    continue;
                }
                const auto parsed =
                    ParseRuntimeFormID(entry.get<std::string>());
                if (!parsed.has_value()) {
                    logger::warn(
                        "Ignoring invalid stocking runtime FormID '{}'",
                        entry.get<std::string>());
                    continue;
                }
                g_config.stockings.insert(*parsed);
            }
        }

        if (const auto it = root.find("heels");
            it != root.end() && it->is_object()) {
            for (auto heel = it->begin(); heel != it->end(); ++heel) {
                if (!heel.value().is_number()) {
                    continue;
                }
                const auto parsed = ParseRuntimeFormID(heel.key());
                if (!parsed.has_value()) {
                    logger::warn(
                        "Ignoring invalid heel runtime FormID '{}'",
                        heel.key());
                    continue;
                }

                const auto noHeel =
                    std::clamp(heel.value().get<float>(), 0.0F, 1.0F);
                g_config.heelNoHeelByArmor.insert_or_assign(*parsed, noHeel);
            }
        }

        logger::info(
            "Loaded manual config: stockings={} heelProfiles={} path='{}'",
            g_config.stockings.size(),
            g_config.heelNoHeelByArmor.size(),
            kConfigPath);
        return true;
    } catch (const std::exception& exception) {
        logger::error(
            "Failed to parse {}: {}", kConfigPath, exception.what());
        g_config = {};
        return false;
    }
}

template <class T>
void AppendUnique(std::vector<T>& a_values, T a_value)
{
    if (std::ranges::find(a_values, a_value) == a_values.end()) {
        a_values.push_back(std::move(a_value));
    }
}

std::vector<LogicalVisualItem> BuildLogicalItems(const VisualState001& a_state)
{
    std::vector<LogicalVisualItem> items;
    std::unordered_map<RE::FormID, std::size_t> indexByArmor;

    for (std::uint32_t index = 0; index < a_state.pieceCount; ++index) {
        const auto& piece = a_state.pieces[index];
        if (piece.replacementArmorFormID == 0) {
            continue;
        }

        auto [it, inserted] = indexByArmor.try_emplace(
            piece.replacementArmorFormID, items.size());
        if (inserted) {
            LogicalVisualItem item;
            item.armorFormID = piece.replacementArmorFormID;
            item.maxPriority = piece.variantPriority;
            items.push_back(std::move(item));
        }

        auto& item = items[it->second];
        item.flags |= piece.flags;
        item.maxPriority = (std::max)(item.maxPriority, piece.variantPriority);
        AppendUnique(item.triggerSlotMasks, piece.triggerSlotMask);
        AppendUnique(item.variants, std::string(SafeString(piece.variantId)));

        GeometryCandidate geometry{
            .armorAddonFormID = piece.replacementArmorAddonFormID,
            .visualSlotMask = piece.visualSlotMask,
            .modelPath = SafeString(piece.actorModelPath)};
        AppendUnique(item.geometries, std::move(geometry));
    }

    std::ranges::sort(items, [](const auto& a_left, const auto& a_right) {
        if (a_left.maxPriority != a_right.maxPriority) {
            return a_left.maxPriority > a_right.maxPriority;
        }
        return a_left.armorFormID < a_right.armorFormID;
    });
    return items;
}

void LogLogicalItems(const std::vector<LogicalVisualItem>& a_items)
{
    logger::info("[logical items] count={}", a_items.size());

    for (std::size_t index = 0; index < a_items.size(); ++index) {
        const auto& item = a_items[index];
        logger::info(
            "[logical item {:02}] ARMO={:08X} stable='{}' flags=0x{:08X} "
            "priority={} variants={} triggers={} geometries={}",
            index,
            item.armorFormID,
            StableIdentifier(item.armorFormID),
            item.flags,
            item.maxPriority,
            item.variants.size(),
            item.triggerSlotMasks.size(),
            item.geometries.size());

        for (const auto trigger : item.triggerSlotMasks) {
            logger::info(
                "  [trigger] mask=0x{:016X}", trigger);
        }

        for (const auto& geometry : item.geometries) {
            logger::info(
                "  [geometry] ARMA={:08X} stable='{}' "
                "visualSlots=0x{:016X} model='{}'",
                geometry.armorAddonFormID,
                StableIdentifier(geometry.armorAddonFormID),
                geometry.visualSlotMask,
                geometry.modelPath);
        }
    }
}

void LogManualResolution(const std::vector<LogicalVisualItem>& a_items)
{
    std::vector<const LogicalVisualItem*> stockings;
    std::vector<std::pair<const LogicalVisualItem*, float>> footwear;

    for (const auto& item : a_items) {
        if (g_config.stockings.contains(item.armorFormID)) {
            stockings.push_back(std::addressof(item));
            logger::info(
                "[manual stocking] ARMO={:08X} stable='{}' geometries={}",
                item.armorFormID,
                StableIdentifier(item.armorFormID),
                item.geometries.size());
            for (const auto& geometry : item.geometries) {
                logger::info(
                    "  [stocking geometry candidate] ARMA={:08X} "
                    "visualSlots=0x{:016X} model='{}'",
                    geometry.armorAddonFormID,
                    geometry.visualSlotMask,
                    geometry.modelPath);
            }
        }

        if (const auto heel =
                g_config.heelNoHeelByArmor.find(item.armorFormID);
            heel != g_config.heelNoHeelByArmor.end()) {
            footwear.emplace_back(std::addressof(item), heel->second);
            logger::info(
                "[manual footwear] ARMO={:08X} stable='{}' "
                "requestedNoHeel={:.3f} geometries={}",
                item.armorFormID,
                StableIdentifier(item.armorFormID),
                heel->second,
                item.geometries.size());
        }
    }

    if (stockings.empty()) {
        logger::info("[heel plan] no configured stocking is currently visible");
        return;
    }
    if (footwear.empty()) {
        logger::info("[heel plan] no configured footwear is currently visible");
        return;
    }
    if (footwear.size() > 1) {
        logger::warn(
            "[heel plan] ambiguous: {} configured footwear items are visible; "
            "no single target is resolved",
            footwear.size());
        return;
    }

    const auto* shoe = footwear.front().first;
    const auto requestedNoHeel = footwear.front().second;
    for (const auto* stocking : stockings) {
        logger::info(
            "[heel plan] stockingARMO={:08X} footwearARMO={:08X} "
            "requestedNoHeel={:.3f} morphApplied=false",
            stocking->armorFormID,
            shoe->armorFormID,
            requestedNoHeel);
    }
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

    const auto logicalItems = BuildLogicalItems(*a_state);
    LogLogicalItems(logicalItems);
    LogManualResolution(logicalItems);
}

bool TryConnect();

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
            logger::debug(
                "Cannot query SVS visual state: player is not available");
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
                // refresh. Queue one more SKSE task so the adapter consumes a
                // live snapshot on the next game-thread turn.
                QueueSnapshot();
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

void HandleSKSEMessage(SKSE::MessagingInterface::Message* a_message)
{
    if (!a_message) {
        return;
    }

    switch (a_message->type) {
    case SKSE::MessagingInterface::kPostPostLoad:
        LoadConfig();
        TryConnect();
        QueueSnapshot();
        break;

    case SKSE::MessagingInterface::kDataLoaded:
    case SKSE::MessagingInterface::kPostLoadGame:
        LoadConfig();
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
        "{} {} loaded (manual-classification logging POC)",
        VanityUBEHeelAdapterPlugin::NAME,
        VanityUBEHeelAdapterPlugin::VERSION.string());

    return true;
}
