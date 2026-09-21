#include "StockingClearanceDiagnostics.h"
#include "StockingClearanceCore.h"
#include "FootSnapshotCore.h"
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <unordered_set>
namespace vanity_ube_heel_adapter::stocking_clearance_diagnostics {
namespace {
using Json=nlohmann::json;
namespace s=surface_posture_core;
namespace c=stocking_clearance_core;
constexpr auto kDirectory="Data/SKSE/Plugins/VanityUBEHeelAdapter";
struct Sample {
    std::vector<s::Point> points,delta;
    std::vector<s::Triangle> triangles;
    Json identity,source,local,skin,calibration;
    std::string context,key,capture,positionFingerprint,topologyFingerprint;
};
struct Group {std::optional<Sample> anchor;std::map<std::string,Sample> feet,donors;};
// Single writer-owned state. No game pointers, no configuration authority.
std::map<std::string,Group> groups;
std::map<std::string,Json> reports;
std::unordered_set<std::string> writtenProposals;
void WriteAtomic(const std::filesystem::path& path,const Json& content){
    std::error_code ec;std::filesystem::create_directories(path.parent_path(),ec);
    if(ec)throw std::runtime_error(ec.message());
    const auto temp=std::filesystem::path(path.wstring()+L".tmp");
    std::ofstream f(temp,std::ios::binary|std::ios::trunc);
    if(!f)throw std::runtime_error("cannot open clearance report");
    f<<content.dump(2)<<'\n';f.flush();if(!f)throw std::runtime_error("cannot flush clearance report");
    f.close();if(f.fail())throw std::runtime_error("cannot close clearance report");
    if(!::MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error(std::format("clearance report replace failed: {}",::GetLastError()));
}
std::optional<Sample> ReadSample(const Json& d,double referenceValue){
    if(d.at("extraction").value("status",std::string{})!="complete"||
       !d.at("sourceModelEvidence").value("measurementEligible",false)||
       !d.at("captureSelection").value("currentGraphConfirmed",false)||
       d.at("captureSelection").value("identityIsCorrelated",true)||
       d.at("identity").value("buffered",true)||!d.at("actorMorphValues").is_array())return {};
    Sample v;v.identity=d.at("identity");v.source=d.at("sourceModelEvidence");
    v.local=d.at("localTransform");v.skin=d.at("rootParentToSkin");
    if(!v.local.is_object()||!v.skin.is_object())return {};
    v.points=d.at("positions").get<std::vector<s::Point>>();v.triangles=d.at("triangles").get<std::vector<s::Triangle>>();
    s::Surface validate(v.points,v.triangles);if(!validate.valid)return {};
    v.context=Json::array({v.identity.at("race"),v.identity.at("sexIndex"),v.identity.at("actorWeight"),d.at("actorMorphValues"),v.local,v.skin}).dump();
    v.capture=d.value("captureKey",std::string{});v.positionFingerprint=d.value("positionFingerprint",std::string{});
    v.topologyFingerprint=d.value("topologyFingerprint",std::string{});
    v.key=Json::array({v.identity.at("armor"),v.identity.at("addon"),v.source.at("asset").at("sourceFingerprint"),v.source.at("sourceGeometryName")}).dump();
    if(d.value("geometryRole",std::string{})=="stocking"){
        v.calibration=d.value("stockingLocalCalibration",Json::object());
        if(v.calibration.value("status",std::string{})!="source-reconstructed-reference")return {};
        const double q=v.calibration.at("rawObservedNoHeel").get<double>();if(!std::isfinite(q))return {};
        std::size_t matches=0;
        for(const auto&m:d.at("nativeMorphMeasurements")){
            if(m.value("morph",std::string{})!="NoHeel"||m.value("status",std::string{})!="present"||
               m.value("shapeIdentityBasis",std::string{})!="matched-source-nif")continue;
            ++matches;v.delta.assign(v.points.size(),{});std::unordered_set<unsigned> seen;
            for(const auto&r:m.at("offsets")){auto i=r.at("index").get<unsigned>();if(i>=v.points.size()||!seen.insert(i).second)return {};
                v.delta[i]=r.at("delta").get<s::Point>();if(!s::Finite(s::V(v.delta[i])))return {};}
        }
        if(matches!=1)return {};
        // The previous stage certified this baseline. Preserve its recorded
        // residual; never treat arbitrary raw coordinates as NoHeel=0.
        for(std::size_t i=0;i<v.points.size();++i)for(unsigned axis=0;axis<3;++axis)
            v.points[i][axis]+=static_cast<float>((referenceValue-q)*v.delta[i][axis]);
    }
    return v;
}
Json FitJson(const s::Fit& f){
    Json j={{"status",f.status},{"automaticApplicationAllowed",false},{"rawNoHeel",nullptr}};
    if(f.status=="diagnostic-fit"){
        j["rawNoHeel"]=f.rawNoHeel;j["rmsResidual"]=f.rmsResidual;j["maxResidual"]=f.maxResidual;
        j["basisNormalizedResidual"]=f.basisNormalizedResidual;j["recommendation"]=s::Recommendation(f);
    }return j;
}
void Evaluate(Group& group,double referenceValue,const Json& config){
    if(!group.anchor)return;const auto& a=*group.anchor;
    for(const auto&[donorKey,d]:group.donors){
        const auto map=s::BuildMap(a.points,a.triangles,d.points,d.delta);
        const auto gap=c::Measure(map,a.points,d.points);
        Json clearance={{"status",gap.status},{"sampledVertices",gap.count},{"eligibleVertices",map.eligible},{"coverage",map.coverage},
          {"rmsGap",gap.rmsGap},{"medianGap",gap.medianGap},{"p95Gap",gap.p95Gap},{"maxGap",gap.maxGap},
          {"rmsNormalComponent",gap.rmsNormalGap},{"rmsTangentialComponent",gap.rmsTangentialGap},
          {"meanSignedNormal",gap.meanSignedNormal},{"negativeNormalSideCount",gap.negativeSide},
          {"scope","NoHeel-affected vertices versus reference SHOE embedded foot; not a measured bare-body fit"},
          {"normalOrientation","triangle winding; NOT a certified inside/outside or collision test"}};
        const auto key=std::format("{:016x}",foot_snapshot_core::HashText(a.key+d.key+a.context+a.positionFingerprint+d.capture));
        Json proposal={{"status","disabled"},{"automaticApplicationAllowed",false}};
        if(config.value("writeStockingFitPreview",true)&&map.status=="mapped"){
            // Conservative illustrative fit target, in model units, not a
            // universal fabric thickness. This path exports data ONLY.
            const auto p=c::Propose(map,a.points,d.points,d.triangles,.15,.25,3);
            proposal={{"status",p.status},{"automaticApplicationAllowed",false},{"changedVertices",p.corrections.size()},
                      {"maxDisplacement",p.maxDisplacement},{"rmsDisplacement",p.rmsDisplacement},
                      {"targetClearance",.15},{"maxStep",.25},{"taperRings",3},
                      {"semantics","normal-only capped foot tightening proposal; not a NoHeel value or universal BodySlide morph"}};
            if(!p.corrections.empty()){
                const auto path=std::filesystem::path(kDirectory)/"fit-proposals"/("clearance-"+key+".json");
                proposal["file"]=path.generic_string();
                if(!writtenProposals.contains(key)){
                    Json exportData={{"schema",1},{"generatorVersion","0.14.0"},{"status","preview-only-not-installable"},
                       {"automaticApplicationAllowed",false},{"identity",d.identity},{"source",d.source},
                       {"referenceFootwear",a.identity},{"actorContext",Json::parse(a.context)},
                       {"stockingCapture",d.capture},{"referenceCapture",a.capture},{"referenceNoHeel",referenceValue},
                       {"topologyFingerprint",d.topologyFingerprint},{"localCalibration",d.calibration},
                       {"parameters",proposal},{"offsets",Json::array()},
                       {"limits","No NIF/OSP/OSD/TRI mutation. No weight or UV changes. No self-intersection, animation or shoe-shell collision certification."}};
                    for(const auto&v:p.corrections)exportData["offsets"].push_back({{"index",v.index},{"delta",v.displacement}});
                    WriteAtomic(path,exportData);if(writtenProposals.size()>=64)writtenProposals.clear();writtenProposals.insert(key);
                }
            }
        }
        for(const auto&[footKey,b]:group.feet){
            const auto comparison=c::Compare(map,a.points,a.triangles,b.points,b.triangles,d.points,referenceValue);
            const auto pairKey=std::format("{:016x}",foot_snapshot_core::HashText(key+footKey+b.positionFingerprint));
            Json entry={{"key",pairKey},{"status",comparison.status},{"automaticApplicationAllowed",false},
                {"stocking",d.identity},{"footwear",b.identity},{"referenceFootwear",a.identity},
                {"referenceCapture",a.capture},{"targetCapture",b.capture},{"donorCapture",d.capture},
                {"contextEqual",true},{"referenceClearance",clearance},{"tighteningProposal",proposal},
                {"fixedGapFit",FitJson(comparison.fixedGap)},{"rotatingGapFit",FitJson(comparison.rotatingGap)},
                {"gapRotationRms",comparison.gapRotationRms},{"rawCoefficientSensitivity",comparison.rawCoefficientSensitivity},
                {"interpretation","Compare approximations; do not choose whichever passes a threshold. Rotating triangle frames preserve measured gap length, not cloth physics."}};
            if(reports.size()>=64)reports.erase(reports.begin());reports.insert_or_assign(pairKey,std::move(entry));
            logger::info("[stocking clearance] donor='{}' target='{}' gapRms={:.6f} fixed={:.6f} rotating={:.6f} sensitivity={:.6f} status={} autoApply=false",
                d.identity.at("armor").get<std::string>(),b.identity.at("armor").get<std::string>(),gap.rmsGap,
                comparison.fixedGap.rawNoHeel,comparison.rotatingGap.rawNoHeel,comparison.rawCoefficientSensitivity,comparison.status);
        }
    }
    Json root={{"schema",1},{"generatorVersion","0.14.0"},{"automaticApplicationAllowed",false},{"status","diagnostic-only"},{"entries",Json::array()}};
    for(const auto&[key,value]:reports)root["entries"].push_back(value);
    WriteAtomic(std::filesystem::path(kDirectory)/"stocking-clearance.json",root);
}
}
void Process(Json& document){
    document["stockingClearance"]={{"status","disabled"},{"automaticApplicationAllowed",false}};
    try{
        std::ifstream f("Data/SKSE/Plugins/VanityUBEHeelAdapter.json");if(!f)return;
        auto config=Json::parse(f);
        if(!config.is_object()||!config.value("measureStockingClearance",true)||!config.value("measureSurfaceCalibration",false))return;
        const auto reference=config.value("surfaceCalibrationReference",Json::object());
        const auto armor=reference.value("armor",std::string{}),addon=reference.value("addon",std::string{});
        const double value=reference.value("noHeel",1.);
        if(armor.empty()||addon.empty()||!std::isfinite(value)||value<0||value>1){document["stockingClearance"]["status"]="invalid-reference";return;}
        auto sample=ReadSample(document,value);if(!sample){document["stockingClearance"]["status"]="source-or-local-baseline-unverified";return;}
        const auto context=sample->context+reference.dump();
        if(!groups.contains(context)&&groups.size()>=4)groups.erase(groups.begin());auto& group=groups[context];
        const auto role=document.value("geometryRole",std::string{});
        const auto sampleKey=sample->key;
        if(role=="stocking"){
            if(group.donors.size()>=4)group.donors.erase(group.donors.begin());group.donors.insert_or_assign(sampleKey,std::move(*sample));
        }else if(role=="foot"){
            if(sample->identity.at("armor")==armor&&sample->identity.at("addon")==addon)group.anchor=*sample;
            if(group.feet.size()>=8)group.feet.erase(group.feet.begin());group.feet.insert_or_assign(sampleKey,std::move(*sample));
        }else{return;}
        if(!group.anchor||group.donors.empty()){document["stockingClearance"]["status"]="waiting-for-reference-pair";return;}
        Evaluate(group,value,config);document["stockingClearance"]["status"]="report-written";
        document["stockingClearance"]["report"]=std::string(kDirectory)+"/stocking-clearance.json";
    }catch(const std::exception&e){document["stockingClearance"]["status"]="measurement-failed";document["stockingClearance"]["error"]=e.what();
        logger::warn("[stocking clearance] measurement failed: {}; no geometry modified",e.what());}
}
} // namespace vanity_ube_heel_adapter::stocking_clearance_diagnostics
