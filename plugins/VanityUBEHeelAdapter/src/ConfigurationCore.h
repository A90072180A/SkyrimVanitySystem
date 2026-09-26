#pragma once
#include "HeightPlanCore.h"
#include <nlohmann/json.hpp>
#include <set>
#include <cctype>
namespace vanity_ube_heel_adapter::configuration_core {
using Json=nlohmann::json;
inline void Require(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
inline bool ID(const std::string& s) {
    auto bar=s.rfind('|');if(bar==std::string::npos||bar<5||s.size()-bar-1!=8||s.size()>300)return false;
    auto file=s.substr(0,bar);std::transform(file.begin(),file.end(),file.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    if(!(file.ends_with(".esp")||file.ends_with(".esm")||file.ends_with(".esl")))return false;
    return std::all_of(s.begin()+bar+1,s.end(),[](unsigned char c){return std::isxdigit(c)!=0;});
}
inline double HeelMax(const Json& j){return j.value("heelMax",2.0);}
inline void Fields(const Json& j,const std::set<std::string>& allowed,const std::string& location){
    Require(j.is_object(),location+" must be an object");
    for(auto i=j.begin();i!=j.end();++i)Require(allowed.contains(i.key()),location+": unknown field "+i.key());
}
inline void Validate(const Json& c){
    Require(c.is_object(),"configuration must be an object");
    Require(height_plan_core::ValidHeelLimit(HeelMax(c)),"heelMax must be finite in [1,10]");
    for(const auto* name:{"applyMorph","automaticHeight","autoDetectStockings","barefootFlatFeet","allowHeightEndpointApproximation","exportFootGeometry","measureSurfaceCalibration","writeRuntimeState","exportStockingCalibration","enableComponentSubset","useOfflineHeightLibrary","applyHeightResidualWarnings"})
        if(c.contains(name))Require(c.at(name).is_boolean(),std::string(name)+" must be boolean");
    for(const auto* name:{"heightMaxNormalizedResidual"})if(c.contains(name)){
        Require(c.at(name).is_number(),std::string(name)+" must be numeric");auto v=c.at(name).get<double>();
        Require(std::isfinite(v)&&v>=0&&v<=1,std::string(name)+" must be finite in [0,1]");}
    if(c.contains("stockings")){
        Require(c.at("stockings").is_array()&&c.at("stockings").size()<=2048,"stockings must be an array of at most 2048 IDs");
        for(const auto& v:c.at("stockings"))Require(v.is_string()&&ID(v.get<std::string>()),"stockings: use stable plugin.esp|00000001 ID");}
    if(c.contains("heels")){
        Require(c.at("heels").is_object(),"heels must be an object");
        for(auto i=c.at("heels").begin();i!=c.at("heels").end();++i){Require(ID(i.key())&&i->is_number(),"heels: invalid ID/value");auto v=i->get<double>();Require(std::isfinite(v)&&v>=0&&v<=1,"legacy heels values are NoHeel and must stay in [0,1]");}}
    if(c.contains("signedHeightOverrides")){
        Require(c.at("signedHeightOverrides").is_array(),"signedHeightOverrides must be an array");std::set<std::string> seen;
        for(const auto&r:c.at("signedHeightOverrides")){
            Require(r.is_object()&&r.contains("stocking")&&r.contains("footwear")&&r.contains("signedPosture"),"signedHeightOverrides: missing fields");
            Require(r.at("stocking").is_string()&&r.at("footwear").is_string()&&ID(r.at("stocking"))&&ID(r.at("footwear")),"signedHeightOverrides: invalid IDs");
            Require(r.at("signedPosture").is_number()&&height_plan_core::FromSigned(r.at("signedPosture").get<double>(),HeelMax(c)).has_value(),"signedPosture out of asymmetric bounds");
            Require(seen.insert(Json::array({r.at("stocking"),r.at("footwear")}).dump()).second,"duplicate signedHeightOverrides pair");}}
    if(c.contains("manualPairs")){
        Require(c.at("manualPairs").is_array()&&c.at("manualPairs").size()<=2048,"manualPairs must be an array");std::set<std::string> seen;
        for(const auto&r:c.at("manualPairs")){
            Fields(r,{"stocking","footwear","mode","NoHeel","Heel","note","approval"},"manual pair");
            const auto stock=r.value("stocking",std::string{}),shoe=r.value("footwear",std::string{}),mode=r.value("mode",std::string{"manual"});
            Require(ID(stock)&&(ID(shoe)||shoe=="<barefoot>"),"manual pair: invalid stable IDs");
            Require(mode=="manual"||mode=="ignore","manual pair mode must be manual or ignore");
            Require(seen.insert(Json::array({stock,shoe}).dump()).second,"duplicate manual pair");
            if(mode=="manual"){
                Require(r.contains("NoHeel")&&r.at("NoHeel").is_number()&&r.contains("Heel")&&r.at("Heel").is_number(),"manual pair requires both NoHeel and Heel");
                Require(height_plan_core::Valid({r.at("NoHeel").get<double>(),r.at("Heel").get<double>()},HeelMax(c)),"manual pair: NoHeel 0..1, Heel 0..heelMax, only one nonzero branch");}
            if(r.contains("note"))Require(r.at("note").is_string()&&r.at("note").get<std::string>().size()<=2000,"note must be <=2000 characters");
            if(r.contains("approval")){
                const auto&a=r.at("approval");Fields(a,{"context","targetPositionFingerprint","donorSourceFingerprint","bodyTriFingerprint","referenceConfiguration","stockingAddon","footwearAddon"},"approval");
                for(const auto* k:{"context","targetPositionFingerprint","donorSourceFingerprint","bodyTriFingerprint","referenceConfiguration","stockingAddon","footwearAddon"})
                    Require(a.contains(k)&&a.at(k).is_string()&&!a.at(k).get<std::string>().empty(),std::string("approval missing ")+k);
            }
        }
    }
    if(c.contains("manualItemKinds")){
        Require(c.at("manualItemKinds").is_object()&&c.at("manualItemKinds").size()<=2048,"manualItemKinds must be an object");
        for(auto i=c.at("manualItemKinds").begin();i!=c.at("manualItemKinds").end();++i){
            Require(ID(i.key())&&i->is_string(),"invalid item classification");auto v=i->get<std::string>();Require(v=="stocking"||v=="footwear"||v=="ignore","invalid item kind");}}
}
inline Json Merge(const Json& base,const Json& user){
    Require(base.is_object(),"base configuration must be an object");Json result=base;
    if(!result.contains("heelMax"))result["heelMax"]=2.0;
    if(!result.contains("writeRuntimeState"))result["writeRuntimeState"]=true;
    if(!result.contains("enableComponentSubset"))result["enableComponentSubset"]=true;
    Fields(user,{"schema","settings","items","pairs"},"user file");
    if(user.contains("schema"))Require(user.at("schema")==1,"unsupported user schema");
    if(user.contains("settings")){
        Fields(user.at("settings"),{"heelMax","heightMaxNormalizedResidual","applyMorph","automaticHeight","autoDetectStockings","barefootFlatFeet","allowHeightEndpointApproximation","exportFootGeometry","exportStockingCalibration","writeRuntimeState","enableComponentSubset","useOfflineHeightLibrary","applyHeightResidualWarnings"},"user settings");
        for(auto i=user.at("settings").begin();i!=user.at("settings").end();++i)result[i.key()]=i.value();
    }
    if(user.contains("items")){
        const auto&items=user.at("items");Require(items.is_array()&&items.size()<=2048,"items must be an array");Json kinds=Json::object(),notes=Json::object();
        for(const auto&row:items){Fields(row,{"armor","kind","note","addons"},"item");auto id=row.value("armor",std::string{}),kind=row.value("kind",std::string{});
            Require(ID(id)&&!kinds.contains(id),"item ID invalid or duplicated");Require(kind=="stocking"||kind=="footwear"||kind=="ignore","kind must be stocking, footwear or ignore");kinds[id]=kind;
            if(row.contains("note")){Require(row.at("note").is_string(),"item note must be a string");notes[id]=row.at("note");}
            if(row.contains("addons")) {
                const auto& list=row.at("addons");Require(list.is_array()&&list.size()<=32,"addons must be an array <=32");
                for(const auto& a:list)Require(a.is_string()&&ID(a.get<std::string>()),"invalid addon ID");
                if(kind=="stocking")result["calibrationDonorAddons"][id]=list;
            }
        }
        result["manualItemKinds"]=kinds;result["manualItemNotes"]=notes;
    }
    if(user.contains("pairs"))result["manualPairs"]=user.at("pairs");
    Validate(result);return result;
}
// Debounce only complete read pairs. Invalid bytes never replace the active
// configuration, and retry is possible after any later valid content change.
struct Debounce {
    std::string last,emitted;unsigned repeats{};
    bool Ready(const std::string& fingerprint){
        if(fingerprint!=last){last=fingerprint;repeats=1;return false;}
        if(repeats<2)++repeats;
        if(repeats>=2&&emitted!=fingerprint){emitted=fingerprint;return true;}return false;
    }
};
}
