#include "FootSnapshotCore.h"
#include "ConfigState.h"
#include "ConfigurationCore.h"
#include "HeightProfiles.h"
#include "TriMorphCore.h"
#include "FootCapturePolicy.h"
#ifdef _WIN32
#include <Windows.h>
#endif
#include <condition_variable>
#include <deque>
#include <fstream>
#include <map>
#include <set>
#include <thread>

namespace vanity_ube_heel_adapter::height_profiles {
namespace {
constexpr auto kPath="Data/SKSE/Plugins/VanityUBEHeelAdapter/height-profiles.json";
constexpr auto kAlgorithm="bounded-native-branches-v2";
std::mutex mutex;
std::map<std::string,Json> profiles, observed, observedDonors;
std::map<std::string,Capability> capabilities;
std::atomic<std::uint64_t> session{1};
std::atomic<void(*)()> notify{nullptr};
std::mutex diskMutex;
std::optional<std::vector<std::uint8_t>> ReadResource(const std::string& resource) {
    const auto key=foot_capture_policy::ResourceKey(resource);
    if(key.empty()||!(key.ends_with(".tri")||key.ends_with(".nif")))return {};
    const auto path=std::filesystem::path("Data/Meshes")/key;
    std::error_code ec;const auto size=std::filesystem::file_size(path,ec);
    if(ec||size>64u*1024u*1024u)return {};
    const auto time=std::filesystem::last_write_time(path,ec);if(ec)return {};
    std::ifstream f(path,std::ios::binary);std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if(!f||!f.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(size)))return {};
    const auto afterSize=std::filesystem::file_size(path,ec);if(ec||afterSize!=size)return {};
    const auto afterTime=std::filesystem::last_write_time(path,ec);if(ec||afterTime!=time)return {};
    return bytes;
}
std::string Fingerprint(const std::vector<std::uint8_t>& b){return std::format("{:016x}",tri_morph_core::HashBytes(b));}
void Signal(){if(auto cb=notify.load())cb();}
// The anchor is part of the numerical calibration, not just presentation.
// Changing it invalidates an old disk profile even when meshes are unchanged.
// Called only on cache/snapshot workers, never on a game task.
std::string ReferenceStamp(){
    try {
        const auto cfg=config_state::Get();
        const auto& r=cfg.at("surfaceCalibrationReference");
        if(!r.is_object() || !r.at("armor").is_string() || !r.at("addon").is_string() ||
           r.at("armor").get<std::string>().empty() || r.at("addon").get<std::string>().empty() ||
           !r.at("noHeel").is_number())return {};
        const auto q=r.at("noHeel").get<double>();
        if(!std::isfinite(q)||q<0||q>1)return {};
        return Json::array({r,cfg.value("heelMax",2.0),cfg.value("enableComponentSubset",true)}).dump();
    }catch(...){return {};}
}
std::string Key(const Json&p){return Json::array({p.at("footwear").at("armor"),p.at("footwear").at("addon"),p.at("stocking").at("armor"),p.at("stocking").at("addon"),p.at("context")}).dump();}
std::string FootKey(const std::string&a,const std::string&b,const std::string&c){return Json::array({a,b,c}).dump();}
bool FormatValid(const Json&p){
    try {
        auto hash=[](const Json&value){if(!value.is_string())return false;auto v=value.get<std::string>();return v.size()==16&&std::all_of(v.begin(),v.end(),[](unsigned char c){return std::isxdigit(c)!=0;});};
        if(!hash(p.at("bodyTriFingerprint"))||!hash(p.at("targetPositionFingerprint"))||!hash(p.at("donorSourceFingerprint")))return false;
        if(!p.is_object()||p.value("algorithm",std::string{})!=kAlgorithm||!p.at("context").is_string()||
           !p.at("targetPositionFingerprint").is_string()||!p.at("donorSourceFingerprint").is_string()||!p.at("inputs").is_array()||p.at("inputs").empty()||p.at("inputs").size()>24)return false;
        const auto c=height_plan_core::Controls{p.at("NoHeel").get<double>(),p.at("Heel").get<double>()};
        if(!height_plan_core::Valid(c,height_plan_core::AbsoluteHeelLimit)||!std::isfinite(p.at("normalizedResidual").get<double>())||p.at("normalizedResidual").get<double>()<0)return false;
        for(const auto* kind:{"stocking","footwear"}) for(const auto*field:{"armor","addon"})
            if(!p.at(kind).at(field).is_string()||p.at(kind).at(field).get<std::string>().empty())return false;
        if(!p.at("saturated").is_boolean())return false;
        for(const auto&input:p.at("inputs"))if(!input.is_object()||!input.at("resource").is_string()||!hash(input.at("fingerprint")))return false;
        return true;
    }catch(...){return false;}
}
bool InputsValid(const Json&p){
    if(!FormatValid(p))return false;
    for(const auto&i:p.at("inputs")) {
        try {auto b=ReadResource(i.at("resource").get<std::string>());if(!b||Fingerprint(*b)!=i.at("fingerprint").get<std::string>())return false;}
        catch(...){return false;}
    }return true;
}
void Persist(){
    std::scoped_lock diskLock(diskMutex);
    Json out={{"schema",1},{"algorithm",kAlgorithm},{"generatorVersion","0.16.0"},{"entries",Json::array()}};
    {std::scoped_lock lock(mutex);for(const auto&[k,p]:profiles)out["entries"].push_back(p);}
    const std::filesystem::path path{kPath},tmp{std::string(kPath)+".tmp"};
    std::filesystem::create_directories(path.parent_path());
    std::ofstream f(tmp,std::ios::binary|std::ios::trunc);if(!f)throw std::runtime_error("height cache open failed");
    f<<out.dump(2)<<'\n';f.flush();if(!f)throw std::runtime_error("height cache flush failed");f.close();if(f.fail())throw std::runtime_error("height cache close failed");
#ifdef _WIN32
    if(!::MoveFileExW(tmp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("height cache atomic replace failed");
#else
    std::filesystem::rename(tmp,path);
#endif
}
void Load(std::uint64_t epoch){
    std::ifstream f(kPath,std::ios::binary|std::ios::ate);if(!f)return;
    if(f.tellg()>8*1024*1024)return;f.seekg(0);
    auto root=Json::parse(f);
    if(root.value("schema",0)!=1||root.value("algorithm",std::string{})!=kAlgorithm||!root.at("entries").is_array()||root.at("entries").size()>64)return;
    const auto reference=ReferenceStamp();if(reference.empty())return;
    unsigned accepted=0,rejected=0;
    for(const auto&p:root.at("entries")) {
        if(epoch!=session.load())return;
        if(!p.contains("referenceConfiguration")||p.at("referenceConfiguration")!=reference||!InputsValid(p)){++rejected;continue;}
        std::scoped_lock lock(mutex);if(epoch!=session.load())return;profiles.try_emplace(Key(p),p);++accepted;
    }
    logger::info("[height cache] loaded={} staleOrInvalid={} (current foot and actor context still required)",accepted,rejected);
}
Capability Probe(const std::string&resource){
    Capability c;c.resource=resource;c.status="resource-unreadable";
    const auto bytes=ReadResource(resource);if(!bytes)return c;
    c.fingerprint=Fingerprint(*bytes);
    auto inventory=tri_morph_core::Parse(*bytes,"Feet","NoHeel");
    if(inventory.status=="malformed"||inventory.status=="unsupported-format"){c.status=inventory.status;return c;}
    if(inventory.shapeNames.empty()||inventory.shapeNames.size()>64){c.status="unsupported-shape-count";return c;}
    for(const auto&name:inventory.shapeNames){
        auto n=tri_morph_core::Parse(*bytes,name,"NoHeel");auto h=tri_morph_core::Parse(*bytes,name,"Heel");
        if(n.status=="malformed"||h.status=="malformed"){c.status="malformed";return c;}
        ShapeCapability s{name,n.Present(),h.Present(),0};
        for(const auto&d:n.offsets)s.maxIndex=(std::max)(s.maxIndex,d.index);
        for(const auto&d:h.offsets)s.maxIndex=(std::max)(s.maxIndex,d.index);
        c.shapes.push_back(std::move(s));
    }c.status="parsed";return c;
}
class Worker {
    std::mutex lock;std::condition_variable cv;std::deque<std::pair<std::uint64_t,std::string>> jobs;
    std::set<std::pair<std::uint64_t,std::string>> pending;std::jthread thread;
    void Run(std::stop_token stop){
        try{Load(session.load());}catch(const std::exception&e){logger::warn("[height cache] ignored invalid cache: {}",e.what());}
        for(;;){std::pair<std::uint64_t,std::string> job;
            {std::unique_lock guard(lock);cv.wait(guard,[&]{return stop.stop_requested()||!jobs.empty();});if(stop.stop_requested())return;job=std::move(jobs.front());jobs.pop_front();}
            if(job.first==session.load())try{
                if(job.second=="__reload__")Load(job.first);
                else {
                    auto value=Probe(job.second);
                    std::scoped_lock guard(mutex);if(job.first==session.load()){if(capabilities.size()>=128)capabilities.clear();capabilities[job.second]=std::move(value);}
                }
                Signal();
            }catch(const std::exception&e){logger::warn("[height capability] {}",e.what());}
            {std::scoped_lock guard(lock);pending.erase(job);}
        }
    }
public:
    Worker():thread([this](std::stop_token stop){Run(stop);}){}
    ~Worker(){thread.request_stop();cv.notify_all();thread.join();}
    void Push(std::string resource){std::scoped_lock guard(lock);auto j=std::make_pair(session.load(),std::move(resource));if(jobs.size()<32&&pending.insert(j).second){jobs.push_back(j);cv.notify_one();}}
};
Worker& Background(){static Worker worker;return worker;}
}
void Initialize(void(*callback)()){notify.store(callback);(void)Background();}
std::uint64_t BeginSession(){
    const auto value=session.fetch_add(1)+1;
    {std::scoped_lock lock(mutex);profiles.clear();observed.clear();observedDonors.clear();capabilities.clear();}
    Background().Push("__reload__");return value;
}
std::uint64_t Session(){return session.load();}
std::optional<Capability> RequestCapability(const std::string& resource){
    const auto key=foot_capture_policy::ResourceKey(resource);if(key.empty()||!key.ends_with(".tri"))return {};
    {std::scoped_lock lock(mutex);auto it=capabilities.find(key);if(it!=capabilities.end())return it->second;}
    Background().Push(key);return {};
}
void ObserveFoot(const Json&d){
    if(d.value("heightSession",std::uint64_t{})!=session.load()||(d.value("geometryRole",std::string{})!="foot" && d.value("geometryRole",std::string{})!="stocking")||
       !d.at("sourceModelEvidence").value("measurementEligible",false)||!d.at("captureSelection").value("currentGraphConfirmed",false)||
       d.at("captureSelection").value("identityIsCorrelated",true))return;
    const auto&id=d.at("identity");const auto context=Json::array({id.at("race"),id.at("sexIndex"),id.at("actorWeight"),d.at("actorMorphValues")}).dump();
    const auto key=FootKey(id.at("armor").get<std::string>(),id.at("addon").get<std::string>(),context);
    bool changed=false;
    {std::scoped_lock lock(mutex);
        if(d.value("heightSession",std::uint64_t{})!=session.load())return;
        auto& destination=d.at("geometryRole")=="foot"?observed:observedDonors;
        const auto value=d.at("geometryRole")=="foot"?d.at("positionFingerprint"):d.at("sourceModelEvidence").at("asset").at("sourceFingerprint");
        changed=!destination.contains(key)||destination[key]!=value;
        if(destination.size()>=32&&!destination.contains(key))destination.clear();destination[key]=value;
    }
    if(changed)Signal();
}
void Publish(Json p){
    if(p.value("heightSession",std::uint64_t{})!=session.load())return;
    const auto reference=ReferenceStamp();if(reference.empty())return;
    p["referenceConfiguration"]=reference;
    p["algorithm"]=kAlgorithm;
    p["key"]=std::format("{:016x}",foot_snapshot_core::HashText(Key(p)));
    if(!FormatValid(p))return;
    // Source hashes are checked once when restoring persisted entries, and are
    // produced by current source-verified snapshots for newly computed entries.
    bool changed=false;
    {std::scoped_lock lock(mutex);if(p.value("heightSession",std::uint64_t{})!=session.load())return;const auto key=Key(p);changed=!profiles.contains(key)||profiles[key]!=p;if(profiles.size()>=64&&!profiles.contains(key))profiles.erase(profiles.begin());profiles[key]=p;}
    if(changed){try{Persist();}catch(const std::exception&e){logger::warn("[height cache] write failed: {}",e.what());}Signal();}
}
std::optional<Json> Lookup(const std::string&foot,const std::string&fa,const std::string&stock,const std::string&sa,const std::string&context){
    std::scoped_lock lock(mutex);
    const auto id=Json::array({foot,fa,stock,sa,context}).dump();const auto it=profiles.find(id);if(it==profiles.end())return {};
    const auto obs=observed.find(FootKey(foot,fa,context));if(obs==observed.end()||obs->second!=it->second.at("targetPositionFingerprint"))return {};
    const auto donor=observedDonors.find(FootKey(stock,sa,context));
    if(donor==observedDonors.end()||donor->second!=it->second.at("donorSourceFingerprint"))return {};
    return it->second;
}
}
