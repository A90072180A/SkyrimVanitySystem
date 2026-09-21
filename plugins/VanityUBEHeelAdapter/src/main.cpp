#include "Plugin.h"
#include "BodySlidePostureResolver.h"
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

struct StockingMorphProfile {
    std::optional<RE::FormID> armorFormID;
    std::string modelContains;
    std::string morph{"NoHeel"};
    float heelValue{0.0F};
    float flatValue{1.0F};
};

struct ManualConfig {
    std::unordered_set<RE::FormID> stockings;
    std::unordered_map<RE::FormID, float> heelNoHeelByArmor;
    std::vector<StockingMorphProfile> stockingMorphProfiles;
    bool applyMorph{true};
    bool autoDetectStockings{true};
    bool autoPostureFromBodySlide{true};
    bool allowSliderSetDefaultPosture{false};
    bool logBodySlideCandidates{true};
    bool debugDiagnostics{false};
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
std::atomic_bool g_equipReapplyQueued{false};
bool g_loggedUnavailable{false};
bool g_equipSinkRegistered{false};
ManualConfig g_config{};
std::unordered_map<std::string, bool> g_noHeelCapabilityCache;

void QueueSnapshot();

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

std::optional<RE::FormID> ResolveFormIdentifier(std::string_view a_text)
{
    constexpr auto kRuntimePrefix = "runtime:"sv;
    if (a_text.starts_with(kRuntimePrefix) ||
        a_text.starts_with("0x") || a_text.starts_with("0X")) {
        return ParseRuntimeFormID(a_text);
    }

    const auto separator = a_text.rfind('|');
    if (separator == std::string_view::npos ||
        separator == 0 || separator + 1 >= a_text.size()) {
        return ParseRuntimeFormID(a_text);
    }

    const auto plugin = a_text.substr(0, separator);
    auto localText = a_text.substr(separator + 1);
    if (localText.starts_with("0x") || localText.starts_with("0X")) {
        localText.remove_prefix(2);
    }
    if (localText.empty() || localText.size() > 8) {
        return std::nullopt;
    }

    RE::FormID localID = 0;
    const auto [ptr, ec] = std::from_chars(
        localText.data(),
        localText.data() + localText.size(),
        localID,
        16);
    if (ec != std::errc{} ||
        ptr != localText.data() + localText.size()) {
        return std::nullopt;
    }

    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) {
        return std::nullopt;
    }

    auto* form = dataHandler->LookupForm(localID, plugin);
    return form ? std::optional<RE::FormID>(form->GetFormID())
                : std::nullopt;
}

std::string LowerCopy(std::string_view a_value)
{
    std::string result(a_value);
    std::ranges::transform(
        result, result.begin(), [](const unsigned char a_char) {
            return static_cast<char>(std::tolower(a_char));
        });
    return result;
}

