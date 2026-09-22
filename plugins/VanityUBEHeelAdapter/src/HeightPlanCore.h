#pragma once
// One physical control direction at a time. The two measured deltas are never
// replaced with a universal scale/sign alias. No geometry is written here.
#include "SurfacePostureCore.h"
#include <optional>
namespace vanity_ube_heel_adapter::height_plan_core {
namespace s = surface_posture_core;
struct Controls { double noHeel{}, heel{}; };
inline bool Valid(Controls c) {
    return std::isfinite(c.noHeel) && std::isfinite(c.heel) &&
        c.noHeel >= 0 && c.noHeel <= 1 && c.heel >= 0 && c.heel <= 1 &&
        !(c.noHeel > 0 && c.heel > 0);
}
inline std::optional<Controls> FromSigned(double value) {
    if (!std::isfinite(value) || value < -1 || value > 1) return {};
    return value >= 0 ? Controls{value, 0} : Controls{0, -value};
}
struct Branch {
    bool available{};
    double raw{}, bounded{}, rms{}, maximum{}, squaredError{};
};
struct Result {
    std::string status{"not-run"};
    Controls controls;
    Branch noHeel, heel;
    bool saturated{};
    double rms{}, maximum{}, normalizedResidual{}, normalizationRms{};
};
inline Branch FitBranch(std::span<const s::Vec> desired, std::span<const s::Vec> delta) {
    Branch out;
    if(desired.empty()||desired.size()!=delta.size()||desired.size()>65535)return out;
    double den=0, num=0;
    for (std::size_t i=0;i<desired.size();++i) { den+=s::Norm2(delta[i]); num+=s::Dot(delta[i],desired[i]); }
    if (den <= 1e-18 || !std::isfinite(den) || !std::isfinite(num)) return out;
    out.available=true; out.raw=num/den; out.bounded=std::clamp(out.raw,0.,1.);
    for (std::size_t i=0;i<desired.size();++i) {
        const double e=s::Norm2(s::Sub(desired[i],s::Mul(delta[i],out.bounded)));
        out.squaredError+=e; out.maximum=(std::max)(out.maximum,std::sqrt(e));
    }
    out.rms=std::sqrt(out.squaredError/double(desired.size()));
    return out;
}
inline Result Solve(std::span<const s::Vec> desired, std::span<const s::Vec> noHeel,
                    std::span<const s::Vec> heel) {
    Result out;
    if (desired.empty() || desired.size()>65535 || desired.size()!=noHeel.size() || desired.size()!=heel.size()) {
        out.status="invalid-height-input"; return out;
    }
    double normN=0,normH=0;
    for (std::size_t i=0;i<desired.size();++i) {
        if (!s::Finite(desired[i]) || !s::Finite(noHeel[i]) || !s::Finite(heel[i])) { out.status="nonfinite-height-input"; return out; }
        normN+=s::Norm2(noHeel[i]); normH+=s::Norm2(heel[i]);
    }
    out.noHeel=FitBranch(desired,noHeel); out.heel=FitBranch(desired,heel);
    if (!out.noHeel.available && !out.heel.available) { out.status="no-usable-height-morph"; return out; }
    // Ties prefer NoHeel; this includes the shared origin and avoids jitter.
    const bool useHeel=out.heel.available && (!out.noHeel.available ||
        out.heel.squaredError+1e-12 < out.noHeel.squaredError);
    const auto& chosen=useHeel ? out.heel : out.noHeel;
    out.controls=useHeel ? Controls{0,chosen.bounded} : Controls{chosen.bounded,0};
    out.saturated=chosen.raw>1.+1e-6;
    out.rms=chosen.rms; out.maximum=chosen.maximum;
    // Preserve the preceding single-NoHeel policy's normalization when present.
    out.normalizationRms=std::sqrt((normN>1e-18?normN:normH)/double(desired.size()));
    out.normalizedResidual=out.rms/out.normalizationRms;
    out.status="bounded-height-fit";
    return out;
}
inline Result FitSurface(const s::Map& map, std::span<const s::Point> anchor,
    std::span<const s::Triangle> at, std::span<const s::Point> target,
    std::span<const s::Triangle> tt, std::span<const s::Point> noHeel,
    std::span<const s::Point> heel, double referenceNoHeel) {
    Result bad;
    if (map.status!="mapped" || anchor.size()!=target.size() || at.size()!=tt.size() ||
        !std::equal(at.begin(),at.end(),tt.begin()) || !std::isfinite(referenceNoHeel) ||
        referenceNoHeel<0 || referenceNoHeel>1 || noHeel.size()!=heel.size()) {
        bad.status="height-correspondence-unverified"; return bad;
    }
    for(auto p:anchor) if(!s::Finite(s::V(p))) {bad.status="nonfinite-foot";return bad;}
    for(auto p:target) if(!s::Finite(s::V(p))) {bad.status="nonfinite-foot";return bad;}
    std::vector<s::Vec> desired,ns,hs;
    for (const auto& m:map.entries) {
        if (m.donorIndex>=noHeel.size() || !s::Finite(m.weights)) {bad.status="invalid-correspondence";return bad;}
        s::Vec move{};double sum=0;
        for(unsigned k=0;k<3;++k) {
            if(m.triangle[k]>=anchor.size() || m.weights[k]<-1e-8 || m.weights[k]>1+1e-8){bad.status="invalid-correspondence";return bad;}
            sum+=m.weights[k];
            move=s::Add(move,s::Mul(s::Sub(s::V(target[m.triangle[k]]),s::V(anchor[m.triangle[k]])),m.weights[k]));
        }
        if(std::abs(sum-1)>1e-7){bad.status="invalid-correspondence";return bad;}
        const auto n=s::V(noHeel[m.donorIndex]),h=s::V(heel[m.donorIndex]);
        ns.push_back(n);hs.push_back(h);desired.push_back(s::Add(move,s::Mul(n,referenceNoHeel)));
    }
    return Solve(desired,ns,hs);
}
inline std::string Decision(const Result& fit, double maximumResidual=.15,
                            bool allowEndpointApproximation=false) {
    if (fit.status!="bounded-height-fit") return fit.status;
    if (!Valid(fit.controls) || !std::isfinite(fit.normalizedResidual) ||
        !std::isfinite(maximumResidual) || maximumResidual<0) return "invalid-height-policy";
    if (fit.normalizedResidual>maximumResidual) return "height-residual-too-large";
    if (fit.saturated && !allowEndpointApproximation) return "height-range-exceeded";
    return fit.saturated ? "accepted-endpoint-approximation" : "accepted-measured-height";
}
} // namespace vanity_ube_heel_adapter::height_plan_core
