#pragma once
// Model-space, cross-topology measurement. Never changes geometry or clamps a fit.
// Closest triangle projection preserves the reference stocking-to-foot gap.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <string>
#include <vector>
namespace vanity_ube_heel_adapter::surface_posture_core {
using Point = std::array<float, 3>;
using Triangle = std::array<std::uint32_t, 3>;
using Vec = std::array<double, 3>;
inline Vec V(const Point& p) { return {p[0],p[1],p[2]}; }
inline Vec Add(Vec a, Vec b) { return {a[0]+b[0],a[1]+b[1],a[2]+b[2]}; }
inline Vec Sub(Vec a, Vec b) { return {a[0]-b[0],a[1]-b[1],a[2]-b[2]}; }
inline Vec Mul(Vec a, double t) { return {a[0]*t,a[1]*t,a[2]*t}; }
inline double Dot(Vec a, Vec b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
inline double Norm2(Vec a) { return Dot(a,a); }
inline bool Finite(Vec a) { return std::isfinite(a[0])&&std::isfinite(a[1])&&std::isfinite(a[2]); }
struct Projection { Vec point{}, weights{}; double distance2{std::numeric_limits<double>::infinity()}; };
inline Projection Project(Vec p, Vec a, Vec b, Vec c) {
    Projection best;
    // Test each edge and the plane-interior point; also handles degenerate faces.
    auto edge=[&](Vec u,Vec w,int i,int j) {
        const auto e=Sub(w,u);const auto n=Norm2(e);
        const double t=n>0?std::clamp(Dot(Sub(p,u),e)/n,0.,1.):0.;
        const auto q=Add(u,Mul(e,t));const auto d=Norm2(Sub(p,q));
        if(d<best.distance2){best={q,{},d};best.weights[i]=1-t;best.weights[j]=t;}
    };
    edge(a,b,0,1);edge(b,c,1,2);edge(c,a,2,0);
    const auto u=Sub(b,a),v=Sub(c,a),w=Sub(p,a);
    const double uu=Dot(u,u),vv=Dot(v,v),uv=Dot(u,v),uw=Dot(u,w),vw=Dot(v,w);
    const double determinant=uu*vv-uv*uv;
    if(determinant>1e-14*std::max(uu*vv,1e-30)) {
        const double s=(uw*vv-vw*uv)/determinant,t=(vw*uu-uw*uv)/determinant;
        if(s>=0&&t>=0&&s+t<=1) {
            auto q=Add(a,Add(Mul(u,s),Mul(v,t)));auto d=Norm2(Sub(p,q));
            if(d<=best.distance2)best={q,{1-s-t,s,t},d};
        }
    }
    return best;
}
struct Hit { Triangle indices{}; Vec weights{}; double distance2{std::numeric_limits<double>::infinity()}; };
class Surface {
    struct Box {Vec lo{INFINITY,INFINITY,INFINITY},hi{-INFINITY,-INFINITY,-INFINITY};};
    struct Node {Box box;std::size_t begin{},end{},left{},right{};bool leaf{true};};
    std::span<const Point> points;
    std::span<const Triangle> triangles;
    std::vector<std::size_t> order;
    std::vector<Node> nodes;
    Box Bounds(std::size_t begin,std::size_t end) const {
        Box b;for(auto n=begin;n<end;++n)for(auto i:triangles[order[n]])for(unsigned k=0;k<3;++k){b.lo[k]=std::min(b.lo[k],double(points[i][k]));b.hi[k]=std::max(b.hi[k],double(points[i][k]));}return b;
    }
    static double Gap(Vec p,const Box& b){double d=0;for(unsigned k=0;k<3;++k){const auto x=std::max({b.lo[k]-p[k],p[k]-b.hi[k],0.});d+=x*x;}return d;}
    std::size_t Build(std::size_t begin,std::size_t end){
        const auto index=nodes.size();Node node;node.box=Bounds(begin,end);node.begin=begin;node.end=end;nodes.push_back(node);
        if(end-begin>8){unsigned axis=0;for(unsigned k=1;k<3;++k)if(node.box.hi[k]-node.box.lo[k]>node.box.hi[axis]-node.box.lo[axis])axis=k;
            auto mid=begin+(end-begin)/2;
            std::nth_element(order.begin()+begin,order.begin()+mid,order.begin()+end,[&](auto x,auto y){double a=0,b=0;for(auto i:triangles[x])a+=points[i][axis];for(auto i:triangles[y])b+=points[i][axis];return a<b;});
            const auto left=Build(begin,mid),right=Build(mid,end);nodes[index].leaf=false;nodes[index].left=left;nodes[index].right=right;
        }return index;
    }
    void Search(std::size_t index,Vec p,Hit& hit) const {
        const auto& node=nodes[index];if(Gap(p,node.box)>hit.distance2)return;
        if(node.leaf){for(auto n=node.begin;n<node.end;++n){auto t=triangles[order[n]];const auto q=Project(p,V(points[t[0]]),V(points[t[1]]),V(points[t[2]]));if(q.distance2<hit.distance2){hit={t,q.weights,q.distance2};}}return;}
        auto a=node.left,b=node.right;if(Gap(p,nodes[b].box)<Gap(p,nodes[a].box))std::swap(a,b);Search(a,p,hit);Search(b,p,hit);
    }
public:
    bool valid{false};
    Surface(std::span<const Point> p,std::span<const Triangle> t):points(p),triangles(t){
        if(p.empty()||p.size()>65535||t.empty()||t.size()>65535)return;
        for(auto x:p)if(!Finite(V(x)))return;
        for(auto f:t)for(auto i:f)if(i>=p.size())return;
        order.resize(t.size());std::iota(order.begin(),order.end(),0);nodes.reserve(t.size()*2);Build(0,t.size());valid=true;
    }
    Hit Nearest(Vec p) const {Hit h;if(valid&&Finite(p))Search(0,p,h);return h;}
};
struct Correspondence {std::uint32_t donorIndex{};Triangle triangle{};Vec weights{},delta{};double gap{};};
struct Map {std::string status{"not-run"};std::vector<Correspondence> entries;std::size_t eligible{};double coverage{},rmsGap{},maxGap{};};
inline Map BuildMap(std::span<const Point> referenceFoot,std::span<const Triangle> footTriangles,
                    std::span<const Point> referenceStocking,std::span<const Point> noHeelDelta,
                    double maxGap=1.5,double minimumDelta=0.01,std::size_t minimumCount=256) {
    Map out;
    if(referenceStocking.empty()||referenceStocking.size()!=noHeelDelta.size()||referenceStocking.size()>65535||
        !std::isfinite(maxGap)||maxGap<=0||!std::isfinite(minimumDelta)||minimumDelta<=0||!minimumCount){out.status="invalid-map-input";return out;}
    Surface surface(referenceFoot,footTriangles);if(!surface.valid){out.status="invalid-reference-surface";return out;}
    double gap2=0;
    for(std::size_t i=0;i<referenceStocking.size();++i){
        const auto p=V(referenceStocking[i]),delta=V(noHeelDelta[i]);
        if(!Finite(p)||!Finite(delta)){out.status="nonfinite-donor";out.entries.clear();return out;}
        if(Norm2(delta)<minimumDelta*minimumDelta)continue;
        ++out.eligible;const auto hit=surface.Nearest(p);if(!std::isfinite(hit.distance2)||hit.distance2>maxGap*maxGap)continue;
        const double gap=std::sqrt(hit.distance2);gap2+=hit.distance2;out.maxGap=std::max(out.maxGap,gap);
        out.entries.push_back({static_cast<std::uint32_t>(i),hit.indices,hit.weights,delta,gap});
    }
    out.coverage=out.eligible?double(out.entries.size())/double(out.eligible):0.;
    out.rmsGap=out.entries.empty()?0.:std::sqrt(gap2/double(out.entries.size()));
    out.status=out.entries.size()<minimumCount?"insufficient-correspondences":out.coverage<0.85?"insufficient-coverage":"mapped";
    return out;
}
struct Fit {std::string status{"not-run"};double rawNoHeel{},rmsResidual{},maxResidual{},basisNormalizedResidual{},motionRelativeResidual{},targetMotionRms{},basisRms{};bool hasMotion{false};};
inline Fit FitTarget(const Map& map,std::span<const Point> anchor,std::span<const Triangle> anchorTriangles,
                     std::span<const Point> target,std::span<const Triangle> targetTriangles,double referenceNoHeel) {
    Fit out;
    if(map.status!="mapped"||map.entries.empty()||!std::isfinite(referenceNoHeel)){out.status="invalid-reference-map";return out;}
    if(anchor.size()!=target.size()||anchorTriangles.size()!=targetTriangles.size()||
       !std::equal(anchorTriangles.begin(),anchorTriangles.end(),targetTriangles.begin())){out.status="target-topology-mismatch";return out;}
    for(auto x:anchor)if(!Finite(V(x))){out.status="nonfinite-foot";return out;}
    for(auto x:target)if(!Finite(V(x))){out.status="nonfinite-foot";return out;}
    double numerator=0,denominator=0,motion2=0;
    std::vector<Vec> changes;changes.reserve(map.entries.size());
    for(const auto& m:map.entries){Vec d{};double sum=0;
        if(!Finite(m.delta)||!Finite(m.weights)){out.status="invalid-correspondence";return out;}
        for(unsigned k=0;k<3;++k){if(m.triangle[k]>=anchor.size()||m.weights[k]<-1e-8||m.weights[k]>1+1e-8){out.status="invalid-correspondence";return out;}sum+=m.weights[k];d=Add(d,Mul(Sub(V(target[m.triangle[k]]),V(anchor[m.triangle[k]])),m.weights[k]));}
        if(std::abs(sum-1)>1e-7){out.status="invalid-correspondence";return out;}
        changes.push_back(d);numerator+=Dot(m.delta,d);denominator+=Norm2(m.delta);motion2+=Norm2(d);
    }
    if(denominator<=1e-20||!std::isfinite(denominator)||!std::isfinite(numerator)){out.status="singular-donor";return out;}
    const auto shift=numerator/denominator;out.rawNoHeel=referenceNoHeel+shift;
    double residual2=0;for(std::size_t i=0;i<changes.size();++i){const auto d=Norm2(Sub(changes[i],Mul(map.entries[i].delta,shift)));residual2+=d;out.maxResidual=std::max(out.maxResidual,std::sqrt(d));}
    out.rmsResidual=std::sqrt(residual2/double(changes.size()));out.basisRms=std::sqrt(denominator/double(changes.size()));out.targetMotionRms=std::sqrt(motion2/double(changes.size()));
    out.basisNormalizedResidual=std::sqrt(residual2/denominator);out.hasMotion=motion2>1e-20;out.motionRelativeResidual=out.hasMotion?std::sqrt(residual2/motion2):0.;
    out.status="diagnostic-fit";return out;
}
inline std::string Recommendation(const Fit& fit,double maxBasisResidual=0.15) {
    if(fit.status!="diagnostic-fit")return fit.status;
    if(!std::isfinite(fit.rawNoHeel)||!std::isfinite(fit.basisNormalizedResidual))return "invalid-fit";
    if(fit.rawNoHeel<0||fit.rawNoHeel>1)return "outside-configured-0-1-range";
    if(fit.basisNormalizedResidual>maxBasisResidual)return "residual-too-large";
    return "candidate-needs-visual-validation"; // Never authorization to apply.
}
} // namespace vanity_ube_heel_adapter::surface_posture_core
