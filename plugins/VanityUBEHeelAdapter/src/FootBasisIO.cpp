#include "FootBasisIO.h"
#include "HeightProfiles.h"
#include "TriMorphCore.h"
#include "FootCapturePolicy.h"
#include "SourceGeometryEvidence.h"
#include "StockingSurfaceCalibration.h"
#include "StockingClearanceDiagnostics.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <format>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace vanity_ube_heel_adapter::foot_basis_io {
namespace {
using Json = nlohmann::json;
namespace tri = tri_morph_core;
struct Cached { std::uintmax_t size{}; std::filesystem::file_time_type modified{}; Json result; };
std::unordered_map<std::string, Cached> cache;
struct LastFile { std::string path; std::uintmax_t size{}; std::filesystem::file_time_type modified{}; std::vector<std::uint8_t> bytes; };
std::optional<LastFile> lastFile;
Json ReadBasis(const std::string& resource,const std::string& shape,const std::string& morph,std::uint32_t limit)
{
    Json out = {{"resource",resource},{"shape",shape},{"morph",morph},
        {"readScope","MO2-visible loose files; BSA-only resources are not read by this worker"},
        {"status","resource-unreadable"},{"offsets",Json::array()},
        {"vertexLimitChecked",limit},{"referenceTopologyValidated",false},
        {"mappingToNoHeel",nullptr},{"automaticApplicationAllowed",false}};
    const auto normalized=foot_capture_policy::ResourceKey(resource);
    if(normalized.empty()||!normalized.ends_with(".tri")){out["status"]="invalid-resource-path";return out;}
    const auto path=std::filesystem::path("Data/Meshes")/normalized;
    std::error_code ec;const auto size=std::filesystem::file_size(path,ec);
    if(ec){out["error"]=ec.message();return out;}
    const auto modified=std::filesystem::last_write_time(path,ec);
    if(ec){out["error"]=ec.message();return out;}
    const auto key=Json::array({normalized,shape,morph,limit}).dump();
    if(const auto found=cache.find(key);found!=cache.end()&&found->second.size==size&&found->second.modified==modified)return found->second.result;
    if(size>64u*1024u*1024u){out["status"]="file-too-large";return out;}
    if(!lastFile||lastFile->path!=normalized||lastFile->size!=size||lastFile->modified!=modified){
        std::ifstream stream(path,std::ios::binary);if(!stream)return out;
        LastFile file{normalized,size,modified,std::vector<std::uint8_t>(static_cast<std::size_t>(size))};
        if(size&&!stream.read(reinterpret_cast<char*>(file.bytes.data()),static_cast<std::streamsize>(size))){out["status"]="resource-read-failed";return out;}
        const auto afterSize=std::filesystem::file_size(path,ec);if(ec||afterSize!=size){out["status"]="resource-changed-during-read";return out;}
        const auto afterTime=std::filesystem::last_write_time(path,ec);if(ec||afterTime!=modified){out["status"]="resource-changed-during-read";return out;}
        lastFile=std::move(file);
    }
    const auto parsed=tri::Parse(lastFile->bytes,shape,morph,limit);
    out["status"]=parsed.status;out["error"]=parsed.error;out["sourceBytes"]=size;
    out["sourceFingerprint"]=std::format("{:016x}",parsed.sourceHash);
    out["fingerprintAlgorithm"]="FNV1a64 of file bytes; diagnostic, not cryptographic";
    out["shapeNames"]=parsed.shapeNames;out["morphNamesForShape"]=parsed.morphNames;
    out["deltaSemantics"]="displacements per one unit of THIS named morph; no alias, endpoint or sign inferred";
    for(const auto&offset:parsed.offsets)out["offsets"].push_back({{"index",offset.index},{"delta",offset.delta}});
    out["nonzeroOffsetCount"]=parsed.offsets.size();
    logger::info("[morph measurement] resource='{}' shape='{}' morph='{}' status={} offsets={} autoApply=false",resource,shape,morph,parsed.status,parsed.offsets.size());
    if(cache.size()>=64)cache.clear();cache.insert_or_assign(key,Cached{size,modified,out});return out;
}
std::vector<std::string> MorphRequests(const Json&document){
    // Heel is measured independently; never assumed to be NoHeel's inverse.
    if(document.value("geometryRole",std::string{"foot"})=="stocking")return {"NoHeel","Heel"};
    std::vector<std::string> names{"NoHeel"};
    if(document.contains("requestedDiagnosticMorphs")&&document["requestedDiagnosticMorphs"].is_array())
        for(const auto&entry:document["requestedDiagnosticMorphs"]){
            if(!entry.is_string()||names.size()>=4)continue;const auto value=entry.get<std::string>();
            if(!value.empty()&&value.size()<=255&&std::find(names.begin(),names.end(),value)==names.end())names.push_back(value);
        }
    return names;
}
}
void Enrich(Json&document){
    source_geometry_evidence::Resolve(document);
    document["generatorVersion"]="0.15.0";document["schema"]=6;
    document["triMorphData"]=Json::array();document["nativeMorphMeasurements"]=Json::array();document["referenceMorphMeasurements"]=Json::array();
    document["referenceCalibration"]={{"status","not-calibrated"},{"posture",nullptr}};
    if(document.value("geometryRole",std::string{})=="rejected-calibration-source")return;
    const auto count=document.at("positions").size();const auto limit=count>0&&count<=65535?static_cast<std::uint32_t>(count):65536u;
    const auto&e=document.at("sourceModelEvidence");const bool source=e.value("measurementEligible",false);
    if(!source && document.value("geometryRole",std::string{})=="stocking"){
        document["nativeFootFit"]={{"status","stocking-source-not-confirmed"},{"automaticApplicationAllowed",false}};return;
    }
    const auto shape=source?e.at("sourceGeometryName").get<std::string>():document.value("geometry",std::string{"Feet"});
    const auto&resources=source?e.at("bodyTriPaths"):document.at("identity").at("bodyTriPaths");
    const auto morphs=MorphRequests(document);std::unordered_set<std::string> visited;
    for(const auto&entry:resources){
        if(!entry.is_string())continue;const auto resource=entry.get<std::string>();
        if(visited.size()>=8){document["triMorphDataTruncated"]=true;break;}
        if(!visited.insert(resource).second)continue;
        for(const auto&morph:morphs){auto result=ReadBasis(resource,shape,morph,limit);result["shapeIdentityBasis"]=source?"matched-source-nif":"runtime-metadata";if(morph=="NoHeel")document["triMorphData"].push_back(result);document["nativeMorphMeasurements"].push_back(std::move(result));}
    }
    const auto reference=document.value("requestedReferenceBodyTri",std::string{});
    if(!reference.empty())for(const auto&morph:morphs){auto result=ReadBasis(reference,"Feet",morph,65536u);if(morph=="NoHeel")document["referenceTriBasis"]=result;document["referenceMorphMeasurements"].push_back(std::move(result));}
    source_geometry_evidence::Measure(document,ReadBasis);
    height_profiles::ObserveFoot(document);
    stocking_surface_calibration::Process(document,ReadBasis);
    // Clearance and tightening deliberately paused for the height milestone.
}
} // namespace vanity_ube_heel_adapter::foot_basis_io
