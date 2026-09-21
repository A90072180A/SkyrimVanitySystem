#include "../src/TriMorphCore.h"
#include "../src/FootPostureFitCore.h"
#include <iostream>
#include <limits>
#include <random>
#include <cstdlib>
namespace tri = vanity_ube_heel_adapter::tri_morph_core;
namespace fit = vanity_ube_heel_adapter::foot_posture_fit_core;
unsigned checks = 0;
void Check(bool value) { ++checks; if (!value) { std::cerr << "FAILED check " << checks << '\n'; std::exit(1); } }
void U16(std::vector<std::uint8_t>& b, unsigned n) { b.push_back(static_cast<std::uint8_t>(n)); b.push_back(static_cast<std::uint8_t>(n >> 8)); }
void F32(std::vector<std::uint8_t>& b, float n) { auto v = std::bit_cast<std::uint32_t>(n); U16(b, v & 65535); U16(b, v >> 16); }
void Name(std::vector<std::uint8_t>& b, std::string_view n) { b.push_back(static_cast<std::uint8_t>(n.size())); b.insert(b.end(), n.begin(), n.end()); }
std::vector<std::uint8_t> File(std::string shape="Feet", std::string morph="NoHeel", float scale=0.25F, unsigned id=2) {
    std::vector<std::uint8_t> b{'P','I','R','T'}; U16(b,1); Name(b,shape); U16(b,1); Name(b,morph); F32(b,scale); U16(b,1);
    U16(b,id); U16(b,4); U16(b,static_cast<unsigned>(-8)); U16(b,12); return b;
}
int main() {
    const auto valid = File(); const auto parsed = tri::Parse(valid, "Feet", "NoHeel", 3);
    Check(parsed.Present()); Check(parsed.offsets.size()==1); Check(parsed.offsets[0].index==2);
    Check(parsed.offsets[0].delta==std::array<float,3>{1,-2,3});
    Check(tri::Parse(File("Body")).status=="shape-not-found");
    Check(tri::Parse(File("Feet","HiHeelz_CBBE")).status=="morph-absent");
    Check(tri::Parse(File("Feet","NoHeel extra")).status=="morph-absent");
    Check(tri::Parse(valid,"Feet","NoHeel",2).status=="malformed");
    Check(tri::Parse(File("Feet","NoHeel",0)).status=="empty-morph");
    Check(tri::Parse(File("Feet","NoHeel",-1)).status=="malformed");
    Check(tri::Parse(File("Feet","NoHeel",std::numeric_limits<float>::infinity())).status=="malformed");
    Check(tri::Parse(File("Feet","NoHeel",std::numeric_limits<float>::max())).status=="malformed");
    Check(tri::Parse(valid,"Feet","NoHeel",0).status=="invalid-request");
    auto uv = valid; U16(uv,0); Check(tri::Parse(uv).Present());
    uv.push_back(1); Check(tri::Parse(uv).status=="malformed");
    for(std::size_t n=0;n<valid.size();++n) Check(!tri::Parse(std::span(valid).first(n)).Present());
    auto fake=valid; fake[0]='T'; Check(tri::Parse(fake).status=="unsupported-format");
    auto duplicate=valid; const auto countAt=duplicate.size()-10; duplicate[countAt]=2;
    duplicate.insert(duplicate.end(),valid.end()-8,valid.end()); Check(tri::Parse(duplicate).error=="duplicate-morph-index");
    // An alias in the UV section does not count as a position morph.
    auto onlyUV=File("Feet","Other"); U16(onlyUV,1); Name(onlyUV,"Feet"); U16(onlyUV,1); Name(onlyUV,"NoHeel");
    F32(onlyUV,1); U16(onlyUV,1); U16(onlyUV,0); U16(onlyUV,1); U16(onlyUV,1);
    Check(tri::Parse(onlyUV).status=="morph-absent");
    auto both=valid; both.insert(both.end(),onlyUV.begin()+static_cast<std::ptrdiff_t>(File("Feet","Other").size()),onlyUV.end());
    Check(tri::Parse(both).Present());
    std::mt19937 random(54123);
    for (unsigned k=0;k<3000;++k) {
        auto b=valid; b[static_cast<std::size_t>(random()) % b.size()] = static_cast<std::uint8_t>(random());
        const auto result=tri::Parse(b,"Feet","NoHeel",3);
        if (result.Present()) for (const auto& offset:result.offsets) Check(offset.index<3 && std::isfinite(offset.delta[0]));
    }
    std::array<fit::Point,3> anchor{{{0,0,0},{1,0,0},{0,1,0}}};
    std::array<fit::Point,3> delta{{{0,0,1},{0,0,1},{0,0,1}}};
    std::array<fit::Point,3> target{{{0,0,-.5F},{1,0,-.5F},{0,1,-.5F}}};
    std::array<fit::Triangle,1> topology{{{0,1,2}}};
    const auto r=fit::Fit(anchor,target,delta,topology,topology,1);
    Check(r.status=="candidate-unvalidated" && r.rawPosture==.5 && r.rmsResidual==0 && r.inUnitInterval);
    target[0][2]=target[1][2]=target[2][2]=1;
    const auto extra=fit::Fit(anchor,target,delta,topology,topology,1);
    Check(extra.rawPosture==2 && !extra.inUnitInterval);
    auto wrong=topology; wrong[0]={2,1,0};
    Check(fit::Fit(anchor,target,delta,topology,wrong,1).status=="ordered-topology-mismatch");
    std::array<fit::Point,3> zero{}; Check(fit::Fit(anchor,target,zero,topology,topology,1).status=="zero-basis");
    target[0][1]=NAN; Check(fit::Fit(anchor,target,delta,topology,topology,1).status=="nonfinite-input");
    Check(fit::Fit(std::span(anchor).first(2),target,delta,topology,topology,1).status=="size-mismatch");
    std::cout<<checks<<" checks passed (including deterministic parser mutations)\n";
}
