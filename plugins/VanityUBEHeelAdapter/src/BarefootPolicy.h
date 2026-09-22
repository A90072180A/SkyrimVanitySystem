#pragma once
// Policy only: no game objects, disk reads or guessed shoe-height values.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace vanity_ube_heel_adapter::barefoot_policy {
struct Evidence {
    bool enabled{true};
    bool snapshotValid{};
    std::size_t declaredFootwear{}, liveFootwear{};
    bool realFootwearWorn{};
    bool activeSkinPart{}, skinAddonConfirmed{}, expectedBareFeetModel{};
    bool currentSkinnedGeometry{};
};
inline std::string_view Decision(const Evidence& e) {
    if (!e.enabled) return "barefoot-disabled";
    if (!e.snapshotValid) return "snapshot-unverified";
    if (e.liveFootwear > 1) return "ambiguous-visible-footwear";
    if (e.liveFootwear == 1) return "visible-footwear";
    // Missing BODYTRI / failed matching must not turn an advertised shoe into
    // naked feet. Actual worn shoes also block this deliberately narrow fallback.
    if (e.declaredFootwear) return "unresolved-visible-footwear";
    if (e.realFootwearWorn) return "real-footwear-still-worn";
    if (!e.activeSkinPart) return "bare-skin-part-unconfirmed";
    if (!e.skinAddonConfirmed) return "bare-skin-addon-unconfirmed";
    if (!e.expectedBareFeetModel) return "bare-foot-model-unconfirmed";
    if (!e.currentSkinnedGeometry) return "bare-foot-geometry-unconfirmed";
    return "confirmed-barefoot";
}
// A short, non-blocking stable-state window rejects attach/detach transients.
// Clock values are supplied by the caller for deterministic regression tests.
class Settler {
    std::string key;
    std::optional<std::uint64_t> since;
public:
    void Reset() { key.clear(); since.reset(); }
    bool Ready(std::string_view decision, const std::string& signature,
               std::uint64_t nowMs, std::uint64_t delayMs = 250) {
        if (decision != "confirmed-barefoot" || signature.empty()) {
            Reset(); return false;
        }
        if (!since || key != signature || nowMs < *since) {
            key = signature; since = nowMs; return delayMs == 0;
        }
        return nowMs - *since >= delayMs;
    }
};
} // namespace vanity_ube_heel_adapter::barefoot_policy
