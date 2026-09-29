#include "PreparedCpb.h"
#include <nlohmann/json.hpp>
#include <array>
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <thread>

namespace vanity_ube_heel_adapter::prepared_cpb {
namespace {
using Json=nlohmann::json;
// Locks the locally reproduced profile, exact source equivalences and blocked
// morph lists together. A different BodySlide build needs a new audited bundle.
constexpr std::uint64_t ProfileFingerprint=0x92352eedae2cf534ULL;
void Require(bool ok,const char* what){if(!ok)throw std::runtime_error(what);}
std::filesystem::path Path(std::string resource){
    resource=bulk_height::Resource(std::move(resource));
    std::replace(resource.begin(),resource.end(),'\\','/');return std::filesystem::path(resource);
}
std::vector<std::uint8_t> Read(const std::filesystem::path& path,std::size_t limit){
    std::ifstream file(path,std::ios::binary);Require(bool(file),"file-unreadable");
    std::vector<std::uint8_t> out;std::array<char,65536> block{};
    while(file){file.read(block.data(),static_cast<std::streamsize>(block.size()));const auto n=file.gcount();
        Require(n>=0&&out.size()+static_cast<std::size_t>(n)<=limit,"file-size-limit");
        out.insert(out.end(),block.data(),block.data()+n);
    }
    Require(file.eof(),"file-read-failed");return out;
}
std::uint64_t Number(const Json& value){
    const auto s=value.get<std::string>();Require(s.size()==16&&std::all_of(s.begin(),s.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}),"invalid-fingerprint");
    return std::stoull(s,nullptr,16);
}
bulk_height::Asset Asset(const Json& j){return {bulk_height::Resource(j.at("resource").get<std::string>()),Number(j.at("fingerprint"))};}
void CheckAsset(const std::filesystem::path& meshes,const bulk_height::Asset& a){
    Require(bulk_height::Fingerprint(Read(meshes/Path(a.resource),64u*1024u*1024u))==a.fingerprint,"asset-fingerprint-mismatch");
}
bool CheckShoe(const Json& shoe,const std::filesystem::path& meshes,std::string& error){
    try{Require(shoe.at("assets").size()==3,"shoe-requires-three-sources");for(const auto& a:shoe.at("assets"))CheckAsset(meshes,Asset(a));return true;}
    catch(const std::exception& e){error=e.what();return false;}
}
}
Snapshot Probe(const std::filesystem::path& profile,const std::filesystem::path& meshes){
    Snapshot out;
    try{
        const auto bytes=Read(profile,65536);
        Require(bulk_height::Fingerprint(bytes)==ProfileFingerprint,"unreviewed-prepared-profile");
        const auto j=Json::parse(bytes);
        Require(j.at("schema")==1&&j.at("algorithm")=="cpb-surface-v1-full-domain","unsupported-profile");
        out.generation=j.at("generation").get<std::string>();
        Require(j.at("aliases").size()==3,"prepared-alias-count");
        for(const auto& a:j.at("aliases")){
            bulk_height::SourceAlias alias{Asset(a.at("before")),Asset(a.at("after"))};
            CheckAsset(meshes,alias.after);out.aliases.push_back(std::move(alias));
        }
        out.triFingerprint=j.at("aliases").at(2).at("after").at("fingerprint").get<std::string>();
        const auto& shoes=j.at("shoes");
        out.agataReady=CheckShoe(shoes.at("agata"),meshes,out.agataError);
        out.glassReady=CheckShoe(shoes.at("glass"),meshes,out.glassError);
        out.agataBlockedMorphs=shoes.at("agata").at("blockedMorphs").get<std::vector<std::string>>();
        out.glassBlockedMorphs=shoes.at("glass").at("blockedMorphs").get<std::vector<std::string>>();
        out.ready=true;out.status="ready";
    }catch(const std::exception& e){out=Snapshot{};out.status="unavailable-or-invalid";out.error=e.what();}
    return out;
}
namespace {
std::atomic<void(*)()> notify{nullptr};
class Worker {
    std::mutex mutex;std::condition_variable cv;std::uint64_t epoch=0;bool pending=false;
    std::shared_ptr<const Snapshot> snapshot=std::make_shared<Snapshot>();
    std::jthread thread;
    void Run(std::stop_token stop){
        while(true){std::uint64_t captured;
            {std::unique_lock lock(mutex);cv.wait(lock,[&]{return stop.stop_requested()||pending;});if(stop.stop_requested())return;captured=epoch;pending=false;}
            auto next=std::make_shared<const Snapshot>(Probe("Data/SKSE/Plugins/VanityUBEHeelAdapter/prepared-cpb.json","Data/Meshes"));
            bool changed=false;{std::scoped_lock lock(mutex);if(captured==epoch){snapshot=std::move(next);changed=true;}}
            if(changed)if(auto callback=notify.load())try{callback();}catch(...){/* schedules a game task only */}
        }
    }
public:
    Worker():thread([this](std::stop_token stop){Run(stop);}){}
    ~Worker(){thread.request_stop();cv.notify_all();thread.join();}
    void Reset(){std::scoped_lock lock(mutex);++epoch;pending=true;snapshot=std::make_shared<Snapshot>();cv.notify_one();}
    std::shared_ptr<const Snapshot> Current(){std::scoped_lock lock(mutex);return snapshot;}
};
Worker& Instance(){static Worker worker;return worker;}
}
void Initialize(void(*callback)()){notify.store(callback);Instance().Reset();}
void Reset(){Instance().Reset();}
std::shared_ptr<const Snapshot> Current(){return Instance().Current();}
}
