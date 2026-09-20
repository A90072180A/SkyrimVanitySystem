#include "Plugin.h"
#include "RaceMenuBridge.h"
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
    bool applyMorph{true};
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

struct AttachmentMatch {
    racemenu::AttachmentRecord attachment;
    std::string method;
    GeometryCandidate geometry;
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

std::string StableIdentifier(const RE::FormID a_formID)
{
    const auto* form = RE::TESForm::LookupByID(a_formID);
    if (!form) {
        return {};
    }

    const auto* file = form->GetFile(0);
    if (!file) {
        return {};
    }

    const auto filename = file->GetFilename();
    if (filename.empty()) {
        return {};
    }

    return std::format(
        "{}|{:08X}", filename, form->GetLocalFormID());
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

        if (const auto it = root.find("applyMorph");
            it != root.end() && it->is_boolean()) {
            g_config.applyMorph = it->get<bool>();
        }

        logger::info(
            "Loaded manual config: stockings={} heelProfiles={} "
            "applyMorph={} path='{}'",
            g_config.stockings.size(),
            g_config.heelNoHeelByArmor.size(),
            g_config.applyMorph,
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

std::string NormalizeMeshStem(std::string_view a_path)
{
    std::string value(a_path);
    std::ranges::transform(
        value, value.begin(), [](const unsigned char a_char) {
            return static_cast<char>(std::tolower(a_char));
        });
    std::ranges::replace(value, '/', '\\');

    constexpr auto kMeshesPrefix = "meshes\\"sv;
    if (value.starts_with(kMeshesPrefix)) {
        value.erase(0, kMeshesPrefix.size());
    }

    const auto dot = value.find_last_of('.');
    if (dot != std::string::npos) {
        value.erase(dot);
    }

    if (value.size() >= 2 &&
        value[value.size() - 2] == '_' &&
        (value.back() == '0' || value.back() == '1')) {
        value.resize(value.size() - 2);
    }
    return value;
}

std::optional<AttachmentMatch> FindAttachment(
    const LogicalVisualItem& a_stocking)
{
    const auto attachments = racemenu::GetPlayerAttachments();
    if (attachments.empty()) {
        return std::nullopt;
    }

    // First preference: RaceMenu reports the same replacement ARMA that SVS
    // exposed. Pick the newest attachment when duplicate callbacks exist.
    std::optional<AttachmentMatch> exact;
    for (const auto& geometry : a_stocking.geometries) {
        for (const auto& attachment : attachments) {
            if (geometry.armorAddonFormID == 0 ||
                geometry.armorAddonFormID != attachment.armorAddonFormID ||
                attachment.bodyTriPath.empty()) {
                continue;
            }

            if (!exact.has_value() ||
                attachment.sequence > exact->attachment.sequence) {
                exact = AttachmentMatch{
                    .attachment = attachment,
                    .method = "exact-arma",
                    .geometry = geometry};
            }
        }
    }
    if (exact.has_value()) {
        return exact;
    }

    // DAVE may hand RaceMenu the source ARMA while replacing only the rendered
    // model. In that case correlate the visible model with BODYTRI by path
    // stem. BodySlide commonly uses foo_0/1.nif + foo.tri, so strip weight
    // suffixes before comparison.
    std::optional<AttachmentMatch> pathMatch;
    for (const auto& geometry : a_stocking.geometries) {
        const auto modelStem = NormalizeMeshStem(geometry.modelPath);
        if (modelStem.empty()) {
            continue;
        }

        for (const auto& attachment : attachments) {
            if (attachment.bodyTriPath.empty() ||
                NormalizeMeshStem(attachment.bodyTriPath) != modelStem) {
                continue;
            }

            if (!pathMatch.has_value() ||
                attachment.sequence > pathMatch->attachment.sequence) {
                pathMatch = AttachmentMatch{
                    .attachment = attachment,
                    .method = "bodytri-stem",
                    .geometry = geometry};
            }
        }
    }
    if (pathMatch.has_value()) {
        return pathMatch;
    }

    // DAVE's render replacement path does not necessarily emit RaceMenu's
    // armor-attachment observer callback. Fall back to the actual third-person
    // scenegraph and correlate BODYTRI paths directly against the SVS visual
    // model path. This is the authoritative runtime visual object for morphing.
    const auto sceneNodes = racemenu::ScanPlayerBodyTriNodes();
    std::optional<AttachmentMatch> sceneMatch;
    for (const auto& geometry : a_stocking.geometries) {
        const auto modelStem = NormalizeMeshStem(geometry.modelPath);
        if (modelStem.empty()) {
            continue;
        }

        for (const auto& scene : sceneNodes) {
            if (scene.bodyTriPath.empty() ||
                NormalizeMeshStem(scene.bodyTriPath) != modelStem) {
                continue;
            }

            AttachmentRecord synthetic;
            synthetic.object = scene.object;
            synthetic.bodyTriPath = scene.bodyTriPath;
            sceneMatch = AttachmentMatch{
                .attachment = std::move(synthetic),
                .method = "scene-bodytri-stem",
                .geometry = geometry};

            logger::info(
                "[scene target candidate] node='{}' BODYTRI='{}' model='{}'",
                scene.nodeName,
                scene.bodyTriPath,
                geometry.modelPath);
            return sceneMatch;
        }
    }

    return std::nullopt;
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
            logger::info("  [trigger] mask=0x{:016X}", trigger);
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

void LogRecentAttachments()
{
    const auto attachments = racemenu::GetPlayerAttachments();
    logger::info("[RaceMenu attachment cache] count={}", attachments.size());
    for (const auto& attachment : attachments) {
        logger::info(
            "  [attachment] seq={} ARMO={:08X} ARMA={:08X} "
            "BODYTRI='{}'",
            attachment.sequence,
            attachment.armorFormID,
            attachment.armorAddonFormID,
            attachment.bodyTriPath);
    }

    const auto sceneNodes = racemenu::ScanPlayerBodyTriNodes();
    logger::info("[player scene BODYTRI] count={}", sceneNodes.size());
    for (const auto& scene : sceneNodes) {
        logger::info(
            "  [scene BODYTRI] node='{}' path='{}'",
            scene.nodeName,
            scene.bodyTriPath);
    }
}

void ResolveAndApply(const std::vector<LogicalVisualItem>& a_items)
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

    auto* player = RE::PlayerCharacter::GetSingleton();
    const auto* shoe = footwear.front().first;
    const auto requestedNoHeel = footwear.front().second;

    for (const auto* stocking : stockings) {
        const auto match = FindAttachment(*stocking);
        if (!match.has_value()) {
            logger::warn(
                "[heel plan] stockingARMO={:08X} footwearARMO={:08X} "
                "requestedNoHeel={:.3f} attachmentMatch=none "
                "morphApplied=false",
                stocking->armorFormID,
                shoe->armorFormID,
                requestedNoHeel);
            LogRecentAttachments();
            continue;
        }

        logger::info(
            "[stocking target] stockingARMO={:08X} candidateARMA={:08X} "
            "matchedARMO={:08X} matchedARMA={:08X} method={} seq={} "
            "BODYTRI='{}' model='{}'",
            stocking->armorFormID,
            match->geometry.armorAddonFormID,
            match->attachment.armorFormID,
            match->attachment.armorAddonFormID,
            match->method,
            match->attachment.sequence,
            match->attachment.bodyTriPath,
            match->geometry.modelPath);

        bool applied = false;
        if (g_config.applyMorph && player && match->attachment.object) {
            applied = racemenu::ApplyScopedNoHeel(
                player,
                match->attachment.object.get(),
                requestedNoHeel,
                std::format(
                    "stocking {:08X}/ARMA {:08X}",
                    stocking->armorFormID,
                    match->geometry.armorAddonFormID));
        }

        logger::info(
            "[heel plan] stockingARMO={:08X} footwearARMO={:08X} "
            "requestedNoHeel={:.3f} applyMorph={} morphApplied={}",
            stocking->armorFormID,
            shoe->armorFormID,
            requestedNoHeel,
            g_config.applyMorph,
            applied);
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

    const auto logicalItems = BuildLogicalItems(*a_state);
    LogLogicalItems(logicalItems);
    ResolveAndApply(logicalItems);
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
            [](const VisualState001*, void*) { QueueSnapshot(); },
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
        racemenu::Initialize();
        racemenu::SetAttachmentChangedCallback(QueueSnapshot);
        TryConnect();
        QueueSnapshot();
        break;

    case SKSE::MessagingInterface::kDataLoaded:
    case SKSE::MessagingInterface::kPostLoadGame:
        LoadConfig();
        racemenu::Initialize();
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
        "{} {} loaded (scoped-local-morph POC)",
        VanityUBEHeelAdapterPlugin::NAME,
        VanityUBEHeelAdapterPlugin::VERSION.string());

    return true;
}
