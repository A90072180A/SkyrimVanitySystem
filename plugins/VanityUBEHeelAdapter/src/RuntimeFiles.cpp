#include "RuntimeFiles.h"
#include "ConfigurationCore.h"
#include "ConfigFilePaths.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <fstream>
#include <iomanip>
#include <sstream>
namespace vanity_ube_heel_adapter::runtime_files {
namespace {
namespace cp=config_file_paths;
constexpr auto basePath="Data/SKSE/Plugins/VanityUBEHeelAdapter.json";
constexpr auto userPath="Data/SKSE/Plugins/VanityUBEHeelAdapter.user.json";
std::mutex mutex,inputMutex;
std::optional<Json> pending,status;
std::optional<ForcedReload> forcedReload;
std::atomic<std::uint64_t> reloadRequested{0};
std::uint64_t reloadFinished{0}; // mutex protects this and the receipt
Json reloadReceipt={{"requestId",0},{"state","not-requested"}};
std::optional<cp::Paths> paths;
bool userWasPresent=false;
std::string lastStatus,acceptedEffective;
std::uint64_t acceptedRevision=0;
Json latestRead=Json::object(),acceptedRead=Json::object();
std::atomic<void(*)()> callback{nullptr};
std::uint64_t Now(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
std::string Fingerprint(const std::string& s){
    std::uint64_t h=14695981039346656037ULL;for(unsigned char b:s){h^=b;h*=1099511628211ULL;}
    std::ostringstream out;out<<std::hex<<std::setfill('0')<<std::setw(16)<<h;return out.str();
}
cp::Paths GetPaths(){
    // Caller owns inputMutex. Retries discovery after an initial I/O failure.
    if(!paths)paths=cp::Discover(std::filesystem::current_path());return *paths;
}
std::string Read(const std::filesystem::path& path,bool optional,bool& present){
    std::error_code ec;present=std::filesystem::exists(path,ec);
    if(ec)throw std::runtime_error("configuration path inaccessible: "+cp::Text(path));
    if(!present){if(optional)return "{}";throw std::runtime_error("configuration missing: "+cp::Text(path));}
    const auto before=std::filesystem::last_write_time(path);const auto size=std::filesystem::file_size(path);
    if(size>1024*1024)throw std::runtime_error("configuration larger than 1 MiB");
#ifdef _WIN32
    // Do not block an editor's atomic rename while this fresh read handle lives.
    cp::Handle h(::CreateFileW(path.c_str(),GENERIC_READ,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,nullptr));
    if(h.value==INVALID_HANDLE_VALUE)throw std::runtime_error("configuration read open failed: "+cp::Text(path)+" error="+std::to_string(::GetLastError()));
    std::string s(static_cast<std::size_t>(size),'\0');DWORD received=0;
    if(size&&(!::ReadFile(h.value,s.data(),static_cast<DWORD>(size),&received,nullptr)||received!=size))
        throw std::runtime_error("configuration read failed or changed: "+cp::Text(path));
#else
    std::ifstream f(path,std::ios::binary);if(!f)throw std::runtime_error("configuration unreadable: "+cp::Text(path));
    std::string s((std::istreambuf_iterator<char>(f)),{});
#endif
    if(s.size()!=size||std::filesystem::last_write_time(path)!=before||std::filesystem::file_size(path)!=size)throw std::runtime_error("configuration changed during read");
    return s;
}
struct Input {Json effective,receipt;std::string stamp;cp::Paths locations;};
Input Candidate(){
    // Every read opens a fresh handle to the pinned backing path. Do not cache
    // an open file across the editor's atomic replacement.
    std::scoped_lock io(inputMutex);auto p=GetPaths();bool aExists=false,bExists=false;
    auto a=Read(p.base,false,aExists),b=Read(p.user,!userWasPresent,bExists);
    if(bExists)userWasPresent=true; // disappearing file is not an instruction to clear
    auto j=configuration_core::Merge(Json::parse(a),Json::parse(b));auto stamp=j.dump();
    Json receipt={{"basePath",cp::Text(p.base)},{"userPath",cp::Text(p.user)},
        {"baseFingerprint",Fingerprint(a)},{"userFingerprint",bExists?Json(Fingerprint(b)):Json(nullptr)},
        {"userExists",bExists},{"effectiveFingerprint",Fingerprint(stamp)}};
    {std::scoped_lock lock(mutex);latestRead=receipt;latestRead["effective"]=stamp;}
    return {std::move(j),std::move(receipt),std::move(stamp),std::move(p)};
}
void Write(const std::filesystem::path&path,const Json&j){
    std::filesystem::create_directories(path.parent_path());auto tmp=path;tmp+=".tmp";
    std::ofstream f(tmp,std::ios::binary|std::ios::trunc);if(!f)throw std::runtime_error("status open failed");
    f<<j.dump(2)<<'\n';f.flush();if(!f)throw std::runtime_error("status flush failed");f.close();if(f.fail())throw std::runtime_error("status close failed");
#ifdef _WIN32
    if(!::MoveFileExW(tmp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("status replace failed");
#else
    std::filesystem::rename(tmp,path);
#endif
}
class Worker {
    std::mutex waitMutex;std::condition_variable cv;std::jthread thread;
    void Run(std::stop_token stop){
        configuration_core::Debounce debounce;std::string lastError,lastInventory,reportedError;
        Json lastReport;std::optional<Input> current;
        const auto processToken=std::to_string(Now())+"-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        Json history={{"schema",1},{"coalesced",true},{"semantics","submitted control states, not proof of visible geometry; intermediate updates may coalesce"},{"entries",Json::array()}};
        Json inventory={{"schema",1},{"generatorVersion","0.18.0"},{"entries",Json::object()}};
        bool loadedInventory=false;
        while(!stop.stop_requested()){
            bool notify=false;
            try{
                auto next=Candidate();const bool fresh=debounce.Ready(next.stamp);
                const bool stable=debounce.repeats>=2&&debounce.emitted==next.stamp;
                {std::scoped_lock lock(mutex);
                    const auto explicitRequest=reloadRequested.load();
                    if(stable&&explicitRequest>reloadFinished) {
                        forcedReload=ForcedReload{next.effective,explicitRequest};
                        reloadReceipt={{"requestId",explicitRequest},{"state","validated-awaiting-main-thread"},
                            {"effectiveFingerprint",Fingerprint(next.stamp)}};
                        pending.reset();notify=true;
                    }
                    else if(stable&&acceptedEffective==next.stamp){acceptedRead=next.receipt;acceptedRead["revision"]=acceptedRevision;pending.reset();}
                    else if(stable&&(fresh||!pending)){pending=next.effective;notify=true;}
                }
                current=std::move(next);lastError.clear();
            }catch(const std::exception&e){
                debounce={};{std::scoped_lock lock(mutex);pending.reset();forcedReload.reset();
                    const auto request=reloadRequested.load();
                    if(request>reloadFinished){reloadFinished=request;reloadReceipt={{"requestId",request},{"state","rejected"},{"error",e.what()}};}}

                if(lastError!=e.what()){lastError=e.what();logger::warn("[config reload] rejected; keeping last good config: {}",lastError);}
            }
            if(notify)if(auto fn=callback.load())fn();
            cp::Paths locations;bool havePaths=false;
            {std::scoped_lock lock(inputMutex);if(paths){locations=*paths;havePaths=true;}}
            if(havePaths){
                if(!loadedInventory){loadedInventory=true;
                    try{std::ifstream f(locations.output/"observed-items.json",std::ios::binary|std::ios::ate);
                        if(f&&f.tellg()<2*1024*1024){f.seekg(0);auto j=Json::parse(f);if(j.value("schema",0)==1&&j.at("entries").is_object()&&j.at("entries").size()<=1024)inventory=std::move(j);}}
                    catch(...){/* diagnostic inventory is not authority */}}
                Json ack;std::string effective;std::optional<Json> snapshot;
                {std::scoped_lock lock(mutex);ack=acceptedRead;effective=acceptedEffective;snapshot=std::move(status);status.reset();}
                Json receipt={{"schema",1},{"generatorVersion","0.18.0"},{"processToken",processToken},{"heartbeatUnixMs",Now()},
                    {"basePath",cp::Text(locations.base)},{"userPath",cp::Text(locations.user)},{"outputDirectory",cp::Text(locations.output)},
                    {"virtualBasePath",cp::Text(locations.virtualBase)},{"virtualUserPath",cp::Text(locations.virtualUser)},
                    {"userPathMode",locations.userMode},{"fingerprintAlgorithm","fnv1a64"},{"accepted",ack},
                    {"state",!lastError.empty()?"rejected":current&&current->stamp==effective?"accepted":"pending"},
                    {"error",lastError.empty()?Json(nullptr):Json(lastError)},
                    {"semantics","accepted confirms main-thread configuration, not morph submission or visible fit"}};
                if(current)receipt["observed"]=current->receipt;
                {std::scoped_lock lock(mutex);receipt["manualReload"]=reloadReceipt;}
                try{Write(locations.output/"configuration-status.json",receipt);}
                catch(const std::exception&e){logger::warn("[config receipt] write failed: {}",e.what());}
                if(snapshot)lastReport=*snapshot;
                if(!snapshot&&lastError!=reportedError&&!lastReport.is_null())snapshot=lastReport;
                if(snapshot)try{
                    (*snapshot)["configurationError"]=lastError.empty()?Json(nullptr):Json(lastError);
                    (*snapshot)["configurationStatusFile"]=cp::Text(locations.output/"configuration-status.json");(*snapshot)["snapshotOnly"]=true;
                    Write(locations.output/"runtime-state.json",*snapshot);reportedError=lastError;
                    history["entries"].push_back(*snapshot);if(history["entries"].size()>64)history["entries"].erase(history["entries"].begin());
                    Write(locations.output/"height-events.json",history);
                    for(const auto&item:snapshot->value("visuals",Json::array())){
                        if(!item.is_object()||!item.contains("armor")||!item.contains("addon")||!item.contains("model"))continue;
                        auto key=Json::array({item.at("armor"),item.at("addon"),item.at("model")}).dump();
                        if(inventory["entries"].size()>=1024&&!inventory["entries"].contains(key))inventory["entries"].erase(inventory["entries"].begin());inventory["entries"][key]=item;}
                    const auto value=inventory.dump();if(value!=lastInventory){Write(locations.output/"observed-items.json",inventory);lastInventory=value;}
                }catch(const std::exception&e){logger::warn("[runtime files] write failed: {}",e.what());}
            }
            std::unique_lock lock(waitMutex);cv.wait_for(lock,std::chrono::milliseconds(500),[&]{return stop.stop_requested();});
        }
    }
public:
    Worker():thread([this](std::stop_token s){Run(s);}){}
    ~Worker(){thread.request_stop();cv.notify_all();thread.join();}
};
Worker& GetWorker(){static Worker worker;return worker;}
}
void Initialize(void(*notify)()){callback.store(notify);(void)GetWorker();}
std::optional<Json> ReadInitial(){try{auto value=Candidate();std::scoped_lock lock(mutex);pending.reset();return value.effective;}catch(const std::exception&e){logger::warn("[config initial] {}",e.what());return {};}}
std::optional<Json> TakePending(){std::scoped_lock lock(mutex);auto j=std::move(pending);pending.reset();return j;}
void Acknowledge(const Json& effective,std::uint64_t revision){
    std::scoped_lock lock(mutex);acceptedEffective=effective.dump();acceptedRevision=revision;
    if(latestRead.value("effective",std::string{})==acceptedEffective){acceptedRead=latestRead;acceptedRead.erase("effective");acceptedRead["revision"]=revision;}
    else acceptedRead=Json::object();
    if(pending&&pending->dump()==acceptedEffective)pending.reset();
}
std::uint64_t RequestReload(){
    const auto id=reloadRequested.fetch_add(1)+1;
    {std::scoped_lock lock(mutex);reloadReceipt={{"requestId",id},{"state","read-requested"}};}
    return id;
}
std::optional<ForcedReload> TakeForcedReload(){
    std::scoped_lock lock(mutex);
    if(!forcedReload||forcedReload->requestId!=reloadRequested.load()||forcedReload->requestId<=reloadFinished)return {};
    auto out=std::move(forcedReload);forcedReload.reset();return out;
}
void AcknowledgeForcedReload(std::uint64_t requestId,std::uint64_t revision){
    std::scoped_lock lock(mutex);
    if(requestId!=reloadRequested.load())return;
    reloadFinished=requestId;forcedReload.reset();
    reloadReceipt={{"requestId",requestId},{"state","accepted-reapply-requested"},{"configurationRevision",revision},
        {"semantics","configuration reread accepted; current scopes scheduled for repeated local submission, not visual proof"}};
}
void Status(Json snapshot){auto s=snapshot.dump();std::scoped_lock lock(mutex);if(lastStatus==s)return;lastStatus=std::move(s);status=std::move(snapshot);}
}
