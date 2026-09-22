// Real bounded file parsing + atomic output + actual polling worker. No game.
#include <iostream>
#include <format>
namespace logger {template<class...T>void warn(const char*,T&&...){} }
#include "../src/RuntimeFiles.cpp"
namespace rf=vanity_ube_heel_adapter::runtime_files;
using Json=nlohmann::json;
int checks=0;void Check(bool b){++checks;if(!b)throw std::runtime_error("runtime files check "+std::to_string(checks));}
std::atomic<unsigned> notices{0};void Notice(){++notices;}
int main(){const auto old=std::filesystem::current_path();auto dir=std::filesystem::temp_directory_path()/std::format("vha-config-{}",std::chrono::steady_clock::now().time_since_epoch().count());
 std::filesystem::create_directories(dir);std::filesystem::current_path(dir);
 auto put=[](const char*path,const std::string&s){std::filesystem::create_directories(std::filesystem::path(path).parent_path());std::ofstream f(path,std::ios::binary|std::ios::trunc);f<<s;};
 try{
  put(rf::basePath,R"({"applyMorph":true,"automaticHeight":true})");auto initial=rf::ReadInitial();Check(initial&&initial->at("heelMax")==2.0);
  put(rf::userPath,R"({"schema":1,"settings":{"heelMax":3}})");Check(rf::ReadInitial()->at("heelMax")==3);
  put(rf::userPath,"{");Check(!rf::ReadInitial());
  put(rf::userPath,R"({"settings":{"heelMax":0.1}})");Check(!rf::ReadInitial());
  put(rf::userPath,R"({"settings":{"heelMax":2.5}})");
  { rf::callback.store(Notice);rf::Worker worker;
    auto wait=[&](auto test){for(int i=0;i<80;++i){if(test())return true;std::this_thread::sleep_for(std::chrono::milliseconds(50));}return false;};
    Check(wait([&]{return notices.load()>0;}));auto next=rf::TakePending();Check(next&&next->at("heelMax")==2.5);
    const auto count=notices.load();put(rf::userPath,"{ invalid");std::this_thread::sleep_for(std::chrono::milliseconds(1300));Check(notices.load()==count);Check(!rf::TakePending());
    put(rf::userPath,R"({"settings":{"heelMax":2.2}})");Check(wait([&]{return notices.load()>count;}));Check(rf::TakePending()->at("heelMax")==2.2);
    Json state={{"schema",1},{"visuals",Json::array({{{"armor","Sock.esp|00000001"},{"addon","Sock.esp|00000002"},{"model","test.nif"}}})},{"decisions",Json::array()}};
    rf::Status(state);Check(wait([]{return std::filesystem::exists("Data/SKSE/Plugins/VanityUBEHeelAdapter/runtime-state.json");}));
    std::ifstream f("Data/SKSE/Plugins/VanityUBEHeelAdapter/runtime-state.json");auto report=Json::parse(f);Check(report["snapshotOnly"]==true);
    Check(wait([]{return std::filesystem::exists("Data/SKSE/Plugins/VanityUBEHeelAdapter/observed-items.json");}));
    Check(!std::filesystem::exists("Data/SKSE/Plugins/VanityUBEHeelAdapter/runtime-state.json.tmp"));
  }
  std::cout<<checks<<" actual reload/atomic report checks passed\n";
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';std::filesystem::current_path(old);return 1;}
 std::filesystem::current_path(old);std::filesystem::remove_all(dir);return 0;
}
