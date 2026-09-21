#pragma once
// Distinguish pose from cloth clearance. Read-only; not a cloth/collision solver.
#include "SurfacePostureCore.h"
#include <optional>
#include <queue>
namespace vanity_ube_heel_adapter::stocking_clearance_core {
namespace s = surface_posture_core;
using s::Vec; using s::Point; using s::Triangle;
inline Vec Cross(Vec a, Vec b) { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
struct Frame { Vec tangent{}, bitangent{}, normal{}; };
inline std::optional<Frame> TriangleFrame(Vec a, Vec b, Vec c) {
    const auto e=s::Sub(b,a),f=s::Sub(c,a),n=Cross(e,f);
    const auto ee=s::Norm2(e),ff=s::Norm2(f),nn=s::Norm2(n);
    if(!s::Finite(e)||!s::Finite(f)||ee<1e-20||ff<1e-20||nn<=1e-12*ee*ff) return {};
    Frame r; r.tangent=s::Mul(e,1/std::sqrt(ee)); r.normal=s::Mul(n,1/std::sqrt(nn));
    r.bitangent=Cross(r.normal,r.tangent); return r;
}
inline Vec RotateGap(Vec gap,const Frame& from,const Frame& to) {
    return s::Add(s::Add(s::Mul(to.tangent,s::Dot(gap,from.tangent)),
                         s::Mul(to.bitangent,s::Dot(gap,from.bitangent))),
                         s::Mul(to.normal,s::Dot(gap,from.normal)));
}
inline double Quantile(std::vector<double> v,double p) {
    if(v.empty()) return 0;
    std::sort(v.begin(),v.end()); const double index=std::clamp(p,0.,1.)*double(v.size()-1);
    const auto lo=std::size_t(index),hi=(std::min)(lo+1,v.size()-1);
    return v[lo]+(index-double(lo))*(v[hi]-v[lo]);
}
struct Clearance {
    std::string status{"not-run"}; std::size_t count{},degenerate{},negativeSide{};
    double rmsGap{},medianGap{},p95Gap{},maxGap{},rmsNormalGap{},rmsTangentialGap{},meanSignedNormal{};
};
inline Clearance Measure(const s::Map& map,std::span<const Point> foot,std::span<const Point> stocking) {
    Clearance r;
    if(map.status!="mapped"||map.entries.empty()){r.status="invalid-reference-map";return r;}
    double total=0,normal2=0,tangent2=0,signedSum=0;std::vector<double> distances;
    for(const auto&m:map.entries){
        if(m.donorIndex>=stocking.size()||!s::Finite(s::V(stocking[m.donorIndex]))){r.status="invalid-donor";return r;}
        Vec p{};double sum=0;
        for(unsigned k=0;k<3;++k){
            if(m.triangle[k]>=foot.size()||!s::Finite(s::V(foot[m.triangle[k]]))||!std::isfinite(m.weights[k])||m.weights[k]<-1e-8||m.weights[k]>1+1e-8){r.status="invalid-correspondence";return r;}
            p=s::Add(p,s::Mul(s::V(foot[m.triangle[k]]),m.weights[k]));sum+=m.weights[k];
        }
        if(std::abs(sum-1)>1e-7){r.status="invalid-correspondence";return r;}
        auto frame=TriangleFrame(s::V(foot[m.triangle[0]]),s::V(foot[m.triangle[1]]),s::V(foot[m.triangle[2]]));
        if(!frame){++r.degenerate;continue;}
        const auto g=s::Sub(s::V(stocking[m.donorIndex]),p);const auto g2=s::Norm2(g),n=s::Dot(g,frame->normal);
        total+=g2;normal2+=n*n;tangent2+=(std::max)(0.,g2-n*n);signedSum+=n;
        distances.push_back(std::sqrt(g2));r.negativeSide+=n<0;++r.count;
    }
    if(!r.count){r.status="no-valid-triangle-frames";return r;}
    r.rmsGap=std::sqrt(total/r.count);r.medianGap=Quantile(distances,.5);r.p95Gap=Quantile(distances,.95);
    r.maxGap=*std::max_element(distances.begin(),distances.end());r.rmsNormalGap=std::sqrt(normal2/r.count);
    r.rmsTangentialGap=std::sqrt(tangent2/r.count);r.meanSignedNormal=signedSum/r.count;
    r.status=r.degenerate?"partial-degenerate-frames":"measured";return r;
}
struct TransportResult {
    std::string status{"not-run"};s::Fit fixedGap,rotatingGap;
    double gapRotationRms{},rawCoefficientSensitivity{};
};
inline TransportResult Compare(const s::Map& map,std::span<const Point> anchor,std::span<const Triangle> anchorTriangles,
                               std::span<const Point> target,std::span<const Triangle> targetTriangles,
                               std::span<const Point> stocking,double referenceNoHeel) {
    TransportResult out;out.fixedGap=s::FitTarget(map,anchor,anchorTriangles,target,targetTriangles,referenceNoHeel);
    if(out.fixedGap.status!="diagnostic-fit"){out.status=out.fixedGap.status;return out;}
    std::vector<Vec> changes;double numerator=0,denominator=0,motion2=0,rotation2=0;
    for(const auto&m:map.entries){
        if(m.donorIndex>=stocking.size()||!s::Finite(s::V(stocking[m.donorIndex]))){out.status="invalid-donor";return out;}
        auto from=TriangleFrame(s::V(anchor[m.triangle[0]]),s::V(anchor[m.triangle[1]]),s::V(anchor[m.triangle[2]]));
        auto to=TriangleFrame(s::V(target[m.triangle[0]]),s::V(target[m.triangle[1]]),s::V(target[m.triangle[2]]));
        if(!from||!to){out.status="degenerate-triangle-frame";return out;}
        Vec a{},b{};
        for(unsigned k=0;k<3;++k){a=s::Add(a,s::Mul(s::V(anchor[m.triangle[k]]),m.weights[k]));b=s::Add(b,s::Mul(s::V(target[m.triangle[k]]),m.weights[k]));}
        const auto gap=s::Sub(s::V(stocking[m.donorIndex]),a);
        const auto correction=s::Sub(RotateGap(gap,*from,*to),gap);
        const auto change=s::Add(s::Sub(b,a),correction);
        changes.push_back(change);numerator+=s::Dot(m.delta,change);denominator+=s::Norm2(m.delta);
        motion2+=s::Norm2(change);rotation2+=s::Norm2(correction);
    }
    if(denominator<=1e-20||!std::isfinite(numerator)||!std::isfinite(motion2)){out.status="invalid-transport-fit";return out;}
    auto& f=out.rotatingGap;const auto shift=numerator/denominator;f.rawNoHeel=referenceNoHeel+shift;
    double residual2=0;
    for(std::size_t i=0;i<changes.size();++i){const auto e2=s::Norm2(s::Sub(changes[i],s::Mul(map.entries[i].delta,shift)));residual2+=e2;f.maxResidual=(std::max)(f.maxResidual,std::sqrt(e2));}
    f.rmsResidual=std::sqrt(residual2/changes.size());f.basisRms=std::sqrt(denominator/changes.size());f.targetMotionRms=std::sqrt(motion2/changes.size());
    f.basisNormalizedResidual=std::sqrt(residual2/denominator);f.hasMotion=motion2>1e-20;f.motionRelativeResidual=f.hasMotion?std::sqrt(residual2/motion2):0;
    f.status="diagnostic-fit";out.gapRotationRms=std::sqrt(rotation2/changes.size());out.rawCoefficientSensitivity=std::abs(f.rawNoHeel-out.fixedGap.rawNoHeel);
    out.status="compared-not-applied";return out;
}

// A bounded, foot-region-only tightening PROPOSAL. Positive-normal-side only;
// triangle winding is not a certified inside/outside test. Never applied here.
struct Correction { std::uint32_t index{}; Vec displacement{}; };
struct Tightening {
    std::string status{"not-run"};std::vector<Correction> corrections;
    double maxDisplacement{},rmsDisplacement{};
};
inline Tightening Propose(const s::Map& map,std::span<const Point> foot,
                          std::span<const Point> stocking,std::span<const Triangle> triangles,
                          double clearance=0.15,double maxStep=0.25,unsigned taperRings=3) {
    Tightening out;
    if(map.status!="mapped"||stocking.empty()||stocking.size()>65535||triangles.empty()||triangles.size()>65535||
       !std::isfinite(clearance)||clearance<=0||!std::isfinite(maxStep)||maxStep<=0||taperRings<1||taperRings>16){out.status="invalid-proposal-input";return out;}
    const auto gaps=Measure(map,foot,stocking);
    if(gaps.status!="measured"){out.status="uncertain-reference-frames";return out;}
    for(auto p:stocking)if(!s::Finite(s::V(p))){out.status="invalid-donor";return out;}
    std::vector<std::vector<std::uint32_t>> adjacent(stocking.size());
    for(const auto&t:triangles){for(auto i:t)if(i>=stocking.size()){out.status="invalid-donor-triangles";return out;}
        for(unsigned k=0;k<3;++k){auto a=t[k],b=t[(k+1)%3];adjacent[a].push_back(b);adjacent[b].push_back(a);}}
    std::vector<bool> eligible(stocking.size(),false);std::vector<Vec> proposed(stocking.size());
    for(const auto&m:map.entries){
        auto frame=TriangleFrame(s::V(foot[m.triangle[0]]),s::V(foot[m.triangle[1]]),s::V(foot[m.triangle[2]]));
        Vec closest{};for(unsigned k=0;k<3;++k)closest=s::Add(closest,s::Mul(s::V(foot[m.triangle[k]]),m.weights[k]));
        const auto gap=s::Sub(s::V(stocking[m.donorIndex]),closest);const double normal=s::Dot(gap,frame->normal);
        const double tangential=std::sqrt((std::max)(0.,s::Norm2(gap)-normal*normal));
        if(normal<=clearance||tangential>0.25*normal+0.01)continue;
        eligible[m.donorIndex]=true;
        proposed[m.donorIndex]=s::Mul(frame->normal,-(std::min)(maxStep,normal-clearance));
    }
    std::vector<unsigned> distance(stocking.size(),taperRings);std::queue<std::uint32_t> pending;
    for(std::size_t i=0;i<stocking.size();++i)if(eligible[i]){
        if(adjacent[i].empty()||std::any_of(adjacent[i].begin(),adjacent[i].end(),[&](auto j){return !eligible[j];})){
            distance[i]=0;pending.push(static_cast<std::uint32_t>(i));}}
    while(!pending.empty()){const auto i=pending.front();pending.pop();
        for(auto j:adjacent[i])if(eligible[j]&&distance[j]>distance[i]+1){distance[j]=distance[i]+1;pending.push(j);}}
    std::vector<Vec> corrected;corrected.reserve(stocking.size());for(auto p:stocking)corrected.push_back(s::V(p));
    double sum2=0;
    for(std::size_t i=0;i<stocking.size();++i)if(eligible[i]){
        const double x=double(distance[i])/taperRings,w=x*x*(3-2*x);const auto movement=s::Mul(proposed[i],w);
        const auto length=std::sqrt(s::Norm2(movement));if(length<=1e-10)continue;
        out.corrections.push_back({static_cast<std::uint32_t>(i),movement});corrected[i]=s::Add(corrected[i],movement);
        out.maxDisplacement=(std::max)(out.maxDisplacement,length);sum2+=length*length;
    }
    for(const auto&t:triangles){const auto before=Cross(s::Sub(s::V(stocking[t[1]]),s::V(stocking[t[0]])),s::Sub(s::V(stocking[t[2]]),s::V(stocking[t[0]])));
        const auto after=Cross(s::Sub(corrected[t[1]],corrected[t[0]]),s::Sub(corrected[t[2]],corrected[t[0]]));
        if(s::Norm2(before)>1e-16&&(s::Norm2(after)<1e-16||s::Dot(before,after)<=0)){
            out.status="proposal-triangle-flip";out.corrections.clear();return out;}}
    if(!out.corrections.empty())out.rmsDisplacement=std::sqrt(sum2/out.corrections.size());
    out.status=out.corrections.empty()?"no-safe-correction":"preview-only-not-installable";return out;
}
} // namespace vanity_ube_heel_adapter::stocking_clearance_core