bool ContainsAnyToken(
    std::string_view a_text,
    std::initializer_list<std::string_view> a_tokens)
{
    const auto lower = LowerCopy(a_text);
    for (const auto token : a_tokens) {
        if (lower.find(token) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::filesystem::path BodyTriDiskPath(std::string_view a_bodyTriPath)
{
    std::string relative(a_bodyTriPath);
    std::ranges::replace(relative, '/', '\\');
    while (!relative.empty() &&
           (relative.front() == '\\' || relative.front() == '/')) {
        relative.erase(relative.begin());
    }
    return std::filesystem::path{"Data"} /
           "Meshes" /
           std::filesystem::path{relative};
}

bool FileContainsAsciiToken(
    const std::filesystem::path& a_path,
    std::string_view a_token)
{
    std::ifstream stream(a_path, std::ios::binary);
    if (!stream.is_open()) {
        return false;
    }

    std::vector<char> bytes(
        (std::istreambuf_iterator<char>(stream)),
        std::istreambuf_iterator<char>());
    if (bytes.empty() || a_token.empty()) {
        return false;
    }

    return std::search(
               bytes.begin(),
               bytes.end(),
               a_token.begin(),
               a_token.end()) != bytes.end();
}

bool BodyTriSupportsNoHeel(std::string_view a_bodyTriPath)
{
    if (a_bodyTriPath.empty()) {
        return false;
    }

    const std::string key(a_bodyTriPath);
    if (const auto it = g_noHeelCapabilityCache.find(key);
        it != g_noHeelCapabilityCache.end()) {
        return it->second;
    }

    const auto diskPath = BodyTriDiskPath(a_bodyTriPath);
    const bool supported = FileContainsAsciiToken(diskPath, "NoHeel");
    g_noHeelCapabilityCache.emplace(key, supported);

    logger::info(
        "[morph capability] BODYTRI='{}' NoHeel={} path='{}'",
        a_bodyTriPath,
        supported,
        diskPath.string());
    return supported;
}

bool LoadConfig()
{
    g_config = {};
    g_noHeelCapabilityCache.clear();

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
                    ResolveFormIdentifier(entry.get<std::string>());
                if (!parsed.has_value()) {
                    logger::warn(
                        "Ignoring unresolved stocking identifier '{}'",
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
                const auto parsed = ResolveFormIdentifier(heel.key());
                if (!parsed.has_value()) {
                    logger::warn(
                        "Ignoring unresolved heel identifier '{}'",
                        heel.key());
                    continue;
                }

                const auto noHeel =
                    std::clamp(heel.value().get<float>(), 0.0F, 1.0F);
                g_config.heelNoHeelByArmor.insert_or_assign(*parsed, noHeel);
            }
        }

        if (const auto it = root.find("stockingMorphProfiles");
            it != root.end() && it->is_array()) {
            for (const auto& entry : *it) {
                if (!entry.is_object()) {
                    continue;
                }

                StockingMorphProfile profile;
                if (const auto armorIt = entry.find("armor");
                    armorIt != entry.end() && armorIt->is_string()) {
                    const auto resolved =
                        ResolveFormIdentifier(armorIt->get<std::string>());
                    if (resolved.has_value()) {
                        profile.armorFormID = *resolved;
                    } else {
                        logger::warn(
                            "Ignoring unresolved stocking morph profile armor '{}'",
                            armorIt->get<std::string>());
                    }
                }
                if (const auto modelIt = entry.find("modelContains");
                    modelIt != entry.end() && modelIt->is_string()) {
                    profile.modelContains = modelIt->get<std::string>();
                }
                if (const auto morphIt = entry.find("morph");
                    morphIt != entry.end() && morphIt->is_string()) {
                    profile.morph = morphIt->get<std::string>();
                }
                if (const auto heelIt = entry.find("heelValue");
                    heelIt != entry.end() && heelIt->is_number()) {
                    profile.heelValue = heelIt->get<float>();
                }
                if (const auto flatIt = entry.find("flatValue");
                    flatIt != entry.end() && flatIt->is_number()) {
                    profile.flatValue = flatIt->get<float>();
                }

                if (profile.morph.empty() ||
                    (!profile.armorFormID.has_value() &&
                     profile.modelContains.empty())) {
                    logger::warn(
                        "Ignoring invalid stocking morph profile; require morph "
                        "and armor or modelContains");
                    continue;
                }

                g_config.stockingMorphProfiles.push_back(std::move(profile));
            }
        }

        if (const auto it = root.find("applyMorph");
            it != root.end() && it->is_boolean()) {
            g_config.applyMorph = it->get<bool>();
        }
        if (const auto it = root.find("autoDetectStockings");
            it != root.end() && it->is_boolean()) {
            g_config.autoDetectStockings = it->get<bool>();
        }
        if (const auto it = root.find("autoPostureFromBodySlide");
            it != root.end() && it->is_boolean()) {
            g_config.autoPostureFromBodySlide = it->get<bool>();
        }
        if (const auto it = root.find("allowSliderSetDefaultPosture");
            it != root.end() && it->is_boolean()) {
            g_config.allowSliderSetDefaultPosture = it->get<bool>();
        }
        if (const auto it = root.find("logBodySlideCandidates");
            it != root.end() && it->is_boolean()) {
            g_config.logBodySlideCandidates = it->get<bool>();
        }
        if (const auto it = root.find("debugDiagnostics");
            it != root.end() && it->is_boolean()) {
            g_config.debugDiagnostics = it->get<bool>();
        }

        logger::info(
            "Loaded config: stockings={} heelProfiles={} morphProfiles={} "
            "applyMorph={} autoDetectStockings={} autoPostureFromBodySlide={} "
            "allowSliderSetDefaultPosture={} logBodySlideCandidates={} "
            "debugDiagnostics={} path='{}'",
            g_config.stockings.size(),
            g_config.heelNoHeelByArmor.size(),
            g_config.stockingMorphProfiles.size(),
            g_config.applyMorph,
            g_config.autoDetectStockings,
            g_config.autoPostureFromBodySlide,
            g_config.allowSliderSetDefaultPosture,
            g_config.logBodySlideCandidates,
            g_config.debugDiagnostics,
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

int StockingConfidence(const LogicalVisualItem& a_item)
{
    constexpr std::uint64_t kSlot38 = 1ULL << (38 - 30);
    constexpr std::uint64_t kSlot48 = 1ULL << (48 - 30);
    constexpr std::uint64_t kSlot53 = 1ULL << (53 - 30);
    constexpr std::uint64_t kHosierySlots = kSlot38 | kSlot48 | kSlot53;

    int score = 0;
    bool hasLegwearToken = false;
    bool hasShoeToken = false;
    for (const auto& geometry : a_item.geometries) {
        if ((geometry.visualSlotMask & kHosierySlots) != 0) {
            score += 3;
        }
        hasLegwearToken = hasLegwearToken || ContainsAnyToken(
            geometry.modelPath,
            {"stocking", "pantyhose", "tights", "hosiery",
             "thighhigh", "thigh_high", "thigh high"});
        hasShoeToken = hasShoeToken || ContainsAnyToken(
            geometry.modelPath,
            {"shoe", "boot", "heel", "sandal", "sneaker", "converse"});
    }

    if (hasLegwearToken) {
        score += 4;
    }
    if (hasShoeToken) {
        score -= 6;
    }
    return score;
}

bool MatchesStockingMorphProfile(const LogicalVisualItem& a_item)
{
    for (const auto& profile : g_config.stockingMorphProfiles) {
        if (profile.armorFormID.has_value() &&
            *profile.armorFormID == a_item.armorFormID) {
            return true;
        }

        if (profile.modelContains.empty()) {
            continue;
        }

        const auto token = LowerCopy(profile.modelContains);
        for (const auto& geometry : a_item.geometries) {
            if (LowerCopy(geometry.modelPath).find(token) !=
                std::string::npos) {
                return true;
            }
        }
    }
    return false;
}

bool IsConfiguredOrAutoStocking(
    const LogicalVisualItem& a_item,
    bool& a_explicit,
    bool& a_profileMatched,
    int& a_confidence)
{
    a_explicit = g_config.stockings.contains(a_item.armorFormID);
    a_profileMatched = MatchesStockingMorphProfile(a_item);
    a_confidence = StockingConfidence(a_item);

    // A custom morph profile is only an override for an already identified
    // stocking; it is not itself proof that the visual is hosiery.
    return a_explicit ||
           (g_config.autoDetectStockings && a_confidence >= 5);
}

struct ResolvedMorphProfile {
    std::string morph{"NoHeel"};
    float heelValue{0.0F};
    float flatValue{1.0F};
    std::string source{"default"};
};

ResolvedMorphProfile ResolveMorphProfile(
    const LogicalVisualItem& a_stocking,
    const GeometryCandidate& a_geometry)
{
    for (const auto& profile : g_config.stockingMorphProfiles) {
        if (profile.armorFormID.has_value() &&
            *profile.armorFormID == a_stocking.armorFormID) {
            return ResolvedMorphProfile{
                .morph = profile.morph,
                .heelValue = profile.heelValue,
                .flatValue = profile.flatValue,
                .source = "armor"};
        }
    }

    const auto lowerModel = LowerCopy(a_geometry.modelPath);
    for (const auto& profile : g_config.stockingMorphProfiles) {
        if (profile.modelContains.empty()) {
            continue;
        }
        const auto token = LowerCopy(profile.modelContains);
        if (!token.empty() &&
            lowerModel.find(token) != std::string::npos) {
            return ResolvedMorphProfile{
                .morph = profile.morph,
                .heelValue = profile.heelValue,
                .flatValue = profile.flatValue,
                .source = "model"};
        }
    }

    return {};
}

float MapPostureToMorph(
    const float a_posture,
    const ResolvedMorphProfile& a_profile)
{
    const auto posture = std::clamp(a_posture, 0.0F, 1.0F);
    return a_profile.heelValue +
           posture * (a_profile.flatValue - a_profile.heelValue);
}

std::optional<AttachmentMatch> FindAttachment(
    const LogicalVisualItem& a_stocking)
{
    const auto attachments = racemenu::GetPlayerAttachments();

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

    // DAVE visual replacements can bypass RaceMenu's attachment observer and
    // can also live under BipedAnim partClone trees that are not discoverable
    // through a generic actor-root BODYTRI walk. Inspect both objects and
    // bufferedObjects, then correlate the replacement model path to BODYTRI.
    const auto bipedParts = racemenu::ScanPlayerBipedParts();
    std::optional<AttachmentMatch> bipedMatch;
    int bestBipedScore = -1;
    for (const auto& geometry : a_stocking.geometries) {
        const auto modelStem = NormalizeMeshStem(geometry.modelPath);
        if (modelStem.empty()) {
            continue;
        }

        for (const auto& part : bipedParts) {
            if (!part.partClone) {
                continue;
            }

            for (const auto& bodyTri : part.bodyTriPaths) {
                if (NormalizeMeshStem(bodyTri) != modelStem) {
                    continue;
                }

                const std::uint64_t slotBit =
                    part.slotIndex < 64 ? (1ULL << part.slotIndex) : 0ULL;
                bool triggerMatch = false;
                for (const auto trigger : a_stocking.triggerSlotMasks) {
                    if ((trigger & slotBit) != 0) {
                        triggerMatch = true;
                        break;
                    }
                }

                // Prefer an active (non-buffered) partClone in the same trigger
                // slot, but still accept a BODYTRI-exact match elsewhere.
                const int score =
                    (triggerMatch ? 2 : 0) + (!part.buffered ? 1 : 0);
                if (score > bestBipedScore) {
                    racemenu::AttachmentRecord synthetic;
                    synthetic.armorFormID = part.itemFormID;
                    synthetic.armorAddonFormID = part.addonFormID;
                    synthetic.object = part.partClone;
                    synthetic.bodyTriPath = bodyTri;

                    bipedMatch = AttachmentMatch{
                        .attachment = std::move(synthetic),
                        .method = triggerMatch
                            ? "biped-trigger-bodytri-stem"
                            : "biped-bodytri-stem",
                        .geometry = geometry};
                    bestBipedScore = score;
                }
            }
        }
    }
    if (bipedMatch.has_value()) {
        logger::info(
            "[biped target candidate] method={} matchedARMO={:08X} "
            "matchedARMA={:08X} BODYTRI='{}' model='{}'",
            bipedMatch->method,
            bipedMatch->attachment.armorFormID,
            bipedMatch->attachment.armorAddonFormID,
            bipedMatch->attachment.bodyTriPath,
            bipedMatch->geometry.modelPath);
        return bipedMatch;
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

            racemenu::AttachmentRecord synthetic;
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
    if (!g_config.debugDiagnostics) {
        return;
    }
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

    const auto bipedParts = racemenu::ScanPlayerBipedParts();
    logger::info("[player biped parts] count={}", bipedParts.size());
    for (const auto& part : bipedParts) {
        logger::info(
            "  [biped part] slot={} index={} buffered={} item={:08X} "
            "addon={:08X} clone={} root='{}' BODYTRIs={} geometries={}",
            part.slotNumber,
            part.slotIndex,
            part.buffered,
            part.itemFormID,
            part.addonFormID,
            static_cast<bool>(part.partClone),
            part.rootName,
            part.bodyTriPaths.size(),
            part.geometryNames.size());

        for (const auto& bodyTri : part.bodyTriPaths) {
            logger::info("    [biped BODYTRI] '{}'", bodyTri);
        }
        for (const auto& geometryName : part.geometryNames) {
            logger::info("    [biped geometry] '{}'", geometryName);
        }
    }
}

bool IsFootwearVisual(const LogicalVisualItem& a_item)
{
    constexpr std::uint64_t kFeetSlot = 1ULL << (37 - 30);

    bool slot37 = false;
    bool footwearToken = false;
    for (const auto& geometry : a_item.geometries) {
        slot37 = slot37 || ((geometry.visualSlotMask & kFeetSlot) != 0);
        footwearToken = footwearToken || ContainsAnyToken(
            geometry.modelPath,
            {"shoe", "boot", "heel", "sandal", "sneaker",
             "converse", "pump", "loafer", "footwear"});
    }
    return slot37 || footwearToken;
}

float PlayerWeight()
{
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* base = player ? player->GetActorBase() : nullptr;
    return base ? std::clamp(base->GetWeight(), 0.0F, 100.0F) : 50.0F;
}

struct FootwearResolution {
    const LogicalVisualItem* item{nullptr};
    float posture{0.0F};
    std::string source;
    int confidence{0};
};

std::optional<bodyslide::PostureCandidate> ResolveBodySlideCandidate(
    const LogicalVisualItem& a_item)
{
    if (!g_config.autoPostureFromBodySlide &&
        !g_config.logBodySlideCandidates) {
        return std::nullopt;
    }
    if (!IsFootwearVisual(a_item)) {
        return std::nullopt;
    }

    std::vector<std::string> models;
    models.reserve(a_item.geometries.size());
    for (const auto& geometry : a_item.geometries) {
        if (!geometry.modelPath.empty()) {
            models.push_back(geometry.modelPath);
        }
    }

    if (models.empty()) {
        return std::nullopt;
    }

    return bodyslide::ResolvePosture(
        models,
        PlayerWeight(),
        g_config.logBodySlideCandidates ||
            g_config.debugDiagnostics);
}

void ResolveAndApply(const std::vector<LogicalVisualItem>& a_items)
{
    std::vector<const LogicalVisualItem*> stockings;
    std::vector<FootwearResolution> footwear;
    bool hasManualFootwear = false;

    for (const auto& item : a_items) {
        bool explicitStocking = false;
        bool profileStocking = false;
        int stockingConfidence = 0;
        if (IsConfiguredOrAutoStocking(
                item,
                explicitStocking,
                profileStocking,
                stockingConfidence)) {
            stockings.push_back(std::addressof(item));
            if (g_config.debugDiagnostics && profileStocking) {
                logger::info(
                    "[stocking profile override available] ARMO={:08X}",
                    item.armorFormID);
            }
            logger::info(
                "[stocking detected] ARMO={:08X} stable='{}' source={} "
                "confidence={} geometries={}",
                item.armorFormID,
                StableIdentifier(item.armorFormID),
                explicitStocking ? "config" : "auto",
                stockingConfidence,
                item.geometries.size());
            if (g_config.debugDiagnostics) {
                for (const auto& geometry : item.geometries) {
                    logger::info(
                        "  [stocking geometry candidate] ARMA={:08X} "
                        "visualSlots=0x{:016X} model='{}'",
                        geometry.armorAddonFormID,
                        geometry.visualSlotMask,
                        geometry.modelPath);
                }
            }
        }

        const auto manual =
            g_config.heelNoHeelByArmor.find(item.armorFormID);
        const bool hasManual =
            manual != g_config.heelNoHeelByArmor.end();

        const auto autoCandidate = ResolveBodySlideCandidate(item);
        if (autoCandidate.has_value() &&
            g_config.logBodySlideCandidates) {
            logger::info(
                "[footwear auto candidate] ARMO={:08X} stable='{}' "
                "source={} set='{}' posture={:.3f} small={:.1f} big={:.1f} "
                "confidence={} manualOverride={}",
                item.armorFormID,
                StableIdentifier(item.armorFormID),
                autoCandidate->source,
                autoCandidate->setName,
                autoCandidate->posture,
                autoCandidate->lowWeightValue,
                autoCandidate->highWeightValue,
                autoCandidate->highConfidence ? "high" : "low",
                hasManual);
        }

        if (hasManual) {
            hasManualFootwear = true;
            footwear.push_back(FootwearResolution{
                .item = std::addressof(item),
                .posture = manual->second,
                .source = "manual",
                .confidence = 100});
            logger::info(
                "[footwear profile] ARMO={:08X} stable='{}' source=manual "
                "requestedPosture={:.3f} geometries={}",
                item.armorFormID,
                StableIdentifier(item.armorFormID),
                manual->second,
                item.geometries.size());
            continue;
        }

        if (!g_config.autoPostureFromBodySlide ||
            !autoCandidate.has_value()) {
            continue;
        }

        const bool accepted =
            autoCandidate->highConfidence ||
            g_config.allowSliderSetDefaultPosture;
        if (!accepted) {
            logger::info(
                "[footwear auto candidate] ARMO={:08X} rejected: "
                "SliderSet default posture is low-confidence",
                item.armorFormID);
            continue;
        }

        footwear.push_back(FootwearResolution{
            .item = std::addressof(item),
            .posture = autoCandidate->posture,
            .source = autoCandidate->source,
            .confidence = autoCandidate->highConfidence ? 80 : 40});
        logger::info(
            "[footwear profile] ARMO={:08X} stable='{}' source={} "
            "requestedPosture={:.3f} set='{}'",
            item.armorFormID,
            StableIdentifier(item.armorFormID),
            autoCandidate->source,
            autoCandidate->posture,
            autoCandidate->setName);
    }

    if (hasManualFootwear) {
        std::erase_if(
            footwear,
            [](const FootwearResolution& a_resolution) {
                return a_resolution.source != "manual";
            });
    }

    if (stockings.empty()) {
        logger::info("[heel plan] no stocking candidate is currently visible");
        return;
    }
    if (footwear.empty()) {
        logger::info(
            "[heel plan] no resolved footwear posture is currently visible");
        return;
    }

    std::ranges::sort(
        footwear,
        [](const FootwearResolution& a_left,
           const FootwearResolution& a_right) {
            if (a_left.confidence != a_right.confidence) {
                return a_left.confidence > a_right.confidence;
            }
            return a_left.item->maxPriority > a_right.item->maxPriority;
        });

    const auto bestConfidence = footwear.front().confidence;
    const auto bestPosture = footwear.front().posture;
    for (std::size_t index = 1; index < footwear.size(); ++index) {
        if (footwear[index].confidence != bestConfidence) {
            break;
        }
        if (std::abs(footwear[index].posture - bestPosture) > 0.02F) {
            logger::warn(
                "[heel plan] ambiguous footwear posture: ARMO={:08X} {:.3f} "
                "vs ARMO={:08X} {:.3f}",
                footwear.front().item->armorFormID,
                bestPosture,
                footwear[index].item->armorFormID,
                footwear[index].posture);
            return;
        }
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    const auto* shoe = footwear.front().item;
    const auto requestedPosture = footwear.front().posture;
    logger::info(
        "[heel plan] resolved footwear ARMO={:08X} source={} "
        "posture={:.3f} confidence={}",
        shoe->armorFormID,
        footwear.front().source,
        requestedPosture,
        footwear.front().confidence);

    for (const auto* stocking : stockings) {
        const auto match = FindAttachment(*stocking);
        if (!match.has_value()) {
            logger::warn(
                "[heel plan] stockingARMO={:08X} footwearARMO={:08X} "
                "requestedPosture={:.3f} attachmentMatch=none "
                "morphApplied=false",
                stocking->armorFormID,
                shoe->armorFormID,
                requestedPosture);
            if (g_config.debugDiagnostics) {
                LogRecentAttachments();
            }
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

        const auto morphProfile =
            ResolveMorphProfile(*stocking, match->geometry);

        const bool explicitCustomMorph =
            morphProfile.source != "default";
        const bool hasValidatedNoHeel =
            morphProfile.morph == "NoHeel" &&
            BodyTriSupportsNoHeel(match->attachment.bodyTriPath);

        if (!explicitCustomMorph && !hasValidatedNoHeel) {
            logger::info(
                "[heel plan] stockingARMO={:08X} BODYTRI='{}' "
                "NoHeel=false; skipping non-morphable stocking",
                stocking->armorFormID,
                match->attachment.bodyTriPath);
            continue;
        }

        const auto targetMorphValue =
            MapPostureToMorph(requestedPosture, morphProfile);

        logger::info(
            "[morph profile] stockingARMO={:08X} source={} morph='{}' "
            "heelValue={:.3f} flatValue={:.3f} requestedPosture={:.3f} "
            "targetMorphValue={:.3f}",
            stocking->armorFormID,
            morphProfile.source,
            morphProfile.morph,
            morphProfile.heelValue,
            morphProfile.flatValue,
            requestedPosture,
            targetMorphValue);

        bool applied = false;
        if (g_config.applyMorph && player && match->attachment.object) {
            applied = racemenu::ApplyScopedMorph(
                player,
                match->attachment.object.get(),
                morphProfile.morph,
                targetMorphValue,
                std::format(
                    "stocking {:08X}/ARMA {:08X}",
                    stocking->armorFormID,
                    match->geometry.armorAddonFormID));
        }

        logger::info(
            "[heel plan] stockingARMO={:08X} footwearARMO={:08X} "
            "requestedPosture={:.3f} morph='{}' targetMorphValue={:.3f} "
            "applyMorph={} morphApplied={}",
            stocking->armorFormID,
            shoe->armorFormID,
            requestedPosture,
            morphProfile.morph,
            targetMorphValue,
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

void OnBodySlideIndexReady()
{
    logger::info(
        "[bodyslide] index-ready callback; queueing SVS reevaluation");
    QueueSnapshot();
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

void QueueSnapshotAfterEquipmentChange(
    const RE::FormID a_baseObject,
    const bool a_equipped)
{
    if (g_equipReapplyQueued.exchange(true)) {
        return;
    }

    logger::info(
        "[equip reapply] queued baseObject={:08X} equipped={}",
        a_baseObject,
        a_equipped);

    auto finalPass = [] {
        g_equipReapplyQueued.store(false);
        logger::info("[equip reapply] executing deferred SVS snapshot");
        QueueSnapshot();
    };

    auto* tasks = SKSE::GetTaskInterface();
    if (!tasks) {
        finalPass();
        return;
    }

    // Equipment changes can rebuild the player's 3D and RaceMenu may reapply
    // actor-wide morphs after the equip event. Two task hops let that rebuild
    // settle before the adapter reapplies the scoped stocking morph.
    tasks->AddTask([finalPass] {
        if (auto* nextTasks = SKSE::GetTaskInterface()) {
            nextTasks->AddTask(finalPass);
        } else {
            finalPass();
        }
    });
}

class PlayerEquipEventSink final :
    public RE::BSTEventSink<RE::TESEquipEvent>
{
public:
    static PlayerEquipEventSink* GetSingleton()
    {
        static PlayerEquipEventSink instance;
        return std::addressof(instance);
    }

    RE::BSEventNotifyControl ProcessEvent(
        const RE::TESEquipEvent* a_event,
        RE::BSTEventSource<RE::TESEquipEvent>*) override
    {
        if (!a_event) {
            return RE::BSEventNotifyControl::kContinue;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || a_event->actor.get() != player) {
            return RE::BSEventNotifyControl::kContinue;
        }

        QueueSnapshotAfterEquipmentChange(
            a_event->baseObject,
            a_event->equipped);
        return RE::BSEventNotifyControl::kContinue;
    }
};

void RegisterEquipEventSink()
{
    if (g_equipSinkRegistered) {
        return;
    }

    auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
    auto* source =
        holder ? holder->GetEventSource<RE::TESEquipEvent>() : nullptr;
    if (!source) {
        logger::warn("[equip reapply] TESEquipEvent source unavailable");
        return;
    }

    source->AddEventSink(PlayerEquipEventSink::GetSingleton());
    g_equipSinkRegistered = true;
    logger::info("[equip reapply] registered player TESEquipEvent sink");
}

void HandleSKSEMessage(SKSE::MessagingInterface::Message* a_message)
{
    if (!a_message) {
        return;
    }

    switch (a_message->type) {
    case SKSE::MessagingInterface::kPostPostLoad:
        racemenu::Initialize();
        racemenu::SetAttachmentChangedCallback(QueueSnapshot);
        TryConnect();
        QueueSnapshot();
        break;

    case SKSE::MessagingInterface::kDataLoaded:
        LoadConfig();
        bodyslide::Reset();
        bodyslide::PrepareAsync(
            g_config.debugDiagnostics,
            OnBodySlideIndexReady);
        racemenu::Initialize();
        RegisterEquipEventSink();
        TryConnect();
        QueueSnapshot();
        break;

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
