#pragma once
// Pure selection helpers: normalization is NOT evidence of a morph alias.
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace vanity_ube_heel_adapter::foot_capture_policy {
inline constexpr std::array<int, 3> RetryMilliseconds{150, 500, 1500};
inline std::string ResourceKey(std::string_view input)
{
    std::string path;
    for (unsigned char ch : input) {
        const char c = ch == '\\' ? '/' : static_cast<char>(std::tolower(ch));
        if (c == '/' && !path.empty() && path.back() == '/') continue;
        path.push_back(c);
    }
    if (path.empty() || path.front() == '/' || path.find(':') != std::string::npos) return {};
    while (path.starts_with("./")) path.erase(0, 2);
    if (path.starts_with("data/meshes/")) path.erase(0, 12);
    else if (path.starts_with("meshes/")) path.erase(0, 7);
    std::size_t begin = 0;
    while (begin <= path.size()) {
        const auto end = path.find('/', begin);
        const auto part = path.substr(begin, end == std::string::npos ? end : end - begin);
        if (part == ".." || part.empty()) return {};
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return path;
}
inline bool SameResource(std::string_view left, std::string_view right)
{
    const auto a = ResourceKey(left), b = ResourceKey(right);
    return !a.empty() && a == b;
}
struct Evidence {
    std::uintptr_t identity{};
    bool exactFeet{}, supportedGeometry{};
    std::vector<std::string> bodyTris;
};
// No basename, name-only, nearest-node, or buffered-node acceptance.
inline std::optional<std::uintptr_t> UniqueResourceMatch(
    std::span<const Evidence> live, std::span<const std::string> expected)
{
    std::optional<std::uintptr_t> selected;
    for (const auto& candidate : live) {
        if (!candidate.identity || !candidate.exactFeet || !candidate.supportedGeometry) continue;
        bool matched = false;
        for (const auto& a : candidate.bodyTris)
            for (const auto& b : expected) matched |= SameResource(a, b);
        if (!matched) continue;
        if (selected && *selected != candidate.identity) return {};
        selected = candidate.identity;
    }
    return selected;
}
inline bool CurrentTicket(std::uint64_t scheduled, std::uint64_t latest)
{
    return scheduled != 0 && scheduled == latest;
}
} // namespace vanity_ube_heel_adapter::foot_capture_policy
