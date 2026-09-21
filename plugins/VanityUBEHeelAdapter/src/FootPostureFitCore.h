#pragma once
// Mathematical kernel only: callers must certify reference identity, units,
// morph semantics and common coordinate space. A fit is NOT a calibration.
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
namespace vanity_ube_heel_adapter::foot_posture_fit_core {
using Point = std::array<float, 3>;
using Triangle = std::array<std::uint32_t, 3>;
struct Result {
    std::string status;
    double rawPosture{}, rmsResidual{}, rmsUnitDelta{};
    bool inUnitInterval{};
};
inline Result Fit(std::span<const Point> anchor, std::span<const Point> target,
                  std::span<const Point> unitDelta, std::span<const Triangle> anchorTriangles,
                  std::span<const Triangle> targetTriangles, double anchorPosture) {
    Result r;
    if (anchor.empty() || anchor.size() != target.size() || anchor.size() != unitDelta.size() ||
        anchorTriangles.empty() || anchorTriangles.size() != targetTriangles.size()) {
        r.status = "size-mismatch"; return r;
    }
    if (!std::isfinite(anchorPosture)) { r.status = "nonfinite-input"; return r; }
    for (std::size_t i = 0; i < anchorTriangles.size(); ++i) {
        if (anchorTriangles[i] != targetTriangles[i]) { r.status = "ordered-topology-mismatch"; return r; }
        for (auto index : anchorTriangles[i]) if (index >= anchor.size()) { r.status = "invalid-index"; return r; }
    }
    double numerator = 0, denominator = 0;
    for (std::size_t i = 0; i < anchor.size(); ++i) for (unsigned k = 0; k < 3; ++k) {
        if (!std::isfinite(anchor[i][k]) || !std::isfinite(target[i][k]) || !std::isfinite(unitDelta[i][k])) {
            r.status = "nonfinite-input"; return r;
        }
        const double d = unitDelta[i][k];
        numerator += (static_cast<double>(target[i][k]) - anchor[i][k]) * d;
        denominator += d * d;
    }
    if (denominator <= 1e-16) { r.status = "zero-basis"; return r; }
    const double factor = numerator / denominator;
    r.rawPosture = anchorPosture + factor;
    double error = 0;
    for (std::size_t i = 0; i < anchor.size(); ++i) for (unsigned k = 0; k < 3; ++k) {
        const double e = static_cast<double>(target[i][k]) - anchor[i][k] - factor * unitDelta[i][k];
        error += e * e;
    }
    r.rmsResidual = std::sqrt(error / static_cast<double>(anchor.size()));
    r.rmsUnitDelta = std::sqrt(denominator / static_cast<double>(anchor.size()));
    r.inUnitInterval = r.rawPosture >= 0 && r.rawPosture <= 1;
    r.status = "candidate-unvalidated"; // Do not clamp, accept, or apply here.
    return r;
}
} // namespace vanity_ube_heel_adapter::foot_posture_fit_core
