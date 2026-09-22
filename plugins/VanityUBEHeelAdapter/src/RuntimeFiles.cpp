#include "RuntimeFiles.h"
#include "ConfigurationCore.h"
#include "FootSnapshotCore.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#ifdef _WIN32
#include <Windows.h>
#endif
namespace vanity_ube_heel_adapter::runtime_files {
namespace {
constexpr auto basePath="Data/SKSE/Plugins/VanityUBEHeelAdapter.json";
constexpr auto userPath="Data/SKSE/Plugins/VanityUBEHeelAdapter.user.json";
std::mutex mutex;
std::optional<Json> pending,status;
std::string lastStatus;
std::atomic<void(*)()> callback{nullptr};
std::string Read(const char* path,bool optional){
    std::error_code ec;auto exists=std::filesystem::exists(path,ec);
    if(ec)throw std::runtime_error("configuration path inaccessible");
    if(!exists){if(optional)return "{}";throw std::runtime_error("base configuration missing");}
    const auto before=std::filesystem::last_write_time(path);const auto size=std::filesystem::file_size(path);
    if(size>1024*1024)throw std::runtime_error("configuration larger than 1 MiB");
    std::ifstream f(path,std::ios::binary);if(!f)throw std::runtime_error("configuration unreadable");
    std::string s((std::istreambuf_iterator<char>(f)),{});
    if(s.size()!=size||std::filesystem::last_write_time(path)!=before||std::filesystem::file_size(path)!=size)throw std::runtime_error("configuration changed during read");
    return s;
}
std::pair<Json,std::string> Candidate(){
    auto a=Read(basePath,false),b=Read(userPath,true);
    auto j=configuration_core::Merge(Json::parse(a),Json::parse(b));
    // Compare normalized effective config: comments/whitespace-only changes do
    // not reset source observations or previously applied height controls.
    return {j,j.dump()};
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
        Json lastReport;Json history={{"schema",1},{"coalesced",true},{"semantics","submitted control states, not proof of visible geometry; intermediate updates may coalesce"},{"entries",Json::array()}};
        Json inventory={{"schema",1},{"generatorVersion","0.16.0"},{"entries",Json::object()}};
        try{std::ifstream f("Data/SKSE/Plugins/VanityUBEHeelAdapter/observed-items.json",std::ios::binary|std::ios::ate);
            if(f&&f.tellg()<2*1024*1024){f.seekg(0);auto j=Json::parse(f);if(j.value("schema",0)==1&&j.at("entries").is_object()&&j.at("entries").size()<=1024)inventory=std::move(j);}}
        catch(...){/* diagnostic inventory is not authority */}
        while(!stop.stop_requested()){
            try{auto [j,stamp]=Candidate();if(debounce.Ready(stamp)){
                    {std::scoped_lock lock(mutex);pending=std::move(j);}if(auto fn=callback.load())fn();
                }lastError.clear();}
            catch(const std::exception&e){if(lastError!=e.what()){lastError=e.what();logger::warn("[config reload] rejected; keeping last good config: {}",lastError);}}
            std::optional<Json> snapshot;
            {std::scoped_lock lock(mutex);snapshot=std::move(status);status.reset();}
            if(snapshot)lastReport=*snapshot;
            if(!snapshot&&lastError!=reportedError&&!lastReport.is_null())snapshot=lastReport;
            if(snapshot)try{
                (*snapshot)["configurationError"]=lastError.empty()?Json(nullptr):Json(lastError);
                (*snapshot)["snapshotOnly"]=true;
                Write("Data/SKSE/Plugins/VanityUBEHeelAdapter/runtime-state.json",*snapshot);
                reportedError=lastError;
                history["entries"].push_back(*snapshot);
                if(history["entries"].size()>64)history["entries"].erase(history["entries"].begin());
                Write("Data/SKSE/Plugins/VanityUBEHeelAdapter/height-events.json",history);
                for(const auto&item:snapshot->value("visuals",Json::array())){
                    if(!item.is_object()||!item.contains("armor")||!item.contains("addon")||!item.contains("model"))continue;
                    auto key=Json::array({item.at("armor"),item.at("addon"),item.at("model")}).dump();
                    if(inventory["entries"].size()>=1024&&!inventory["entries"].contains(key))inventory["entries"].erase(inventory["entries"].begin());
                    inventory["entries"][key]=item;
                }
                const auto value=inventory.dump();if(value!=lastInventory){Write("Data/SKSE/Plugins/VanityUBEHeelAdapter/observed-items.json",inventory);lastInventory=value;}
            }catch(const std::exception&e){logger::warn("[runtime files] write failed: {}",e.what());}
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
std::optional<Json> ReadInitial(){try{return Candidate().first;}catch(const std::exception&e){logger::warn("[config initial] {}",e.what());return {};}}
std::optional<Json> TakePending(){std::scoped_lock lock(mutex);auto j=std::move(pending);pending.reset();return j;}
void Status(Json snapshot){auto s=snapshot.dump();std::scoped_lock lock(mutex);if(lastStatus==s)return;lastStatus=std::move(s);status=std::move(snapshot);}
}
