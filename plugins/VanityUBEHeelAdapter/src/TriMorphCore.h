#pragma once
// Read-only parser for BodySlide's packed BODYTRI (PIRT) format.
// Format reference: ousnius/BodySlide-and-Outfit-Studio/src/files/TriFile.cpp.
// No engine pointers, file I/O, extrapolated aliases, or whole-file substring tests.
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace vanity_ube_heel_adapter::tri_morph_core {
struct Offset { std::uint32_t index{}; std::array<float, 3> delta{}; };
struct Result {
    std::string status{"not-parsed"};
    std::string error;
    std::vector<std::string> shapeNames;
    std::vector<std::string> morphNames;
    std::vector<Offset> offsets;
    std::uint64_t sourceHash{};
    bool Present() const { return status == "present"; }
};
inline std::uint64_t HashBytes(std::span<const std::uint8_t> bytes) {
    std::uint64_t h = 14695981039346656037ULL;
    for (auto b : bytes) { h ^= b; h *= 1099511628211ULL; }
    return h;
}
class Reader {
    std::span<const std::uint8_t> data;
    std::size_t pos{};
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : data(bytes) {}
    std::size_t Remaining() const { return data.size() - pos; }
    void Require(std::size_t n) const {
        if (n > Remaining()) throw std::runtime_error("truncated-record");
    }
    void Skip(std::size_t n) { Require(n); pos += n; }
    std::uint8_t U8() { Require(1); return data[pos++]; }
    std::uint16_t U16() {
        const auto a = U8(); const auto b = U8();
        return static_cast<std::uint16_t>(a | (static_cast<unsigned>(b) << 8));
    }
    std::int16_t I16() { return std::bit_cast<std::int16_t>(U16()); }
    float F32() {
        const auto lo = U16(); const auto hi = U16();
        const auto bits = static_cast<std::uint32_t>(lo) | (static_cast<std::uint32_t>(hi) << 16);
        const auto value = std::bit_cast<float>(bits);
        if (!std::isfinite(value)) throw std::runtime_error("nonfinite-multiplier");
        return value;
    }
    std::string Name() {
        const auto n = U8(); Require(n);
        if (!n) throw std::runtime_error("empty-name");
        std::string value(reinterpret_cast<const char*>(data.data() + pos), n); pos += n;
        if (value.find('\0') != std::string::npos) throw std::runtime_error("embedded-nul-name");
        return value;
    }
};
inline Result Parse(std::span<const std::uint8_t> bytes,
                    std::string_view wantedShape = "Feet",
                    std::string_view wantedMorph = "NoHeel",
                    std::uint32_t vertexLimit = 65536) {
    Result result;
    if (bytes.size() > 64u * 1024u * 1024u) { result.status = "file-too-large"; return result; }
    result.sourceHash = HashBytes(bytes);
    if (bytes.size() < 4) { result.status = "malformed"; result.error = "truncated-header"; return result; }
    if (std::string_view(reinterpret_cast<const char*>(bytes.data()), 4) != "PIRT") {
        result.status = "unsupported-format"; return result;
    }
    if (!vertexLimit || vertexLimit > 65536 || wantedShape.empty() || wantedMorph.empty()) {
        result.status = "invalid-request"; return result;
    }
    try {
        Reader reader(bytes); reader.Skip(4);
        bool foundShape = false, foundMorph = false;
        auto readSection = [&](unsigned dimensions, bool positionSection) {
            const auto shapes = reader.U16();
            if (shapes > 4096) throw std::runtime_error("shape-count-limit");
            std::unordered_set<std::string> seenShapes;
            for (unsigned i = 0; i < shapes; ++i) {
                const auto shape = reader.Name();
                if (!seenShapes.insert(shape).second) throw std::runtime_error("duplicate-shape");
                if (positionSection) result.shapeNames.push_back(shape);
                const bool shapeMatch = positionSection && shape == wantedShape;
                foundShape |= shapeMatch;
                const auto morphs = reader.U16();
                if (morphs > 4096) throw std::runtime_error("morph-count-limit");
                std::unordered_set<std::string> seenMorphs;
                for (unsigned j = 0; j < morphs; ++j) {
                    const auto morph = reader.Name();
                    if (!seenMorphs.insert(morph).second) throw std::runtime_error("duplicate-morph");
                    if (shapeMatch) result.morphNames.push_back(morph);
                    const auto multiplier = reader.F32();
                    if (multiplier < 0) throw std::runtime_error("negative-multiplier");
                    const auto count = reader.U16();
                    const auto recordBytes = static_cast<std::size_t>(count) * (2u + 2u * dimensions);
                    reader.Require(recordBytes);
                    const bool selected = shapeMatch && morph == wantedMorph;
                    if (!selected) { reader.Skip(recordBytes); continue; }
                    foundMorph = true;
                    std::unordered_set<std::uint16_t> seenVertices;
                    for (unsigned k = 0; k < count; ++k) {
                        const auto id = reader.U16();
                        if (id >= vertexLimit) throw std::runtime_error("morph-index-out-of-range");
                        if (!seenVertices.insert(id).second) throw std::runtime_error("duplicate-morph-index");
                        Offset offset; offset.index = id;
                        bool nonzero = false;
                        for (unsigned axis = 0; axis < dimensions; ++axis) {
                            offset.delta[axis] = static_cast<float>(reader.I16()) * multiplier;
                            if (!std::isfinite(offset.delta[axis])) throw std::runtime_error("nonfinite-offset");
                            nonzero |= offset.delta[axis] != 0;
                        }
                        if (nonzero) result.offsets.push_back(offset);
                    }
                }
            }
        };
        readSection(3, true);
        // Old packed files can end after position data. Newer files append UV data.
        if (reader.Remaining()) readSection(2, false);
        if (reader.Remaining()) throw std::runtime_error("unexpected-trailing-data");
        result.status = !foundShape ? "shape-not-found" : !foundMorph ? "morph-absent" :
                        result.offsets.empty() ? "empty-morph" : "present";
    } catch (const std::exception& e) {
        result.status = "malformed"; result.error = e.what(); result.offsets.clear();
    }
    return result;
}
} // namespace vanity_ube_heel_adapter::tri_morph_core
