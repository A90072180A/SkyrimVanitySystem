#include "../src/ConfigurationCore.h"
#include "../src/ConfigState.h"
#include <iostream>
using namespace vanity_ube_heel_adapter;using Json=nlohmann::json;
int checks=0;void Check(bool b){++checks;if(!b)throw std::runtime_error("configuration check "+std::to_string(checks));}
int main(){
    auto base=Json{{"applyMorph",true},{"heels",{{"Shoe.esp|00000001",1.0}}}};
    auto merged=configuration_core::Merge(base,Json::object());Check(merged["heelMax"]==2.0);Check(merged["applyMorph"]==true);
    Json user={{"schema",1},{"settings",{{"heelMax",2.0}}},{"pairs",Json::array({{{"stocking","Sock.esp|00000001"},{"footwear","Shoe.esp|00000001"},{"NoHeel",0.0},{"Heel",1.1068}}})},
        {"items",Json::array({{{"armor","Sock.esp|00000001"},{"kind","stocking"},{"note","test"}}})}};
    merged=configuration_core::Merge(base,user);Check(merged["manualPairs"][0]["Heel"]==1.1068);Check(merged["manualItemKinds"]["Sock.esp|00000001"]=="stocking");
    config_state::Set(merged);const auto last=config_state::Get();
    auto refuse=[&](Json value){bool bad=false;try{config_state::Set(configuration_core::Merge(base,value));}catch(...){bad=true;}Check(bad);Check(config_state::Get()==last);};
    for(double n:{-0.1,1.001,double(INFINITY),double(NAN)}){auto j=user;j["pairs"][0]["NoHeel"]=n;refuse(j);}
    for(double h:{-0.1,2.001,double(INFINITY),double(NAN)}){auto j=user;j["pairs"][0]["Heel"]=h;refuse(j);}
    for(double cap:{0.99,10.01,double(INFINITY),double(NAN)}){auto j=user;j["settings"]["heelMax"]=cap;refuse(j);}
    {auto j=user;j["pairs"][0]["NoHeel"]=0.2;refuse(j);}
    {auto j=user;j["pairs"].push_back(j["pairs"][0]);refuse(j);}
    {auto j=user;j["items"].push_back(j["items"][0]);refuse(j);}
    {auto j=user;j["settings"]["HeelMax"]=4;refuse(j);}
    {auto j=user;j["schema"]=2;refuse(j);}
    {auto j=user;j["pairs"][0]["stocking"]="runtime:FE123456";refuse(j);}
    {auto j=user;j["pairs"][0]["approval"]=Json::object();refuse(j);}
    {auto j=user;j["pairs"][0]={{"stocking","Sock.esp|00000001"},{"footwear","<barefoot>"},{"mode","ignore"}};Check(configuration_core::Merge(base,j)["manualPairs"][0]["mode"]=="ignore");}
    {auto j=user;j["settings"]["heelMax"]=5;j["pairs"][0]["Heel"]=3.5;Check(configuration_core::Merge(base,j)["manualPairs"][0]["Heel"]==3.5);}
    for (const auto* key : {"useOfflineHeightLibrary", "applyHeightResidualWarnings"}) {
        for (bool value : {false, true}) {
            auto j=user; j["settings"][key]=value;
            Check(configuration_core::Merge(base,j).at(key)==value);
        }
        for (const auto& value : {Json("false"), Json(1), Json(nullptr)}) {
            auto j=user; j["settings"][key]=value; refuse(j);
        }
        auto b=base; b[key]="true";
        bool bad=false; try { configuration_core::Validate(b); } catch (...) { bad=true; }
        Check(bad);
    }
    configuration_core::Debounce db;Check(!db.Ready("a"));Check(db.Ready("a"));Check(!db.Ready("a"));Check(!db.Ready("b"));Check(!db.Ready("c"));Check(db.Ready("c"));Check(!db.Ready("c"));
    for(int i=0;i<100;++i){auto c=configuration_core::Merge(base,user);Check(c["manualPairs"].size()==1);Check(c["manualPairs"][0]["Heel"]==1.1068);}
    std::cout<<checks<<" configuration checks passed\n";
}
