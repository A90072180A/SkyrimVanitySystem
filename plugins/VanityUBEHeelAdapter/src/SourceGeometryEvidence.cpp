#include "ConfigState.h"
#include "SourceGeometryEvidence.h"
#include "NifSourceCore.h"
#include "NativeFootFitCore.h"
#include "FootCapturePolicy.h"
#include "TriMorphCore.h"
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <format>
#include <unordered_map>
#include <unordered_set>

namespace vanity_ube_heel_adapter::source_geometry_evidence {
namespace {
using Json=nlohmann::json;
namespace nif=nif_source_core;
namespace core=foot_snapshot_core;
struct Asset { std::string status{"unreadable"},error,fingerprint,resource; nif::Result parsed; std::uintmax_t size{}; std::filesystem::file_time_type time{}; };
// Writer-owned session cache. This is not an automatically applied shoe profile.
std::unordered_map<std::string,std::shared_ptr<const Asset>> assets;
Json Config(){return config_state::Get();}
std::shared_ptr<const Asset> Read(const std::string& resource){
 auto a=std::make_shared<Asset>();a->resource=resource;
 const auto key=foot_capture_policy::ResourceKey(resource);
 if(key.empty()||!key.ends_with(".nif")){a->status="invalid-nif-resource";return a;}
 const auto path=std::filesystem::path("Data/Meshes")/key;std::error_code ec;
 const auto size=std::filesystem::file_size(path,ec);if(ec){a->error=ec.message();return a;}
 const auto time=std::filesystem::last_write_time(path,ec);if(ec){a->error=ec.message();return a;}
 if(auto it=assets.find(key);it!=assets.end()&&it->second->size==size&&it->second->time==time)return it->second;
 if(size>64u*1024u*1024u){a->status="file-too-large";return a;}
 std::ifstream f(path,std::ios::binary);if(!f)return a;
 std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
 if(size&&!f.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(size))){a->status="read-failed";return a;}
 auto after=std::filesystem::file_size(path,ec);if(ec||after!=size){a->status="changed-during-read";return a;}
 auto afterTime=std::filesystem::last_write_time(path,ec);if(ec||afterTime!=time){a->status="changed-during-read";return a;}
 a->size=size;a->time=time;a->fingerprint=std::format("{:016x}",tri_morph_core::HashBytes(bytes));a->parsed=nif::Parse(bytes);a->status=a->parsed.status;a->error=a->parsed.error;
 if(assets.size()>=16)assets.clear();assets[key]=a;return a;
}
Json AssetInfo(const Asset& a){return {{"resource",a.resource},{"status",a.status},{"error",a.error},{"sourceBytes",a.size},{"sourceFingerprint",a.fingerprint},{"fingerprintAlgorithm","FNV1a64 file bytes; diagnostic"}};}
Json Transform(const nif::Transform&t){return {{"rotation",{{t.rotation[0],t.rotation[1],t.rotation[2]},{t.rotation[3],t.rotation[4],t.rotation[5]},{t.rotation[6],t.rotation[7],t.rotation[8]}}},{"translation",t.translation},{"scale",t.scale}};}
bool SameTransform(const Json& a,const Json& b){
 try{if(!a.is_object()||!b.is_object())return false;std::vector<double>x,y;for(const auto&row:a.at("rotation"))for(const auto&v:row)x.push_back(v.get<double>());for(const auto&row:b.at("rotation"))for(const auto&v:row)y.push_back(v.get<double>());for(const auto&v:a.at("translation"))x.push_back(v.get<double>());for(const auto&v:b.at("translation"))y.push_back(v.get<double>());x.push_back(a.at("scale").get<double>());y.push_back(b.at("scale").get<double>());if(x.size()!=13||y.size()!=13)return false;for(std::size_t i=0;i<x.size();++i)if(!std::isfinite(x[i])||!std::isfinite(y[i])||std::abs(x[i]-y[i])>1e-5)return false;return true;}catch(...){return false;}
}
const nif::Shape* UniqueShape(const nif::Result&r,const std::vector<core::Triangle>&triangles,std::size_t count,const std::string& name={}){
 const nif::Shape* chosen=nullptr;
 for(const auto&s:r.shapes)if(s.mesh.Complete()&&s.mesh.positions.size()==count&&s.mesh.triangles==triangles&&(name.empty()||s.name==name)){if(chosen)return nullptr;chosen=&s;}return chosen;
}
bool DonorAllowed(const Json&cfg,const Json&identity){
 const auto armor=identity.value("armor",std::string{}),addon=identity.value("addon",std::string{});
 const auto it=cfg.find("calibrationDonorAddons");if(it==cfg.end()||!it->is_object())return false;
 const auto pair=it->find(armor);if(pair==it->end()||!pair->is_array())return false;
 for(const auto&v:*pair)if(v.is_string()&&v.get<std::string>()==addon)return true;return false;
}
std::optional<std::vector<core::Point>> Dense(const Json&measurement,std::size_t n){
 if(measurement.value("status",std::string{})!="present"||n>65535)return {};
 std::vector<core::Point> out(n);std::unordered_set<unsigned>seen;
 try{for(const auto&o:measurement.at("offsets")){const auto i=o.at("index").get<unsigned>();if(i>=n||!seen.insert(i).second)return {};out[i]=o.at("delta").get<core::Point>();for(float v:out[i])if(!std::isfinite(v))return {};}}catch(...){return {};}return out;
}
}
void Resolve(Json& d){
 d["captureGeneratorVersion"]=d.value("generatorVersion",std::string{});d["generatorVersion"]="0.12.0";d["schema"]=4;
 d["sourceModelEvidence"]={{"status","not-run"},{"measurementEligible",false}};
 auto&e=d["sourceModelEvidence"];const auto cfg=Config();const auto role=d.value("geometryRole",std::string{});
 if(role=="stocking"&&!DonorAllowed(cfg,d.at("identity"))){
  e["status"]="rejected-unregistered-armor-addon-pair";d["claimedGeometryRole"]=role;d["geometryRole"]="rejected-calibration-source";
  logger::warn("[source geometry] rejected calibration armor='{}' addon='{}'; no native morph calibration",d["identity"].value("armor",std::string{}),d["identity"].value("addon",std::string{}));return;
 }
 if(d.at("extraction").value("status",std::string{})!="complete"){e["status"]="runtime-extraction-incomplete";return;}
 const auto a=Read(d.at("identity").value("armaModel",std::string{}));e["asset"]=AssetInfo(*a);
 if(!a->parsed.Complete()){e["status"]="source-nif-unavailable-or-unsupported";return;}
 const auto triangles=d.at("triangles").get<std::vector<core::Triangle>>();const auto n=d.at("positions").size();
 const auto*s=UniqueShape(a->parsed,triangles,n);
 if(!s){e["status"]="no-unique-source-topology-match";return;}
 if(!SameTransform(d.value("localTransform",Json()),Transform(s->local))||!SameTransform(d.value("rootParentToSkin",Json()),Transform(s->skin))){e["status"]="source-transform-mismatch";return;}
 e["status"]="exact-source-topology-match";e["measurementEligible"]=true;e["sourceGeometryName"]=s->name;e["bodyTriPaths"]=s->bodyTris;e["orderedTopologyEqual"]=true;e["sourceVertexCount"]=s->mesh.positions.size();e["sourceTriangleCount"]=s->mesh.triangles.size();e["topologyFingerprint"]=std::format("{:016x}",s->mesh.topologyHash);
 e["identityBasis"]="actual declared ARMA model plus unique ordered topology and local/bind transform match; explicit ARMO/ARMA pair additionally required for stocking donor";
 logger::info("[source geometry] armor='{}' runtimeName='{}' sourceName='{}' BODYTRIs={} exactTopology=true",d["identity"].value("armor",std::string{}),d.value("geometry",std::string{}),s->name,s->bodyTris.size());
}
void Measure(Json&d,const ReadMorph&readMorph){
 d["nativeFootFit"]={{"status","not-run"},{"mappingToNoHeel",nullptr},{"automaticApplicationAllowed",false}};auto&out=d["nativeFootFit"];
 if(d.value("geometryRole",std::string{})!="foot"){out["status"]="not-foot";return;}
 if(!d.at("sourceModelEvidence").value("measurementEligible",false)){out["status"]="source-identity-unverified";return;}
 auto cfg=Config();const auto paths=cfg.value("referenceFeetModels",Json::array());
 if(!paths.is_array()||paths.size()!=2||!paths[0].is_string()||!paths[1].is_string()){out["status"]="reference-models-not-configured";return;}
 const auto lo=Read(paths[0].get<std::string>()),hi=Read(paths[1].get<std::string>());out["referenceAssets"]=Json::array({AssetInfo(*lo),AssetInfo(*hi)});
 if(!lo->parsed.Complete()||!hi->parsed.Complete()){out["status"]="reference-nif-unavailable-or-unsupported";return;}
 const auto observed=d.at("positions").get<std::vector<core::Point>>();const auto triangles=d.at("triangles").get<std::vector<core::Triangle>>();
 const auto*l=UniqueShape(lo->parsed,triangles,observed.size(),"Feet"),*h=UniqueShape(hi->parsed,triangles,observed.size(),"Feet");
 if(!l||!h){out["status"]="reference-topology-mismatch-no-remap";return;}
 if(!SameTransform(d.value("localTransform",Json()),Transform(l->local))||!SameTransform(d.value("rootParentToSkin",Json()),Transform(l->skin))||!SameTransform(Transform(l->local),Transform(h->local))||!SameTransform(Transform(l->skin),Transform(h->skin))){out["status"]="reference-transform-mismatch";return;}
 if(l->bodyTris.size()!=1||l->bodyTris!=h->bodyTris){out["status"]="reference-bodytri-ambiguous";return;}
 const std::string firstName="HiHeelz_CBBE",secondName="HiHeelz_CBBE_to_UBE";const auto n=static_cast<std::uint32_t>(observed.size());
 const auto m1=readMorph(l->bodyTris[0],l->name,firstName,n),m2=readMorph(l->bodyTris[0],l->name,secondName,n);
 auto a=Dense(m1,n),b=Dense(m2,n);if(!a||!b){out["status"]="native-basis-unavailable";return;}
 out["basisResource"]=l->bodyTris[0];out["basisSourceFingerprint"]=m1.value("sourceFingerprint",std::string{});
 const double weight=d.at("identity").value("actorWeight",0.0)/100.;if(!std::isfinite(weight)||weight<0||weight>1){out["status"]="invalid-weight";return;}
 std::vector<core::Point> reference(n),correction(n);
 for(unsigned i=0;i<n;++i)for(unsigned c=0;c<3;++c)reference[i][c]=static_cast<float>(l->mesh.positions[i][c]+weight*(h->mesh.positions[i][c]-l->mesh.positions[i][c]));
 std::unordered_map<std::string,double> values;std::unordered_set<std::string>available;
 for(const auto&v:m1.value("morphNamesForShape",Json::array()))if(v.is_string())available.insert(v.get<std::string>());
 const auto actor=d.value("actorMorphValues",Json());if(!actor.is_array()){out["status"]="actor-morph-state-unavailable";return;}
 for(const auto&v:actor){const auto name=v.value("name",std::string{});if(name==firstName||name==secondName||!available.contains(name))continue;const auto value=v.at("value").get<double>();if(!std::isfinite(value)){out["status"]="invalid-morph-value";return;}if(value==0)continue;if(!values.emplace(name,value).second){out["status"]="multiple-keys-effective-morph-unknown";return;}}
 out["bodyMorphCorrection"]=Json::array();
 for(const auto&[name,value]:values){const auto m=readMorph(l->bodyTris[0],l->name,name,n);auto diff=Dense(m,n);if(!diff){if(m.value("status",std::string{})=="empty-morph")continue;out["status"]="body-morph-basis-unavailable";return;}for(unsigned i=0;i<n;++i)for(unsigned c=0;c<3;++c)correction[i][c]+=static_cast<float>(value*(*diff)[i][c]);out["bodyMorphCorrection"].push_back({{"name",name},{"value",value}});}
 const auto fit=native_foot_fit_core::Fit(observed,reference,correction,*a,*b);out["status"]=fit.status;
 if(fit.status!="diagnostic-fit")return;
 out["coefficients"]={{firstName,fit.first},{secondName,fit.second}};out["rmsResidual"]=fit.rms;out["maxVertexResidual"]=fit.maxError;out["relativeResidual"]=fit.relativeResidual;out["firstBasisRms"]=fit.basisRms;out["normalizedBasisDeterminant"]=fit.normalizedDeterminant;
 out["assumption"]="subtract single-key actor morphs using the reference TRI response; target-specific conforming and baked preset differences remain in residual; NOT certified NoHeel calibration";
 logger::info("[native foot fit] armor='{}' HiHeelz_CBBE={:.6f} HiHeelz_CBBE_to_UBE={:.6f} rms={:.6f} autoApply=false",d["identity"].value("armor",std::string{}),fit.first,fit.second,fit.rms);
}
} // namespace vanity_ube_heel_adapter::source_geometry_evidence
