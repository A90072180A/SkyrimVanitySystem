#include "StockingSurfaceCalibration.h"
#include "SurfacePostureCore.h"
#include "NifSourceCore.h"
#include "FootCapturePolicy.h"
#include "TriMorphCore.h"
#include <Windows.h>
#include <fstream>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <unordered_set>

namespace vanity_ube_heel_adapter::stocking_surface_calibration {
namespace {
using Json = nlohmann::json;
namespace surface = surface_posture_core;
namespace nif = nif_source_core;
using Point = surface::Point;
using Triangle = surface::Triangle;
using ReadMorph = source_geometry_evidence::ReadMorph;
constexpr auto kOutput = "Data/SKSE/Plugins/VanityUBEHeelAdapter/calibration-candidates.json";
struct Asset { nif::Result data; std::string fingerprint, resource; std::uintmax_t size{}; std::filesystem::file_time_type time{}; };
// All maps below are writer-owned, bounded, session-only. Persisted reports are
// never loaded as authority and never used by the runtime morph resolver.
std::map<std::string, std::shared_ptr<const Asset>> assets;
std::map<std::string, Json> results;
Json Configuration() {
    std::ifstream file("Data/SKSE/Plugins/VanityUBEHeelAdapter.json");
    if (!file) return Json::object();
    auto config = Json::parse(file);
    return config.is_object() ? config : Json::object();
}
std::shared_ptr<const Asset> ReadNif(const std::string& resource) {
    const auto key = foot_capture_policy::ResourceKey(resource);
    if (key.empty() || !key.ends_with(".nif")) return {};
    const auto path = std::filesystem::path("Data/Meshes") / key;
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > 64u * 1024u * 1024u) return {};
    const auto time = std::filesystem::last_write_time(path, ec);
    if (ec) return {};
    const auto found = assets.find(key);
    if (found != assets.end() && found->second->size == size && found->second->time == time) return found->second;
    std::ifstream file(path, std::ios::binary);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!file || !file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) return {};
    const auto afterSize = std::filesystem::file_size(path, ec);
    if (ec || afterSize != size) return {};
    const auto afterTime = std::filesystem::last_write_time(path, ec);
    if (ec || afterTime != time) return {};
    auto asset = std::make_shared<Asset>();
    asset->data = nif::Parse(bytes);
    if (!asset->data.Complete()) return {};
    asset->size = size; asset->time = time; asset->resource = resource;
    asset->fingerprint = std::format("{:016x}", tri_morph_core::HashBytes(bytes));
    if (assets.size() >= 8) assets.clear();
    assets[key] = asset;
    return asset;
}
Json Transform(const nif::Transform& t) {
    return {{"rotation", {{t.rotation[0],t.rotation[1],t.rotation[2]},
                           {t.rotation[3],t.rotation[4],t.rotation[5]},
                           {t.rotation[6],t.rotation[7],t.rotation[8]}}},
            {"translation", t.translation}, {"scale", t.scale}};
}
bool SameTransform(const Json& a, const Json& b) {
    if (!a.is_object() || !b.is_object()) return false;
    try {
        std::vector<double> x, y;
        for (const auto& row : a.at("rotation")) for (const auto& v : row) x.push_back(v.get<double>());
        for (const auto& row : b.at("rotation")) for (const auto& v : row) y.push_back(v.get<double>());
        for (const auto& v : a.at("translation")) x.push_back(v.get<double>());
        for (const auto& v : b.at("translation")) y.push_back(v.get<double>());
        x.push_back(a.at("scale").get<double>()); y.push_back(b.at("scale").get<double>());
        if (x.size() != 13 || y.size() != 13) return false;
        for (std::size_t i = 0; i < x.size(); ++i)
            if (!std::isfinite(x[i]) || !std::isfinite(y[i]) || std::abs(x[i]-y[i]) > 1e-5) return false;
        return true;
    } catch (...) { return false; }
}
struct Sample {
    std::vector<Point> positions, delta;
    std::vector<Triangle> triangles;
    Json identity, source, local, skin, localFit;
    std::string context, captureKey, fingerprint;
};
std::optional<Sample> CheckedSample(const Json& d) {
    if (d.at("extraction").value("status", std::string{}) != "complete" ||
        !d.at("sourceModelEvidence").value("measurementEligible", false) ||
        !d.at("captureSelection").value("currentGraphConfirmed", false) ||
        d.at("captureSelection").value("identityIsCorrelated", true)) return {};
    Sample s;
    s.identity = d.at("identity"); s.source = d.at("sourceModelEvidence");
    s.local = d.at("localTransform"); s.skin = d.at("rootParentToSkin");
    if (!s.local.is_object() || !s.skin.is_object() || s.identity.value("buffered", true)) return {};
    if (!d.at("actorMorphValues").is_array()) return {};
    s.positions = d.at("positions").get<std::vector<Point>>();
    s.triangles = d.at("triangles").get<std::vector<Triangle>>();
    if (s.positions.empty() || s.positions.size()>65535 || s.triangles.empty() || s.triangles.size()>65535) return {};
    for (auto p : s.positions) if (!surface::Finite(surface::V(p))) return {};
    for (auto t : s.triangles) for (auto i : t) if (i >= s.positions.size()) return {};
    // Compare full context values rather than treating a short diagnostic hash
    // as proof of equality. No free spatial alignment or animation coordinates.
    s.context = Json::array({s.identity.at("race"), s.identity.at("sexIndex"),
                            s.identity.at("actorWeight"), d.at("actorMorphValues")}).dump();
    s.captureKey = d.value("captureKey", std::string{});
    s.fingerprint = s.source.at("asset").value("sourceFingerprint", std::string{});
    return s;
}
const nif::Shape* UniqueShape(const Asset& a, const Sample& sample) {
    const nif::Shape* result = nullptr;
    const auto name = sample.source.at("sourceGeometryName").get<std::string>();
    for (const auto& shape : a.data.shapes) {
        if (!shape.mesh.Complete() || shape.name != name ||
            shape.mesh.positions.size() != sample.positions.size() || shape.mesh.triangles != sample.triangles ||
            !SameTransform(sample.local, Transform(shape.local)) || !SameTransform(sample.skin, Transform(shape.skin))) continue;
        if (result) return nullptr;
        result = &shape;
    }
    return result;
}
std::optional<std::vector<Point>> Dense(const Json& measurement, std::size_t n) {
    const auto status = measurement.value("status", std::string{});
    if (status != "present" && status != "empty-morph") return {};
    std::vector<Point> values(n); std::unordered_set<unsigned> seen;
    for (const auto& offset : measurement.at("offsets")) {
        const auto i = offset.at("index").get<unsigned>();
        if (i >= n || !seen.insert(i).second) return {};
        values[i] = offset.at("delta").get<Point>();
        if (!surface::Finite(surface::V(values[i]))) return {};
    }
    return values;
}
bool ReconstructDonor(Sample& s, Json& d, const ReadMorph& readMorph, double referenceValue) {
    Json info = {{"status","not-run"}, {"automaticApplicationAllowed",false},
                 {"semantics","runtime NoHeel coefficient relative to the winning built NIF plus this stocking's own actor morph responses"}};
    auto finish = [&](const char* status) { info["status"] = status; d["stockingLocalCalibration"] = info; return false; };
    auto path = foot_capture_policy::ResourceKey(s.identity.at("armaModel").get<std::string>());
    if (!(path.ends_with("_0.nif") || path.ends_with("_1.nif"))) return finish("weight-pair-path-not-supported");
    auto path0 = path, path1 = path;
    path0[path0.size()-5] = '0'; path1[path1.size()-5] = '1';
    const auto lo = ReadNif(path0), hi = ReadNif(path1);
    if (!lo || !hi) return finish("stocking-weight-pair-unavailable");
    const auto* l = UniqueShape(*lo,s); const auto* h = UniqueShape(*hi,s);
    if (!l || !h) return finish("stocking-source-pair-mismatch");
    if (l->bodyTris.size()!=1 || l->bodyTris != h->bodyTris) return finish("stocking-bodytri-ambiguous");
    const auto n=static_cast<std::uint32_t>(s.positions.size());
    auto noHeel=readMorph(l->bodyTris[0],l->name,"NoHeel",n);
    if (noHeel.value("status",std::string{})!="present") return finish("NoHeel-unavailable");
    auto deltas=Dense(noHeel,n); if(!deltas) return finish("invalid-NoHeel-record");
    const double weight=s.identity.at("actorWeight").get<double>()/100.;
    if(!std::isfinite(weight)||weight<0||weight>1)return finish("invalid-actor-weight");
    std::vector<surface::Vec> baseline(n);
    for(unsigned i=0;i<n;++i)baseline[i]=surface::Add(surface::V(l->mesh.positions[i]),
        surface::Mul(surface::Sub(surface::V(h->mesh.positions[i]),surface::V(l->mesh.positions[i])),weight));
    std::unordered_set<std::string> available;
    for(const auto& name:noHeel.at("morphNamesForShape"))available.insert(name.get<std::string>());
    std::map<std::string,double> values;
    for(const auto& row:d.at("actorMorphValues")) {
        const auto name=row.at("name").get<std::string>();const auto value=row.at("value").get<double>();
        if(!std::isfinite(value))return finish("invalid-actor-morph-value");
        if(value==0 || name=="NoHeel" || !available.contains(name))continue;
        if(!values.emplace(name,value).second)return finish("multiple-effective-morph-keys-unsupported");
    }
    for(const auto& [name,value]:values) {
        const auto m=readMorph(l->bodyTris[0],l->name,name,n);const auto delta=Dense(m,n);
        if(!delta)return finish("stocking-own-body-morph-unavailable");
        for(unsigned i=0;i<n;++i)baseline[i]=surface::Add(baseline[i],surface::Mul(surface::V((*delta)[i]),value));
    }
    double numerator=0,denominator=0;
    for(unsigned i=0;i<n;++i){const auto v=surface::V((*deltas)[i]);denominator+=surface::Norm2(v);numerator+=surface::Dot(v,surface::Sub(surface::V(s.positions[i]),baseline[i]));}
    if(denominator<1e-12)return finish("singular-NoHeel-basis");
    const double q=numerator/denominator;
    double sum2=0,active2=0,maxError=0;unsigned activeCount=0;
    for(unsigned i=0;i<n;++i){const auto r=surface::Sub(surface::Sub(surface::V(s.positions[i]),baseline[i]),surface::Mul(surface::V((*deltas)[i]),q));const auto err=surface::Norm2(r);sum2+=err;maxError=std::max(maxError,std::sqrt(err));if(surface::Norm2(surface::V((*deltas)[i]))>=1e-4){active2+=err;++activeCount;}}
    if(!activeCount||!std::isfinite(q)||!std::isfinite(sum2))return finish("invalid-local-fit");
    const double rms=std::sqrt(sum2/n),activeRms=std::sqrt(active2/activeCount);
    info["rawObservedNoHeel"]=q;info["allVertexRms"]=rms;info["affectedVertexRms"]=activeRms;info["maxVertexResidual"]=maxError;
    info["sourceFiles"]=Json::array({{{"resource",path0},{"fingerprint",lo->fingerprint}},{{"resource",path1},{"fingerprint",hi->fingerprint}}});
    info["NoHeelSourceFingerprint"]=noHeel.at("sourceFingerprint");info["ownBodyMorphCorrectionCount"]=values.size();info["referenceNoHeel"]=referenceValue;
    // Refuse a baseline that does not explain the observed mesh. This catches
    // unknown local sliders, changed builds, missing morphs and unmodelled edits.
    if(rms>0.05 || activeRms>0.03 || maxError>0.3)return finish("stocking-baseline-residual-too-large");
    s.delta=std::move(*deltas);
    for(unsigned i=0;i<n;++i){const auto v=surface::Add(baseline[i],surface::Mul(surface::V(s.delta[i]),referenceValue));for(unsigned k=0;k<3;++k)s.positions[i][k]=static_cast<float>(v[k]);}
    info["status"]="source-reconstructed-reference";
    d["stockingLocalCalibration"]=info;s.localFit=info;
    logger::info("[stocking local calibration] armor='{}' observedNoHeel={:.6f} affectedRms={:.6f} ownBodyMorphs={} autoApply=false",s.identity.at("armor").get<std::string>(),q,activeRms,values.size());
    return true;
}
struct Context {
    std::optional<Sample> anchor;
    std::map<std::string,Sample> feet,donors;
};
std::map<std::string,Context> contexts;
std::string PairID(const Sample& s) { return Json::array({s.identity.at("armor"),s.identity.at("addon"),s.identity.at("armaModel"),s.fingerprint}).dump(); }
void Persist() {
    Json out={{"schema",1},{"generatorVersion","0.13.0"},{"status","diagnostic-only"},
              {"automaticApplicationAllowed",false},{"entries",Json::array()},
              {"semantics","reference-gap-preserving surface correspondence; no NoHeel/HiHeelz alias; no extrapolation applied"}};
    for(const auto& [key,result]:results)out["entries"].push_back(result);
    const std::filesystem::path path(kOutput), temp(std::string(kOutput)+".tmp");
    std::error_code ec;std::filesystem::create_directories(path.parent_path(),ec);if(ec)throw std::runtime_error(ec.message());
    std::ofstream file(temp,std::ios::binary|std::ios::trunc);
    if(!file)throw std::runtime_error("cannot create calibration report");
    file<<out.dump(2)<<'\n';file.flush();if(!file)throw std::runtime_error("cannot flush calibration report");file.close();if(file.fail())throw std::runtime_error("cannot close calibration report");
    if(!::MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error(std::format("calibration report replace failed: {}",::GetLastError()));
}
void Evaluate(Context& group,double referenceValue) {
    if(!group.anchor)return;
    const auto& anchor=*group.anchor;
    for(const auto& [donorID,donor]:group.donors) {
        if(!SameTransform(anchor.local,donor.local)||!SameTransform(anchor.skin,donor.skin))continue;
        const auto map=surface::BuildMap(anchor.positions,anchor.triangles,donor.positions,donor.delta);
        for(const auto& [footID,target]:group.feet) {
            if(!SameTransform(anchor.local,target.local)||!SameTransform(anchor.skin,target.skin))continue;
            const auto fit=surface::FitTarget(map,anchor.positions,anchor.triangles,target.positions,target.triangles,referenceValue);
            const auto recommendation=surface::Recommendation(fit);
            const auto key=std::format("{:016x}",foot_snapshot_core::HashText(donorID+footID+anchor.context+anchor.fingerprint));
            Json result={{"key",key},{"status",recommendation},{"automaticApplicationAllowed",false},
                         {"stocking",donor.identity},{"footwear",target.identity},{"referenceFootwear",anchor.identity},
                         {"referenceNoHeel",referenceValue},{"anchorAuthority","explicit manual reference pairing; not independent automatic validation"},
                         {"referenceCapture",anchor.captureKey},{"targetCapture",target.captureKey},{"donorCapture",donor.captureKey},
                         {"referenceSource",anchor.source},{"targetSource",target.source},{"donorSource",donor.source},
                         {"donorLocalCalibration",donor.localFit},{"sourceMorphContextEqual",true},
                         {"correspondence",{{"status",map.status},{"eligibleVertices",map.eligible},{"matchedVertices",map.entries.size()},{"coverage",map.coverage},{"rmsReferenceGap",map.rmsGap},{"maxReferenceGap",map.maxGap}}},
                         {"rawNoHeel",nullptr},{"clampedValue",nullptr}};
            if(fit.status=="diagnostic-fit") {
                result["rawNoHeel"]=fit.rawNoHeel;result["rmsResidual"]=fit.rmsResidual;result["maxResidual"]=fit.maxResidual;
                result["basisNormalizedResidual"]=fit.basisNormalizedResidual;result["targetMotionRms"]=fit.targetMotionRms;
                result["motionRelativeResidual"]=fit.hasMotion?Json(fit.motionRelativeResidual):Json(nullptr);
            }
            if(results.size()>=64)results.erase(results.begin());
            results.insert_or_assign(key,std::move(result));
            logger::info("[surface posture] armor='{}' donor='{}' rawNoHeel={:.6f} residual={:.6f} status={} autoApply=false",target.identity.at("armor").get<std::string>(),donor.identity.at("armor").get<std::string>(),fit.rawNoHeel,fit.rmsResidual,recommendation);
        }
    }
    Persist();
}
} // namespace
void Process(Json& document,const ReadMorph& readMorph) {
    document["analysisGeneratorVersion"]="0.13.0";
    document["surfaceCalibration"]={{"status","disabled"},{"automaticApplicationAllowed",false}};
    try {
        const auto cfg=Configuration();
        if(!cfg.value("measureSurfaceCalibration",false))return;
        const auto reference=cfg.value("surfaceCalibrationReference",Json::object());
        const auto armor=reference.value("armor",std::string{}),addon=reference.value("addon",std::string{});
        const double referenceValue=reference.value("noHeel",1.);
        if(armor.empty()||addon.empty()||!std::isfinite(referenceValue)||referenceValue<0||referenceValue>1){document["surfaceCalibration"]["status"]="invalid-reference-configuration";return;}
        auto sample=CheckedSample(document);
        if(!sample){document["surfaceCalibration"]["status"]="source-or-context-unverified";return;}
        auto contextKey=sample->context+reference.dump();
        if(!contexts.contains(contextKey)&&contexts.size()>=4)contexts.erase(contexts.begin());
        auto& group=contexts[contextKey];
        const auto role=document.value("geometryRole",std::string{});
        if(role=="stocking") {
            if(!ReconstructDonor(*sample,document,readMorph,referenceValue)){document["surfaceCalibration"]["status"]="stocking-local-reference-unverified";return;}
            if(group.donors.size()>=4)group.donors.erase(group.donors.begin());
            const auto id=PairID(*sample);group.donors.insert_or_assign(id,std::move(*sample));
        } else if(role=="foot") {
            if(sample->identity.at("armor")==armor&&sample->identity.at("addon")==addon)group.anchor=*sample;
            if(group.feet.size()>=8)group.feet.erase(group.feet.begin());
            const auto id=PairID(*sample);group.feet.insert_or_assign(id,std::move(*sample));
        } else { document["surfaceCalibration"]["status"]="unsupported-role";return; }
        Evaluate(group,referenceValue);
        document["surfaceCalibration"]["status"]=group.anchor&& !group.donors.empty()?"report-written":"waiting-for-reference-pair";
        document["surfaceCalibration"]["report"]=kOutput;
    } catch(const std::exception& e) {
        document["surfaceCalibration"]["status"]="measurement-failed";document["surfaceCalibration"]["error"]=e.what();
        logger::warn("[surface posture] measurement failed: {}; runtime morphs unchanged",e.what());
    }
}
} // namespace vanity_ube_heel_adapter::stocking_surface_calibration
