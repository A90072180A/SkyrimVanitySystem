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
 // Publication and an immediate reader can race with a Windows rename handle.
 // Retry only bounded I/O failures; once bytes are read, empty/malformed JSON is
 // a hard failure, not a reason to wait until a later report hides corruption.
 auto readReport=[](const std::filesystem::path& path){
    std::optional<std::string> bytes;
    for(unsigned attempt=0;attempt<100&&!bytes;++attempt){
        try{bool present=false;bytes=rf::Read(path,false,present);}
        catch(const std::exception&){if(attempt==99)throw;std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    }
    return Json::parse(*bytes);
 };
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
    auto report=readReport("Data/SKSE/Plugins/VanityUBEHeelAdapter/runtime-state.json");Check(report["snapshotOnly"]==true);
    Check(wait([]{return std::filesystem::exists("Data/SKSE/Plugins/VanityUBEHeelAdapter/observed-items.json");}));
    Check(!std::filesystem::exists("Data/SKSE/Plugins/VanityUBEHeelAdapter/runtime-state.json.tmp"));
  }

  // Model MO2's immutable virtual aliases: startup virtual files remain unchanged,
  // but the native resolver has identified a distinct physical backing path.
  // Tests use the actual production worker and I/O after path discovery, not a
  // mock watcher. Actual usvfs injection / Skyrim task timing are not simulated.
  const auto physical=dir/"physical-output";
  const auto overlay=physical/"VanityUBEHeelAdapter.user.json";
  const auto physicalBase=dir/"base-mod"/"VanityUBEHeelAdapter.json";
  std::filesystem::create_directories(physical);
  std::filesystem::create_directories(physicalBase.parent_path());
  {std::ofstream f(physicalBase);f<<R"({"applyMorph":true,"automaticHeight":true})";}
  rf::cp::Paths routed{physicalBase,overlay,physical/"VanityUBEHeelAdapter",
      dir/rf::basePath,dir/"unmapped-user.json","output-sibling-overlay"};
  {std::scoped_lock lock(rf::inputMutex);rf::paths=routed;rf::userWasPresent=false;}
  {std::scoped_lock lock(rf::mutex);rf::pending.reset();rf::acceptedRead=Json::object();rf::acceptedEffective.clear();}
  auto initial2=rf::ReadInitial();Check(initial2&&initial2->at("heelMax")==2);
  rf::Acknowledge(*initial2,10);
  auto readReceipt=[&](){return readReport(routed.output/"configuration-status.json");};
  auto atomic=[&](const std::string& bytes){auto tmp=overlay;tmp+=".editor.tmp";{std::ofstream f(tmp,std::ios::binary);f<<bytes;}
#ifdef _WIN32
    Check(::MoveFileExW(tmp.c_str(),overlay.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0);
#else
    std::filesystem::rename(tmp,overlay);
#endif
  };
  const std::string glass=R"({"schema":1,"pairs":[{"stocking":"Sock.esp|00000001","footwear":"Glass.esp|00000002","mode":"manual","NoHeel":0,"Heel":1.1}]})";
  std::uint64_t revision=10;
  auto accept=[&](const Json& next){rf::Acknowledge(next,++revision);};
  auto wait=[&](auto test){for(unsigned i=0;i<100;++i){try{if(test())return true;}catch(const std::exception&){}std::this_thread::sleep_for(std::chrono::milliseconds(50));}return false;};
  auto change=[&](const std::string& bytes,double heel){atomic(bytes);std::optional<Json> value;
    Check(wait([&]{value=rf::TakePending();return value.has_value();}));Check(value->at("manualPairs").at(0).at("Heel")==heel);
    // File observation / queuing alone must never be called main-thread acceptance.
    Check(readReceipt().at("accepted").value("userFingerprint",Json(nullptr))!=rf::Fingerprint(bytes));
    accept(*value);Check(wait([&]{auto c=readReceipt();return c.at("accepted").value("userFingerprint",Json(nullptr))==rf::Fingerprint(bytes)&&c.at("accepted").at("revision")==revision;}));
  };
  {
    rf::Worker worker;
    Check(wait([&]{return readReceipt().at("accepted").at("revision")==10;}));
    Check(readReceipt().at("userPath")==rf::cp::Text(overlay));
    // Overlay did not exist at process start. Its later creation is read from
    // the resolved output sibling even while the virtual alias stays absent.
    Check(!std::filesystem::exists(routed.virtualUser));change(glass,1.1);
    Check(!std::filesystem::exists(routed.virtualUser));
    auto low=glass;low.replace(low.find("1.1"),3,"0.9");
    const auto originalTime=std::filesystem::last_write_time(overlay);atomic(low);
    std::filesystem::last_write_time(overlay,originalTime);Check(glass.size()==low.size());
    std::optional<Json> v;Check(wait([&]{v=rf::TakePending();return v.has_value();}));Check(v->at("manualPairs").at(0).at("Heel")==.9);accept(*v);
    Check(wait([&]{return readReceipt().at("accepted").value("userFingerprint",Json(nullptr))==rf::Fingerprint(low);}));
    // Another subsystem changing the working directory cannot redirect polling.
    const auto away=dir/"different-cwd";std::filesystem::create_directory(away);std::filesystem::current_path(away);
    change(glass,1.1);
    for(const auto& invalid:std::vector<std::string>{"{",R"({"pairs":[{"stocking":"Sock.esp|00000001","footwear":"Glass.esp|00000002","NoHeel":1.1,"Heel":0}]})"}){
        atomic(invalid);Check(wait([&]{return readReceipt().at("state")=="rejected";}));Check(!rf::TakePending());
        Check(readReceipt().at("accepted").value("userFingerprint",Json(nullptr))==rf::Fingerprint(glass));}
    atomic(glass);Check(wait([&]{return readReceipt().at("state")=="accepted";}));
    // A transient disappearance is not interpreted as removing all overrides.
    std::filesystem::remove(overlay);Check(wait([&]{return readReceipt().at("state")=="rejected";}));Check(!rf::TakePending());
    atomic("{}");Check(wait([&]{v=rf::TakePending();return v.has_value();}));Check(!v->contains("manualPairs"));accept(*v);
    Check(wait([&]{return readReceipt().at("accepted").value("userFingerprint",Json(nullptr))==rf::Fingerprint("{}");}));
    Check(!std::filesystem::exists(routed.output/"configuration-status.json.tmp"));
    auto heartbeat=readReceipt().at("heartbeatUnixMs").get<std::uint64_t>();
    Check(wait([&]{return readReceipt().at("heartbeatUnixMs").get<std::uint64_t>()>heartbeat;}));
    Check(readReceipt().at("fingerprintAlgorithm")=="fnv1a64");
    // Exercise actual explicit-reload delivery without starting a second worker.
    // A request with identical content must still be delivered for forced reapply.
    auto request=[&]{return rf::RequestReload();};
    const auto firstRequest=request();std::optional<rf::ForcedReload> forced;
    Check(wait([&]{forced=rf::TakeForcedReload();return forced.has_value();}));
    Check(forced->requestId==firstRequest);Check(forced->effective==*v);
    // Taking is not an ACK; a missed game task is retried rather than forgotten.
    forced.reset();Check(wait([&]{forced=rf::TakeForcedReload();return forced.has_value();}));
    Check(forced->requestId==firstRequest);
    rf::Acknowledge(forced->effective,++revision);rf::AcknowledgeForcedReload(firstRequest,revision);
    Check(wait([&]{return readReceipt().at("manualReload").at("state")=="accepted-reapply-requested";}));
    Check(!rf::TakeForcedReload());
    atomic(glass);const auto secondRequest=request();
    Check(wait([&]{forced=rf::TakeForcedReload();return forced&&forced->requestId==secondRequest;}));
    Check(forced->effective.at("manualPairs").at(0).at("Heel")==1.1);
    rf::Acknowledge(forced->effective,++revision);rf::AcknowledgeForcedReload(secondRequest,revision);
    atomic("{ malformed");const auto thirdRequest=request();
    Check(wait([&]{auto r=readReceipt().at("manualReload");return r.at("requestId")==thirdRequest&&r.at("state")=="rejected";}));
    Check(!rf::TakeForcedReload());
    Check(readReceipt().at("accepted").at("revision")==revision);
    atomic(glass);const auto fourthRequest=request();
    Check(wait([&]{forced=rf::TakeForcedReload();return forced&&forced->requestId==fourthRequest;}));
    rf::Acknowledge(forced->effective,++revision);rf::AcknowledgeForcedReload(fourthRequest,revision);
    Check(wait([&]{return readReceipt().at("manualReload").at("configurationRevision")==revision;}));

  }
  std::filesystem::current_path(dir);
  // Native Windows HANDLE path resolution is exercised by Discover on Unicode
  // paths; on POSIX canonical paths are tested instead. Never writes an overlay.
  auto nativeRoot=dir/std::filesystem::path(u8"native-\u8DEF\u5F84");
  auto base=nativeRoot/rf::basePath;std::filesystem::create_directories(base.parent_path());
  {std::ofstream f(base);f<<"{}";}
  auto resolved=rf::cp::Discover(nativeRoot);
  Check(std::filesystem::equivalent(resolved.base,base));Check(resolved.user.is_absolute());
  Check(!std::filesystem::exists(resolved.user));Check(resolved.userMode=="output-sibling-overlay");
  {std::ofstream f(resolved.user);f<<"{}";}
  auto resolved2=rf::cp::Discover(nativeRoot);Check(std::filesystem::equivalent(resolved2.user,resolved.user));
  Check(resolved2.userMode=="existing-winning-overlay");
  for(const auto& file:std::filesystem::directory_iterator(resolved.output))Check(file.path().extension()!=".tmp");

  std::cout<<checks<<" actual reload/atomic report/routing/receipt checks passed\n";
 }catch(const std::exception&e){std::cerr<<"after check "<<checks<<": "<<e.what()<<'\n';std::filesystem::current_path(old);return 1;}
 std::filesystem::current_path(old);std::filesystem::remove_all(dir);return 0;
}
