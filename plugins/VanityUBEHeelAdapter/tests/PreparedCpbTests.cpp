#include "../src/BulkHeightLibrary.cpp"
#include "../src/PreparedCpb.cpp"
#include <chrono>
#include <iostream>
namespace p=vanity_ube_heel_adapter::prepared_cpb;
int main(int argc,char** argv){
    if(argc==4&&std::string(argv[1])=="probe"){
        const auto s=p::Probe(argv[2],argv[3]);
        std::cout<<nlohmann::json{{"status",s.status},{"error",s.error},{"ready",s.ready},{"agataReady",s.agataReady},{"glassReady",s.glassReady},{"generation",s.generation},{"triFingerprint",s.triFingerprint},{"aliases",s.aliases.size()}}.dump()<<'\n';return s.ready?0:1;
    }
    const auto root=std::filesystem::temp_directory_path()/("vha-prepared-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);auto check=[](bool v){if(!v)throw std::runtime_error("prepared IO guard failed");};
    check(!p::Probe(root/"missing.json",root).ready);
    {std::ofstream file(root/"profile.json");file<<"{\"schema\":1,\"algorithm\":\"cpb-surface-v1-full-domain\",\"aliases\":[]}";}
    const auto bad=p::Probe(root/"profile.json",root);check(!bad.ready&&bad.error=="unreviewed-prepared-profile"&&bad.aliases.empty());
    {std::ofstream file(root/"profile.json");file<<std::string(65537,'x');}
    check(p::Probe(root/"profile.json",root).error=="file-size-limit");
    p::Initialize(nullptr);for(unsigned i=0;i<1000&&p::Current()->status=="pending";++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    check(!p::Current()->ready);p::Reset();check(p::Current()->status=="pending"||!p::Current()->ready);
    std::filesystem::remove_all(root);std::cout<<"Prepared profile guards and worker lifecycle passed\n";
}
