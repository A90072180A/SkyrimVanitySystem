// Exercises the actual cache/capability implementation without Skyrim objects.
// Run from a temporary working directory; no installed game data is read.
#include <atomic>
#include <mutex>
#include <filesystem>
#include <format>
#include <iostream>
#include <stdexcept>
#ifndef _WIN32
#include <unistd.h>
unsigned GetCurrentProcessId(){return static_cast<unsigned>(getpid());}
#endif
namespace logger {
template<class... T> void info(const char*,T&&...) {}
template<class... T> void warn(const char*,T&&...) {}
}
#include "../src/HeightProfiles.cpp"
namespace hp=vanity_ube_heel_adapter::height_profiles;
using Json=nlohmann::json;
int checks=0;
void check(bool ok){++checks;if(!ok)throw std::runtime_error("cache check "+std::to_string(checks));}
void put(const std::string&path,const std::vector<std::uint8_t>&bytes){
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    if(!f)throw std::runtime_error("fixture write");
}
std::vector<std::uint8_t> tri(){
    std::vector<std::uint8_t>b{'P','I','R','T'};
    auto u16=[&](unsigned v){b.push_back(v&255);b.push_back((v>>8)&255);};
    auto name=[&](std::string n){b.push_back(static_cast<std::uint8_t>(n.size()));for(char c:n)b.push_back(c);};
    u16(1);name("Sock");u16(2);
    for(auto n:{"NoHeel","Heel"}){name(n);b.insert(b.end(),{0,0,128,63});u16(1);u16(0);u16(0);u16(n==std::string("Heel")?65535:1);u16(0);}
    return b;
}
int main(){
    const auto old=std::filesystem::current_path();
    const auto dir=std::filesystem::temp_directory_path()/std::format("svs-height-cache-test-{}",::GetCurrentProcessId());
    std::filesystem::remove_all(dir);std::filesystem::create_directories(dir);std::filesystem::current_path(dir);
    try {
        const Json ref={{"armor","reference.esp|00000001"},{"addon","reference.esp|00000002"},{"noHeel",1.0}};
        auto setReference=[&](const Json& r){
            std::filesystem::create_directories("Data/SKSE/Plugins");
            std::ofstream f("Data/SKSE/Plugins/VanityUBEHeelAdapter.json");
            f<<Json{{"surfaceCalibrationReference",r}}.dump();
            vanity_ube_heel_adapter::config_state::Set(Json{{"surfaceCalibrationReference",r},{"heelMax",2.0}});
        };
        setReference(ref);check(!hp::ReferenceStamp().empty());
        const auto bytes=tri();put("Data/Meshes/test/sock.tri",bytes);
        const auto fp=hp::Fingerprint(bytes);
        auto c=hp::Probe("test/sock.tri");check(c.status=="parsed");check(c.fingerprint==fp);
        check(c.shapes.size()==1&&c.shapes[0].name=="Sock");check(c.shapes[0].heel&&c.shapes[0].noHeel);
        check(hp::Probe("../sock.tri").status=="resource-unreadable");
        check(hp::Probe("test/missing.tri").status=="resource-unreadable");
        put("Data/Meshes/test/bad.tri",{'P','I','R','T'});check(hp::Probe("test/bad.tri").status=="malformed");
        Json identity={{"armor","sock.esp|00000001"},{"addon","sock.esp|00000002"},{"race","race.esp|00000001"},{"sexIndex",1},{"actorWeight",0.0}};
        Json shoe=identity;shoe["armor"]="shoe.esp|00000001";shoe["addon"]="shoe.esp|00000002";
        const auto context=Json::array({identity["race"],identity["sexIndex"],identity["actorWeight"],Json::array()}).dump();
        Json p={{"heightSession",hp::Session()},{"algorithm","bounded-native-branches-v2"},{"stocking",identity},{"footwear",shoe},
            {"context",context},{"NoHeel",0.0},{"Heel",0.5},{"normalizedResidual",0.01},{"saturated",false},
            {"targetPositionFingerprint","1111111111111111"},{"donorSourceFingerprint","2222222222222222"},
            {"bodyTriFingerprint",fp},{"inputs",Json::array({{{"resource","test/sock.tri"},{"fingerprint",fp}}})}};
        check(hp::FormatValid(p));check(hp::InputsValid(p));
        for(const auto*key:{"bodyTriFingerprint","context","inputs","saturated","footwear","NoHeel"}){auto bad=p;bad.erase(key);check(!hp::FormatValid(bad));}
        auto bad=p;bad["NoHeel"]=0.5;check(!hp::FormatValid(bad));bad=p;bad["Heel"]=10.1;check(!hp::FormatValid(bad));bad=p;bad["NoHeel"]=1.01;check(!hp::FormatValid(bad));bad=p;bad["Heel"]=1.107;check(hp::FormatValid(bad));
        bad=p;bad["inputs"][0]["fingerprint"]="3333333333333333";check(!hp::InputsValid(bad));
        bad=p;bad["inputs"][0]["resource"]="../sock.tri";check(!hp::InputsValid(bad));
        hp::Publish(p);check(std::filesystem::exists(hp::kPath));
        auto lookup=[&]{return hp::Lookup(shoe["armor"],shoe["addon"],identity["armor"],identity["addon"],context);};
        check(!lookup());
        Json d={{"heightSession",hp::Session()},{"geometryRole","foot"},{"identity",shoe},{"actorMorphValues",Json::array()},
            {"positionFingerprint","1111111111111111"},{"sourceModelEvidence",{{"measurementEligible",true},{"asset",{{"sourceFingerprint","2222222222222222"}}}}},
            {"captureSelection",{{"currentGraphConfirmed",true},{"identityIsCorrelated",false}}}};
        hp::ObserveFoot(d);check(!lookup());d["identity"]=identity;d["geometryRole"]="stocking";hp::ObserveFoot(d);check(lookup().has_value());
        check(!hp::Lookup(shoe["armor"],shoe["addon"],identity["armor"],identity["addon"],"other-context"));
        {std::scoped_lock lock(hp::mutex);hp::profiles.clear();}check(!lookup());hp::Load(hp::Session());check(lookup().has_value());
        auto changedReference=ref;changedReference["noHeel"]=0.5;setReference(changedReference);
        {std::scoped_lock lock(hp::mutex);hp::profiles.clear();}hp::Load(hp::Session());check(!lookup());
        setReference(Json::object());check(hp::ReferenceStamp().empty());hp::Load(hp::Session());check(!lookup());
        setReference(ref);hp::Load(hp::Session());check(lookup().has_value());
        put("Data/Meshes/test/sock.tri",{'c','h','a','n','g','e','d'});check(!hp::InputsValid(p));
        {std::scoped_lock lock(hp::mutex);hp::profiles.clear();}hp::Load(hp::Session());check(!lookup());
        put("Data/Meshes/test/sock.tri",bytes);hp::Load(hp::Session());check(lookup().has_value());
        // Simulate a new generation without starting the timer/worker. Public
        // publish/observation entry points must reject every old-generation job.
        hp::session.fetch_add(1);{std::scoped_lock lock(hp::mutex);hp::profiles.clear();hp::observed.clear();hp::observedDonors.clear();}
        hp::Publish(p);hp::ObserveFoot(d);check(!lookup());check(hp::profiles.empty());check(hp::observedDonors.empty());
        p["heightSession"]=hp::Session();hp::Publish(p);check(hp::profiles.size()==1);
        d["heightSession"]=hp::Session();d["captureSelection"]["identityIsCorrelated"]=true;hp::ObserveFoot(d);check(hp::observedDonors.empty());
        std::ifstream f(hp::kPath);auto saved=Json::parse(f);check(saved["entries"].size()==1);check(saved["entries"][0]["Heel"]==0.5);
        check(!std::filesystem::exists(std::string(hp::kPath)+".tmp"));
        std::cout<<checks<<" real cache/capability checks passed\n";
    }catch(const std::exception&e){std::cerr<<e.what()<<'\n';std::filesystem::current_path(old);return 1;}
    std::filesystem::current_path(old);std::filesystem::remove_all(dir);return 0;
}
