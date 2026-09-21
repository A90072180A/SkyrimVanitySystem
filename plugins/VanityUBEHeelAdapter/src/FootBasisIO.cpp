#include "FootBasisIO.h"
#include "TriMorphCore.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <format>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace vanity_ube_heel_adapter::foot_basis_io {
namespace {
using Json = nlohmann::json;
namespace tri = tri_morph_core;
struct Cached {
    std::uintmax_t size{};
    std::filesystem::file_time_type modified{};
    Json result;
};
// Owned exclusively by the snapshot writer thread. Not a persistent index.
std::unordered_map<std::string, Cached> cache;
std::optional<std::filesystem::path> ResourcePath(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    if (value.empty() || value.front() == '/' || value.find(':') != std::string::npos) return {};
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.starts_with("data/meshes/")) value.erase(0, 12);
    else if (lower.starts_with("meshes/")) value.erase(0, 7);
    std::filesystem::path relative(value);
    for (const auto& component : relative) if (component == "..") return {};
    auto extension = relative.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension != ".tri") return {};
    return std::filesystem::path("Data/Meshes") / relative;
}
Json ReadBasis(const std::string& resource, std::uint32_t limit) {
    Json out = {{"resource", resource}, {"shape", "Feet"}, {"morph", "NoHeel"},
        {"readScope", "MO2-visible loose files; BSA-only resources are not read by this worker"},
        {"status", "resource-unreadable"}, {"offsets", Json::array()},
        {"vertexLimitChecked", limit}, {"referenceTopologyValidated", false}};
    const auto path = ResourcePath(resource);
    if (!path) { out["status"] = "invalid-resource-path"; return out; }
    std::error_code ec;
    const auto size = std::filesystem::file_size(*path, ec);
    if (ec) { out["error"] = ec.message(); return out; }
    const auto modified = std::filesystem::last_write_time(*path, ec);
    if (ec) { out["error"] = ec.message(); return out; }
    const auto key = path->generic_string() + "|" + std::to_string(limit);
    if (const auto found = cache.find(key); found != cache.end() && found->second.size == size && found->second.modified == modified)
        return found->second.result;
    if (size > 64u * 1024u * 1024u) { out["status"] = "file-too-large"; return out; }
    std::ifstream stream(*path, std::ios::binary);
    if (!stream) return out;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (size && !stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
        out["status"] = "resource-read-failed"; return out;
    }
    const auto afterSize = std::filesystem::file_size(*path, ec);
    if (ec || afterSize != size) { out["status"] = "resource-changed-during-read"; return out; }
    const auto afterTime = std::filesystem::last_write_time(*path, ec);
    if (ec || afterTime != modified) { out["status"] = "resource-changed-during-read"; return out; }
    const auto parsed = tri::Parse(bytes, "Feet", "NoHeel", limit);
    out["status"] = parsed.status;
    out["error"] = parsed.error;
    out["sourceBytes"] = size;
    out["sourceFingerprint"] = std::format("{:016x}", parsed.sourceHash);
    out["fingerprintAlgorithm"] = "FNV1a64 of file bytes; diagnostic, not cryptographic";
    out["shapeNames"] = parsed.shapeNames;
    out["morphNamesForShape"] = parsed.morphNames;
    out["deltaSemantics"] = "packed position differences per 1.0 morph unit; baseline/direction not calibrated";
    for (const auto& offset : parsed.offsets) out["offsets"].push_back({{"index", offset.index}, {"delta", offset.delta}});
    out["nonzeroOffsetCount"] = parsed.offsets.size();
    // Only a completed structural parse can establish present/absent. An I/O
    // failure and a malformed/unsupported TRI never become 'NoHeel=false'.
    logger::info("[NoHeel basis] resource='{}' status={} shape=Feet offsets={}", resource, parsed.status, parsed.offsets.size());
    if (cache.size() >= 64) cache.clear();
    cache.insert_or_assign(key, Cached{size, modified, out});
    return out;
}
}
void Enrich(Json& document) {
    document["triMorphData"] = Json::array();
    const auto count = document.at("positions").size();
    const auto limit = count > 0 && count <= 65535 ? static_cast<std::uint32_t>(count) : 65536u;
    std::unordered_set<std::string> visited;
    for (const auto& entry : document.at("identity").at("bodyTriPaths")) {
        if (!entry.is_string()) continue;
        const auto resource = entry.get<std::string>();
        if (visited.size() >= 8) { document["triMorphDataTruncated"] = true; break; }
        if (!visited.insert(resource).second) continue;
        document["triMorphData"].push_back(ReadBasis(resource, limit));
    }
    // This optional independent resource supplies a measured delta, NOT a
    // certified topology or a guessed flat/heel endpoint for the observed shoe.
    const auto reference = document.value("requestedReferenceBodyTri", std::string{});
    if (!reference.empty()) document["referenceTriBasis"] = ReadBasis(reference, 65536u);
    document["referenceCalibration"] = {{"status", "not-calibrated"}, {"posture", nullptr}};
}
} // namespace vanity_ube_heel_adapter::foot_basis_io
