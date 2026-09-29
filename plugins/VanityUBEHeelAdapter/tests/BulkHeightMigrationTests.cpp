#include "../src/BulkHeightLibrary.cpp"
#include <chrono>
#include <iostream>
namespace b=vanity_ube_heel_adapter::bulk_height;
struct Wire {
    std::vector<std::uint8_t> data;
    void u(std::uint64_t n,unsigned size=4){for(unsigned i=0;i<size;++i)data.push_back(static_cast<std::uint8_t>(n>>(8*i)));}
    void s(const std::string& n){u(n.size());data.insert(data.end(),n.begin(),n.end());}
    void d(double n){u(std::bit_cast<std::uint64_t>(n),8);}
    void id(const b::Identity& n){s(n.armor);s(n.addon);s(n.model);}
    void magic(const char* n){data.insert(data.end(),n,n+8);}
};
void check(bool value){if(!value)throw std::runtime_error("migration check failed");}
void put(const std::filesystem::path& path,const std::vector<std::uint8_t>& data){std::filesystem::create_directories(path.parent_path());std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(data.data()),static_cast<std::streamsize>(data.size()));check(bool(f));}
int main(){
    const auto root=std::filesystem::temp_directory_path()/("vha-migration-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup{std::filesystem::path p;~Cleanup(){std::error_code ec;std::filesystem::remove_all(p,ec);}} cleanup{root};
    const std::vector<std::uint8_t> oldStock{1,2,3},newStock{1,2,3,4},shoe{4,5,6};
    const b::Identity shoeID{"shoe.esp|00000001","shoe.esp|00000002","shoe.nif"},sockID{"sock.esp|00000001","sock.esp|00000002","sock.nif"};
    Wire shard;shard.magic("VHASHD1");shard.id(shoeID);shard.d(0);shard.u(2);
    shard.s("shoe.nif");shard.u(b::Fingerprint(shoe),8);shard.s("sock.nif");shard.u(b::Fingerprint(oldStock),8);
    shard.u(1);shard.d(0);shard.d(1.1);shard.u(1);shard.id(sockID);shard.u(0);shard.u(1);shard.d(0);shard.d(1.1);shard.d(.2);shard.u(2);shard.u(0);shard.u(1);
    const auto file=std::string(64,'a')+".vhs";Wire index;index.magic("VHAIDX1");index.s("generation-one");index.u(1);index.id(sockID);index.u(1);index.id(shoeID);index.s(file);index.u(b::Fingerprint(shard.data),8);index.u(1);
    put(root/"library"/file,shard.data);put(root/"library/index.vhi",index.data);put(root/"meshes/shoe.nif",shoe);put(root/"meshes/sock.nif",newStock);
    b::Library library(root/"library",root/"meshes",[]{});library.Reset();
    b::Request q{shoeID,sockID,"context",0,2,true,{}};
    auto wait=[&]{for(int i=0;i<10000;++i){auto r=library.Lookup(q);if(r.state!="pending")return r;std::this_thread::sleep_for(std::chrono::milliseconds(1));}throw std::runtime_error("worker timed out");};
    check(wait().state=="stale-or-unreadable-source");
    q.sourceAliases={{{"sock.nif",b::Fingerprint(oldStock)},{"sock.nif",b::Fingerprint(newStock)}}};
    auto r=wait();check(r.Ready()&&r.values.heel==1.1&&r.migratedSources==1&&(r.flags&1));
    auto before=library.Counters();for(int i=0;i<100;++i)check(wait().Ready());check(library.Counters().assetReads==before.assetReads);
    q.sourceAliases[0].before.fingerprint++;check(wait().state=="stale-or-unreadable-source");
    q.sourceAliases[0].before.fingerprint--;q.sourceAliases[0].after.fingerprint++;check(wait().state=="stale-or-unreadable-source");
    q.sourceAliases[0].after.fingerprint--;check(wait().Ready());
    q.sourceAliases.push_back(q.sourceAliases[0]);check(wait().state=="invalid-request");q.sourceAliases.pop_back();
    q.sourceAliases[0].after.resource="../outside.nif";check(wait().state=="invalid-request");q.sourceAliases[0].after.resource="sock.nif";
    q.allowResidualWarnings=false;check(wait().state=="residual-warning-disabled");q.allowResidualWarnings=true;
    q.weight=20;check(wait().state=="source-weight-mismatch");q.weight=0;
    q.sourceAliases.clear();check(wait().state=="stale-or-unreadable-source");
    put(root/"meshes/sock.nif",oldStock);library.Reset();check(wait().Ready());
    check(b::Read(root/"library/index.vhi",10000)==index.data);check(b::Read(root/"library"/file,10000)==shard.data);
    std::cout<<"Exact source migration, cache isolation, stale guards and rollback passed\n";
}
