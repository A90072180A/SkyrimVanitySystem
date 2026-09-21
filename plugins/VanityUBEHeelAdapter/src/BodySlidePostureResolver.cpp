#include "BodySlidePostureResolver.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vanity_ube_heel_adapter::bodyslide {
namespace {

struct SliderValues {
    std::optional<float> small;
    std::optional<float> big;

    bool Complete() const
    {
        return small.has_value() && big.has_value();
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

bool g_indexBuilt{false};
std::unordered_map<std::string, std::vector<SliderSetRecord>> g_setsByModel;
std::unordered_map<std::string, std::vector<PresetRecord>> g_presetsBySet;

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

        if (const auto small = Attribute(tag, "small")) {
            result.small = ParseFloat(*small);
        }
        if (const auto big = Attribute(tag, "big")) {
            result.big = ParseFloat(*big);
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
            result.small = *value;
        } else if (lowerSize == "big") {
            result.big = *value;
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

void ScanSliderSets(const bool a_diagnostics)
{
    const std::filesystem::path root{
        "Data/CalienteTools/BodySlide/SliderSets"};
    if (!std::filesystem::exists(root)) {
        logger::warn(
            "[bodyslide] SliderSets directory not found at '{}'",
            root.string());
        return;
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
                if (record.noHeel.small.has_value() ||
                    record.noHeel.big.has_value()) {
                    ++noHeelSets;
                }
                if (record.hiHeelz.small.has_value() ||
                    record.hiHeelz.big.has_value()) {
                    ++hiHeelSets;
                }
            }
        }
        logger::info(
            "[bodyslide] slider capability sets: NoHeel={} HiHeelz_CBBE={}",
            noHeelSets,
            hiHeelSets);
    }
}

void ScanPresets()
{
    const std::filesystem::path root{
        "Data/CalienteTools/BodySlide/SliderPresets"};
    if (!std::filesystem::exists(root)) {
        logger::warn(
            "[bodyslide] SliderPresets directory not found at '{}'",
            root.string());
        return;
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
}

void EnsureIndex(const bool a_diagnostics)
{
    if (g_indexBuilt) {
        return;
    }

    g_setsByModel.clear();
    g_presetsBySet.clear();
    ScanSliderSets(a_diagnostics);
    ScanPresets();
    g_indexBuilt = true;
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
            candidate.smallValue = *preset.noHeel.small;
            candidate.bigValue = *preset.noHeel.big;
            candidate.posture = std::clamp(
                Interpolate(
                    candidate.smallValue,
                    candidate.bigValue,
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
        candidate.smallValue = *a_set.noHeel.small;
        candidate.bigValue = *a_set.noHeel.big;
        candidate.posture = std::clamp(
            Interpolate(
                candidate.smallValue,
                candidate.bigValue,
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
        (a_set.hiHeelz.small.has_value() ||
         a_set.hiHeelz.big.has_value())) {
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
    g_indexBuilt = false;
    g_setsByModel.clear();
    g_presetsBySet.clear();
}

std::optional<PostureCandidate> ResolvePosture(
    const std::span<const std::string> a_modelPaths,
    const float a_actorWeight,
    const bool a_diagnostics)
{
    EnsureIndex(a_diagnostics);

    std::optional<PostureCandidate> resolved;
    for (const auto& path : a_modelPaths) {
        const auto stem = NormalizeModelStem(path);
        if (stem.empty()) {
            continue;
        }

        const auto it = g_setsByModel.find(stem);
        if (it == g_setsByModel.end()) {
            continue;
        }

        for (const auto& set : it->second) {
            const auto candidate =
                CandidateFromSet(set, a_actorWeight, a_diagnostics);
            if (!candidate.has_value()) {
                continue;
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
    }

    return resolved;
}

}  // namespace vanity_ube_heel_adapter::bodyslide
