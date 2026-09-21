#pragma once
// Portable, engine-independent validation. No borrowed runtime pointers here.
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vanity_ube_heel_adapter::foot_snapshot_core {
using Point = std::array<float, 3>;
using Triangle = std::array<std::uint32_t, 3>;
struct Result {
    std::string status;
    std::vector<Point> positions;
    std::vector<Triangle> triangles;
    std::uint64_t topologyHash{0};
    std::uint64_t positionHash{0};
    bool Complete() const { return status == "complete"; }
};
inline void HashU32(std::uint64_t& h, std::uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i) {
        h ^= (v >> (8 * i)) & 255u;
        h *= 1099511628211ULL;
    }
}
inline std::uint64_t HashText(std::string_view s)
{
    std::uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}
inline float HalfToFloat(std::uint16_t h)
{
    const unsigned exponent = (h >> 10) & 31u;
    const unsigned mantissa = h & 1023u;
    float value;
    if (exponent == 0) value = std::ldexp(static_cast<float>(mantissa), -24);
    else if (exponent == 31) value = mantissa ? NAN : INFINITY;
    else value = std::ldexp(static_cast<float>(1024u + mantissa), static_cast<int>(exponent) - 25);
    return (h & 0x8000u) ? -value : value;
}
inline Result Decode(std::span<const std::uint8_t> bytes,
                     std::span<const std::uint16_t> indices,
                     std::uint32_t count, std::uint32_t stride,
                     std::uint32_t componentBytes)
{
    auto fail = [](const char* status) { Result r; r.status = status; return r; };
    if (!count || count > 65535u) return fail("invalid-vertex-count");
    if (componentBytes != 2 && componentBytes != 4) return fail("unsupported-component-size");
    if (stride < 3 * componentBytes || stride > 256u) return fail("invalid-vertex-stride");
    if (bytes.size() != static_cast<std::size_t>(count) * stride) return fail("vertex-buffer-size-mismatch");
    if (indices.empty() || indices.size() % 3 || indices.size() > 196605u) return fail("invalid-index-count");
    for (const auto i : indices) if (i >= count) return fail("index-out-of-range");
    Result r;
    r.positions.reserve(count);
    r.triangles.reserve(indices.size() / 3);
    r.topologyHash = r.positionHash = 14695981039346656037ULL;
    HashU32(r.topologyHash, count);
    HashU32(r.topologyHash, static_cast<std::uint32_t>(indices.size()));
    HashU32(r.positionHash, count);
    for (std::uint32_t i = 0; i < count; ++i) {
        Point point{};
        for (unsigned axis = 0; axis < 3; ++axis) {
            const auto* src = bytes.data() + static_cast<std::size_t>(i) * stride + axis * componentBytes;
            if (componentBytes == 4) std::memcpy(&point[axis], src, 4);
            else { std::uint16_t h; std::memcpy(&h, src, 2); point[axis] = HalfToFloat(h); }
            if (!std::isfinite(point[axis])) return fail("nonfinite-position");
            if (point[axis] == 0.0F) point[axis] = 0.0F;
            HashU32(r.positionHash, std::bit_cast<std::uint32_t>(point[axis]));
        }
        r.positions.push_back(point);
    }
    bool nondegenerate = false;
    for (std::size_t i = 0; i < indices.size(); i += 3) {
        const Triangle t{indices[i], indices[i + 1], indices[i + 2]};
        for (const auto v : t) HashU32(r.topologyHash, v);
        nondegenerate |= t[0] != t[1] && t[1] != t[2] && t[0] != t[2];
        r.triangles.push_back(t);
    }
    if (!nondegenerate) return fail("all-indices-degenerate");
    r.status = "complete";
    return r;
}
} // namespace vanity_ube_heel_adapter::foot_snapshot_core
