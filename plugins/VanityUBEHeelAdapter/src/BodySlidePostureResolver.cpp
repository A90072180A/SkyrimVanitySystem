#include "BodySlidePostureResolver.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vanity_ube_heel_adapter::bodyslide {
namespace {

struct SliderValues {
    std::optional<float> lowWeight;
    std::optional<float> highWeight;

    bool Complete() const
    {
        return lowWeight.has_value() && highWeight.has_value();
    }
};

struct SliderSetRecord {
    std::string setName;
    std::string modelStem;
    std::string sourceFile;
    SliderValues noHeel;
    SliderValues hiHeelz;
};

struct PresetRecord {
    std::string presetName;
    std::string setName;
    std::string sourceFile;
    SliderValues noHeel;
};

enum class IndexState : std::uint8_t {
    kNotStarted,
    kBuilding,
    kReady,
    kFailed
};

std::atomic<IndexState> g_indexState{IndexState::kNotStarted};
std::atomic_bool g_loggedIndexPending{false};
std::jthread g_indexThread;
std::unordered_map<std::string, std::vector<SliderSetRecord>> g_setsByModel;
std::unordered_map<std::string, std::vector<PresetRecord>> g_presetsBySet;
std::unordered_set<std::string> g_loggedTraceModels;

std::string LowerCopy(std::string_view a_value)
{
    std::string result(a_value);
    std::ranges::transform(
        result, result.begin(), [](const unsigned char a_char) {
            return static_cast<char>(std::tolower(a_char));
        });
    return result;
}

std::string TrimCopy(std::string_view a_value)
{
    while (!a_value.empty() &&
           std::isspace(static_cast<unsigned char>(a_value.front()))) {
        a_value.remove_prefix(1);
    }
    while (!a_value.empty() &&
           std::isspace(static_cast<unsigned char>(a_value.back()))) {
        a_value.remove_suffix(1);
    }
    return std::string(a_value);
}

std::string XmlUnescape(std::string a_value)
{
    const std::pair<std::string_view, std::string_view> replacements[] = {
        {"&amp;", "&"},
        {"&quot;", "\""},
        {"&apos;", "'"},
        {"&lt;", "<"},
        {"&gt;", ">"}};

    for (const auto& [from, to] : replacements) {
        std::size_t pos = 0;
        while ((pos = a_value.find(from, pos)) != std::string::npos) {
            a_value.replace(pos, from.size(), to);
            pos += to.size();
        }
    }
    return a_value;
}

std::optional<std::string> ReadTextFile(const std::filesystem::path& a_path)
{
    std::ifstream stream(a_path, std::ios::binary);
    if (!stream.is_open()) {
        return std::nullopt;
    }

    return std::string(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

std::optional<std::string> Attribute(
    std::string_view a_openTag,
    std::string_view a_name)
{
    const auto lower = LowerCopy(a_openTag);
    const auto needle = LowerCopy(a_name);
    std::size_t pos = 0;

    while ((pos = lower.find(needle, pos)) != std::string::npos) {
        if (pos > 0) {
            const auto prev =
                static_cast<unsigned char>(lower[pos - 1]);
            if (!std::isspace(prev) && lower[pos - 1] != '<') {
                pos += needle.size();
                continue;
            }
        }

        auto cursor = pos + needle.size();
        while (cursor < lower.size() &&
               std::isspace(static_cast<unsigned char>(lower[cursor]))) {
            ++cursor;
        }
        if (cursor >= lower.size() || lower[cursor] != '=') {
            pos += needle.size();
            continue;
        }
        ++cursor;
        while (cursor < lower.size() &&
               std::isspace(static_cast<unsigned char>(lower[cursor]))) {
            ++cursor;
        }
        if (cursor >= lower.size() ||
            (lower[cursor] != '"' && lower[cursor] != '\'')) {
            return std::nullopt;
        }

        const char quote = lower[cursor++];
        const auto end = a_openTag.find(quote, cursor);
        if (end == std::string_view::npos) {
            return std::nullopt;
        }
        return XmlUnescape(
            std::string(a_openTag.substr(cursor, end - cursor)));
    }

    return std::nullopt;
}

std::optional<float> ParseFloat(std::string_view a_value)
{
    try {
        std::size_t used = 0;
        const auto value = std::stof(std::string(a_value), &used);
        if (used != a_value.size() || !std::isfinite(value)) {
            return std::nullopt;
        }
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string> TagText(
    std::string_view a_block,
    std::string_view a_tagName)
{
    const auto lower = LowerCopy(a_block);
    const auto openNeedle = "<" + LowerCopy(a_tagName);
    const auto closeNeedle = "</" + LowerCopy(a_tagName) + ">";

    const auto open = lower.find(openNeedle);
    if (open == std::string::npos) {
        return std::nullopt;
    }
    const auto openEnd = lower.find('>', open);
    if (openEnd == std::string::npos) {
        return std::nullopt;
    }
    const auto close = lower.find(closeNeedle, openEnd + 1);
    if (close == std::string::npos) {
        return std::nullopt;
    }

    return XmlUnescape(
        TrimCopy(a_block.substr(openEnd + 1, close - openEnd - 1)));
}

std::vector<std::string_view> Blocks(
    const std::string& a_content,
    std::string_view a_tagName)
{
    std::vector<std::string_view> result;
    const auto lower = LowerCopy(a_content);
    const auto openNeedle = "<" + LowerCopy(a_tagName);
    const auto closeNeedle = "</" + LowerCopy(a_tagName) + ">";

    std::size_t cursor = 0;
    while (cursor < lower.size()) {
        auto open = lower.find(openNeedle, cursor);
        if (open == std::string::npos) {
            break;
        }

        const auto afterName = open + openNeedle.size();
        if (afterName < lower.size() &&
            lower[afterName] != '>' &&
            !std::isspace(static_cast<unsigned char>(lower[afterName]))) {
            cursor = afterName;
            continue;
        }

        const auto close = lower.find(closeNeedle, afterName);
        if (close == std::string::npos) {
            break;
        }

        const auto end = close + closeNeedle.size();
        result.emplace_back(a_content.data() + open, end - open);
        cursor = end;
    }

    return result;
}

std::vector<std::string_view> OpenTags(
    std::string_view a_block,
    std::string_view a_tagName)
{
    std::vector<std::string_view> result;
    const auto lower = LowerCopy(a_block);
    const auto needle = "<" + LowerCopy(a_tagName);

    std::size_t cursor = 0;
    while (cursor < lower.size()) {
        auto open = lower.find(needle, cursor);
        if (open == std::string::npos) {
            break;
        }

        const auto afterName = open + needle.size();
        if (afterName < lower.size() &&
            lower[afterName] != '>' &&
            lower[afterName] != '/' &&
            !std::isspace(static_cast<unsigned char>(lower[afterName]))) {
            cursor = afterName;
            continue;
        }

        const auto end = lower.find('>', afterName);
        if (end == std::string::npos) {
            break;
        }

        result.emplace_back(
            a_block.data() + open,
            end - open + 1);
        cursor = end + 1;
    }

    return result;
}

std::string NormalizeModelStem(std::string_view a_path)
{
    std::string value(a_path);
    std::ranges::replace(value, '/', '\\');

    while (value.starts_with(".\\")) {
        value.erase(0, 2);
    }

    auto lower = LowerCopy(value);
    constexpr auto kMeshesPrefix = "meshes\\"sv;
    if (lower.starts_with(kMeshesPrefix)) {
        value.erase(0, kMeshesPrefix.size());
        lower.erase(0, kMeshesPrefix.size());
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

    return LowerCopy(value);
}

std::string_view Basename(std::string_view a_stem)
{
    const auto slash = a_stem.find_last_of("\\/");
    return slash == std::string_view::npos
        ? a_stem
        : a_stem.substr(slash + 1);
}

std::string OptionalValueText(const std::optional<float>& a_value)
{
    return a_value.has_value()
        ? std::format("{:.1f}", *a_value)
        : "missing";
}

void TraceSliderSet(
    const SliderSetRecord& a_set,
    const float a_actorWeight)
{
    const auto presetIt =
        g_presetsBySet.find(LowerCopy(a_set.setName));
    const std::size_t presetCount =
        presetIt == g_presetsBySet.end() ? 0 : presetIt->second.size();

    logger::info(
        "[bodyslide posture trace] exact set='{}' model='{}' "
        "NoHeel(small={},big={}) HiHeelz_CBBE(small={},big={}) "
        "zeroedPresetEntries={} actorWeight={:.1f} source='{}'",
        a_set.setName,
        a_set.modelStem,
        OptionalValueText(a_set.noHeel.lowWeight),
        OptionalValueText(a_set.noHeel.highWeight),
        OptionalValueText(a_set.hiHeelz.lowWeight),
        OptionalValueText(a_set.hiHeelz.highWeight),
        presetCount,
        a_actorWeight,
        a_set.sourceFile);

    if (presetIt == g_presetsBySet.end()) {
        logger::info(
            "[bodyslide posture trace] set='{}' zeroedPresetMatch=false",
            a_set.setName);
        return;
    }

    for (const auto& preset : presetIt->second) {
        logger::info(
            "[bodyslide posture trace] preset='{}' set='{}' "
            "NoHeel(small={},big={}) complete={} source='{}'",
            preset.presetName,
            preset.setName,
            OptionalValueText(preset.noHeel.lowWeight),
            OptionalValueText(preset.noHeel.highWeight),
            preset.noHeel.Complete(),
            preset.sourceFile);
    }
}

void TraceNearModelMatches(std::string_view a_stem)
{
    const auto basename = Basename(a_stem);
    std::size_t count = 0;

    for (const auto& [indexedStem, records] : g_setsByModel) {
        if (Basename(indexedStem) != basename) {
            continue;
        }

        for (const auto& record : records) {
            logger::info(
                "[bodyslide posture trace] same-basename candidate "
                "requested='{}' indexed='{}' set='{}' source='{}'",
                a_stem,
                indexedStem,
                record.setName,
                record.sourceFile);
            if (++count >= 8) {
                return;
            }
        }
    }

    if (count == 0) {
        logger::info(
            "[bodyslide posture trace] no same-basename SliderSet candidate "
            "for normalized='{}'",
            a_stem);
    }
}

SliderValues ParseSlider(
    std::string_view a_block,
    std::string_view a_sliderName)
{
    SliderValues result;
    for (const auto tag : OpenTags(a_block, "Slider")) {
        const auto name = Attribute(tag, "name");
        if (!name.has_value() ||
            LowerCopy(*name) != LowerCopy(a_sliderName)) {
            continue;
        }

        if (const auto lowWeight = Attribute(tag, "small")) {
            result.lowWeight = ParseFloat(*lowWeight);
        }
        if (const auto highWeight = Attribute(tag, "big")) {
            result.highWeight = ParseFloat(*highWeight);
        }
        break;
    }
    return result;
}

SliderValues ParsePresetSlider(
    std::string_view a_block,
    std::string_view a_sliderName)
{
    SliderValues result;
    for (const auto tag : OpenTags(a_block, "SetSlider")) {
        const auto name = Attribute(tag, "name");
        if (!name.has_value() ||
            LowerCopy(*name) != LowerCopy(a_sliderName)) {
            continue;
        }

        const auto size = Attribute(tag, "size");
        const auto valueText = Attribute(tag, "value");
        if (!size.has_value() || !valueText.has_value()) {
            continue;
        }

        const auto value = ParseFloat(*valueText);
        if (!value.has_value()) {
            continue;
        }

        const auto lowerSize = LowerCopy(*size);
        if (lowerSize == "small") {
            result.lowWeight = *value;
        } else if (lowerSize == "big") {
            result.highWeight = *value;
        }
    }
    return result;
}

std::string JoinOutputModel(
    std::string a_outputPath,
    std::string a_outputFile)
{
    std::ranges::replace(a_outputPath, '/', '\\');
    std::ranges::replace(a_outputFile, '/', '\\');

    while (!a_outputPath.empty() &&
           (a_outputPath.back() == '\\' || a_outputPath.back() == '/')) {
        a_outputPath.pop_back();
    }
    while (!a_outputFile.empty() &&
           (a_outputFile.front() == '\\' || a_outputFile.front() == '/')) {
        a_outputFile.erase(a_outputFile.begin());
    }

    if (a_outputPath.empty()) {
        return a_outputFile;
    }
    if (a_outputFile.empty()) {
        return a_outputPath;
    }
    return a_outputPath + "\\" + a_outputFile;
}

bool ScanSliderSets(
    const bool a_diagnostics,
    const std::stop_token a_stop)
{
    const std::filesystem::path root{
        "Data/CalienteTools/BodySlide/SliderSets"};
    if (!std::filesystem::exists(root)) {
        logger::warn(
            "[bodyslide] SliderSets directory not found at '{}'",
            root.string());
        return true;
    }

    std::size_t files = 0;
    std::size_t sets = 0;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(
             root,
             std::filesystem::directory_options::skip_permission_denied,
             ec),
         end;
         it != end;
         it.increment(ec)) {
        if (a_stop.stop_requested()) {
            return false;
        }
        if (ec) {
            ec.clear();
            continue;
        }
        if (!it->is_regular_file()) {
            continue;
        }

        const auto extension = LowerCopy(it->path().extension().string());
        if (extension != ".osp" && extension != ".xml") {
            continue;
        }

        const auto content = ReadTextFile(it->path());
        if (!content.has_value()) {
            continue;
        }
        ++files;

        for (const auto block : Blocks(*content, "SliderSet")) {
            const auto openEnd = block.find('>');
            if (openEnd == std::string_view::npos) {
                continue;
            }
            const auto openTag = block.substr(0, openEnd + 1);
            const auto setName = Attribute(openTag, "name");
            const auto outputPath = TagText(block, "OutputPath");
            const auto outputFile = TagText(block, "OutputFile");
            if (!setName.has_value() ||
                !outputPath.has_value() ||
                !outputFile.has_value()) {
                continue;
            }

            SliderSetRecord record;
            record.setName = *setName;
            record.modelStem = NormalizeModelStem(
                JoinOutputModel(*outputPath, *outputFile));
            record.sourceFile = it->path().string();
            record.noHeel = ParseSlider(block, "NoHeel");
            record.hiHeelz = ParseSlider(block, "HiHeelz_CBBE");

            if (record.modelStem.empty()) {
                continue;
            }
            g_setsByModel[record.modelStem].push_back(std::move(record));
            ++sets;
        }
    }

    logger::info(
        "[bodyslide] indexed SliderSets files={} sets={} modelStems={}",
        files,
        sets,
        g_setsByModel.size());

    if (a_diagnostics) {
        std::size_t noHeelSets = 0;
        std::size_t hiHeelSets = 0;
        for (const auto& [_, records] : g_setsByModel) {
            for (const auto& record : records) {
                if (record.noHeel.lowWeight.has_value() ||
                    record.noHeel.highWeight.has_value()) {
                    ++noHeelSets;
                }
                if (record.hiHeelz.lowWeight.has_value() ||
                    record.hiHeelz.highWeight.has_value()) {
                    ++hiHeelSets;
                }
            }
        }
        logger::info(
            "[bodyslide] slider capability sets: NoHeel={} HiHeelz_CBBE={}",
            noHeelSets,
            hiHeelSets);
    }
    return !a_stop.stop_requested();
}

bool ScanPresets(const std::stop_token a_stop)
{
    const std::filesystem::path root{
        "Data/CalienteTools/BodySlide/SliderPresets"};
    if (!std::filesystem::exists(root)) {
        logger::warn(
            "[bodyslide] SliderPresets directory not found at '{}'",
            root.string());
        return true;
    }

    std::size_t files = 0;
    std::size_t zeroed = 0;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(
             root,
             std::filesystem::directory_options::skip_permission_denied,
             ec),
         end;
         it != end;
         it.increment(ec)) {
        if (a_stop.stop_requested()) {
            return false;
        }
        if (ec) {
            ec.clear();
            continue;
        }
        if (!it->is_regular_file() ||
            LowerCopy(it->path().extension().string()) != ".xml") {
            continue;
        }

        const auto content = ReadTextFile(it->path());
        if (!content.has_value()) {
            continue;
        }
        ++files;

        for (const auto block : Blocks(*content, "Preset")) {
            const auto openEnd = block.find('>');
            if (openEnd == std::string_view::npos) {
                continue;
            }
            const auto openTag = block.substr(0, openEnd + 1);
            const auto presetName = Attribute(openTag, "name");
            const auto setName = Attribute(openTag, "set");
            if (!presetName.has_value() || !setName.has_value()) {
                continue;
            }

            if (LowerCopy(*presetName).find("zeroed") == std::string::npos) {
                continue;
            }

            PresetRecord record;
            record.presetName = *presetName;
            record.setName = *setName;
            record.sourceFile = it->path().string();
            record.noHeel = ParsePresetSlider(block, "NoHeel");

            g_presetsBySet[LowerCopy(record.setName)]
                .push_back(std::move(record));
            ++zeroed;
        }
    }

    logger::info(
        "[bodyslide] indexed SliderPresets files={} zeroedPresetEntries={}",
        files,
        zeroed);
    return !a_stop.stop_requested();
}

void BuildIndex(
    const bool a_diagnostics,
    const std::stop_token a_stop,
    const IndexReadyCallback a_readyCallback)
{
    const auto started = std::chrono::steady_clock::now();

    g_setsByModel.clear();
    g_presetsBySet.clear();

    if (!ScanSliderSets(a_diagnostics, a_stop) ||
        !ScanPresets(a_stop) ||
        a_stop.stop_requested()) {
        logger::info("[bodyslide] asynchronous index build cancelled");
        return;
    }

    g_indexState.store(IndexState::kReady, std::memory_order_release);
    g_loggedIndexPending.store(false);

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    logger::info(
        "[bodyslide] asynchronous index ready in {:.3f}s",
        static_cast<double>(elapsed.count()) / 1000.0);

    if (a_readyCallback) {
        a_readyCallback();
    }
}

float Interpolate(
    const float a_small,
    const float a_big,
    const float a_actorWeight)
{
    const auto weight = std::clamp(a_actorWeight, 0.0F, 100.0F) / 100.0F;
    return a_small + weight * (a_big - a_small);
}

std::optional<PostureCandidate> CandidateFromSet(
    const SliderSetRecord& a_set,
    const float a_actorWeight,
    const bool a_diagnostics)
{
    const auto presetIt =
        g_presetsBySet.find(LowerCopy(a_set.setName));

    std::optional<PostureCandidate> presetCandidate;
    if (presetIt != g_presetsBySet.end()) {
        for (const auto& preset : presetIt->second) {
            if (!preset.noHeel.Complete()) {
                continue;
            }

            PostureCandidate candidate;
            candidate.lowWeightValue = *preset.noHeel.lowWeight;
            candidate.highWeightValue = *preset.noHeel.highWeight;
            candidate.posture = std::clamp(
                Interpolate(
                    candidate.lowWeightValue,
                    candidate.highWeightValue,
                    a_actorWeight) /
                    100.0F,
                0.0F,
                1.0F);
            candidate.highConfidence = true;
            candidate.source = "zeroed-preset";
            candidate.setName = a_set.setName;
            candidate.modelStem = a_set.modelStem;
            candidate.sourceFile = preset.sourceFile;

            if (presetCandidate.has_value() &&
                std::abs(
                    presetCandidate->posture -
                    candidate.posture) > 0.01F) {
                logger::warn(
                    "[bodyslide posture] conflicting zeroed presets for "
                    "set='{}': {:.3f} vs {:.3f}",
                    a_set.setName,
                    presetCandidate->posture,
                    candidate.posture);
                return std::nullopt;
            }
            presetCandidate = std::move(candidate);
        }
    }

    if (presetCandidate.has_value()) {
        return presetCandidate;
    }

    if (a_set.noHeel.Complete()) {
        PostureCandidate candidate;
        candidate.lowWeightValue = *a_set.noHeel.lowWeight;
        candidate.highWeightValue = *a_set.noHeel.highWeight;
        candidate.posture = std::clamp(
            Interpolate(
                candidate.lowWeightValue,
                candidate.highWeightValue,
                a_actorWeight) /
                100.0F,
            0.0F,
            1.0F);
        candidate.highConfidence = false;
        candidate.source = "sliderset-default";
        candidate.setName = a_set.setName;
        candidate.modelStem = a_set.modelStem;
        candidate.sourceFile = a_set.sourceFile;
        return candidate;
    }

    if (a_diagnostics &&
        (a_set.hiHeelz.lowWeight.has_value() ||
         a_set.hiHeelz.highWeight.has_value())) {
        logger::info(
            "[bodyslide posture] set='{}' model='{}' has "
            "HiHeelz_CBBE but no usable NoHeel; not inferred",
            a_set.setName,
            a_set.modelStem);
    }

    return std::nullopt;
}

}  // namespace

void Reset()
{
    if (g_indexThread.joinable()) {
        g_indexThread.request_stop();
        g_indexThread.join();
    }

    g_setsByModel.clear();
    g_presetsBySet.clear();
    g_loggedTraceModels.clear();
    g_loggedIndexPending.store(false);
    g_indexState.store(IndexState::kNotStarted, std::memory_order_release);
}

void PrepareAsync(
    const bool a_diagnostics,
    const IndexReadyCallback a_readyCallback)
{
    auto expected = IndexState::kNotStarted;
    if (!g_indexState.compare_exchange_strong(
            expected,
            IndexState::kBuilding,
            std::memory_order_acq_rel)) {
        if (expected == IndexState::kReady && a_readyCallback) {
            a_readyCallback();
        }
        return;
    }

    logger::info("[bodyslide] starting asynchronous index build");

    g_indexThread = std::jthread(
        [a_diagnostics, a_readyCallback](const std::stop_token a_stop) {
            try {
                BuildIndex(a_diagnostics, a_stop, a_readyCallback);
            } catch (const std::exception& exception) {
                logger::error(
                    "[bodyslide] asynchronous index build failed: {}",
                    exception.what());
                g_indexState.store(
                    IndexState::kFailed,
                    std::memory_order_release);
            } catch (...) {
                logger::error(
                    "[bodyslide] asynchronous index build failed with "
                    "unknown exception");
                g_indexState.store(
                    IndexState::kFailed,
                    std::memory_order_release);
            }
        });
}

bool IsReady()
{
    return g_indexState.load(std::memory_order_acquire) ==
           IndexState::kReady;
}

std::optional<PostureCandidate> ResolvePosture(
    const std::span<const std::string> a_modelPaths,
    const float a_actorWeight,
    const bool a_diagnostics)
{
    const auto state =
        g_indexState.load(std::memory_order_acquire);
    if (state != IndexState::kReady) {
        if (state == IndexState::kNotStarted) {
            PrepareAsync(a_diagnostics, nullptr);
        }
        if (!g_loggedIndexPending.exchange(true)) {
            logger::info(
                "[bodyslide posture] index not ready; returning without "
                "blocking the game thread");
        }
        return std::nullopt;
    }

    g_loggedIndexPending.store(false);
    std::optional<PostureCandidate> resolved;

    for (const auto& path : a_modelPaths) {
        const auto stem = NormalizeModelStem(path);
        if (stem.empty()) {
            continue;
        }

        const bool trace =
            a_diagnostics && g_loggedTraceModels.insert(stem).second;
        if (trace) {
            logger::info(
                "[bodyslide posture trace] input='{}' normalized='{}'",
                path,
                stem);
        }

        const auto it = g_setsByModel.find(stem);
        if (it == g_setsByModel.end()) {
            if (trace) {
                logger::info(
                    "[bodyslide posture trace] normalized='{}' "
                    "exactSliderSetMatch=false",
                    stem);
                TraceNearModelMatches(stem);
            }
            continue;
        }

        if (trace) {
            logger::info(
                "[bodyslide posture trace] normalized='{}' "
                "exactSliderSetMatch=true matchingSets={}",
                stem,
                it->second.size());
        }

        bool anyCandidateForStem = false;
        for (const auto& set : it->second) {
            if (trace) {
                TraceSliderSet(set, a_actorWeight);
            }

            const auto candidate =
                CandidateFromSet(set, a_actorWeight, a_diagnostics);
            if (!candidate.has_value()) {
                if (trace) {
                    logger::info(
                        "[bodyslide posture trace] set='{}' result=none "
                        "reason=no-complete-NoHeel-source",
                        set.setName);
                }
                continue;
            }

            anyCandidateForStem = true;
            if (trace) {
                logger::info(
                    "[bodyslide posture trace] set='{}' result=candidate "
                    "source={} posture={:.3f} confidence={} "
                    "small={:.1f} big={:.1f}",
                    set.setName,
                    candidate->source,
                    candidate->posture,
                    candidate->highConfidence ? "high" : "low",
                    candidate->lowWeightValue,
                    candidate->highWeightValue);
            }

            if (!resolved.has_value()) {
                resolved = candidate;
                continue;
            }

            if (resolved->highConfidence != candidate->highConfidence) {
                if (candidate->highConfidence) {
                    resolved = candidate;
                }
                continue;
            }

            if (std::abs(resolved->posture - candidate->posture) > 0.01F) {
                logger::warn(
                    "[bodyslide posture] ambiguous exact model match '{}': "
                    "set='{}' posture={:.3f} vs set='{}' posture={:.3f}",
                    stem,
                    resolved->setName,
                    resolved->posture,
                    candidate->setName,
                    candidate->posture);
                return std::nullopt;
            }
        }

        if (trace && !anyCandidateForStem) {
            logger::info(
                "[bodyslide posture trace] normalized='{}' finalForStem=none",
                stem);
        }
    }

    if (a_diagnostics) {
        if (resolved.has_value()) {
            logger::info(
                "[bodyslide posture trace] final result source={} set='{}' "
                "posture={:.3f} confidence={}",
                resolved->source,
                resolved->setName,
                resolved->posture,
                resolved->highConfidence ? "high" : "low");
        } else {
            logger::info(
                "[bodyslide posture trace] final result=none for "
                "{} model path(s)",
                a_modelPaths.size());
        }
    }

    return resolved;
}

}  // namespace vanity_ube_heel_adapter::bodyslide
