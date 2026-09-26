#include "BulkHeightLibrary.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

namespace vanity_ube_heel_adapter::bulk_height {
namespace {
constexpr std::size_t IndexLimit=8*1024*1024, ShardLimit=16*1024*1024, AssetLimit=64*1024*1024;
void Require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
bool Hex(const std::string& v){return std::all_of(v.begin(),v.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});}
bool Stable(const std::string& id){
    const auto p=id.rfind('|');if(p==std::string::npos||p<5||id.size()-p!=9||id.size()>300)return false;
    const auto f=Normalize(id.substr(0,p));
    return (f.ends_with(".esp")||f.ends_with(".esm")||f.ends_with(".esl"))&&
        f.find_first_of("/\\:")==std::string::npos&&Hex(Normalize(id.substr(p+1)));
}
bool Valid(Controls c,double cap=10.){return std::isfinite(cap)&&cap>=1&&cap<=10&&
    std::isfinite(c.noHeel)&&std::isfinite(c.heel)&&c.noHeel>=0&&c.noHeel<=1&&c.heel>=0&&c.heel<=cap&&!(c.noHeel>0&&c.heel>0);}
Identity Canon(Identity id){
    Require(Stable(id.armor)&&Stable(id.addon),"invalid-library-identity");
    id.armor=Normalize(id.armor);id.addon=Normalize(id.addon);id.model=Resource(id.model);
    Require(id.model.ends_with(".nif"),"identity-model-is-not-NIF");return id;
}
struct Reader {
    std::span<const std::uint8_t> bytes;std::size_t at{};
    std::span<const std::uint8_t> Take(std::size_t n){Require(n<=bytes.size()-at,"truncated-library");auto s=bytes.subspan(at,n);at+=n;return s;}
    std::uint64_t UInt(unsigned n){auto b=Take(n);std::uint64_t v=0;for(unsigned i=0;i<n;++i)v|=std::uint64_t(b[i])<<(8*i);return v;}
    std::uint32_t Count(std::uint32_t max=UINT32_MAX){auto n=UInt(4);Require(n<=max,"library-count-limit");return static_cast<std::uint32_t>(n);}
    double Double(){auto v=std::bit_cast<double>(UInt(8));Require(std::isfinite(v),"nonfinite-library-value");return v;}
    std::string String(){auto n=Count(2048);Require(n>0,"empty-library-string");auto b=Take(n);std::string s(b.begin(),b.end());Require(std::none_of(s.begin(),s.end(),[](unsigned char c){return c<32;}),"invalid-library-string");return s;}
    Identity ID(){return Canon({String(),String(),String()});}
    void Magic(const std::array<std::uint8_t,8>& magic){auto b=Take(8);Require(std::equal(b.begin(),b.end(),magic.begin()),"library-format-mismatch");}
    void End(){Require(at==bytes.size(),"trailing-library-data");}
};
std::vector<std::uint8_t> Read(const std::filesystem::path& path,std::size_t limit){
    // Open first: do not use path-based stat as a prerequisite under MO2.
    std::ifstream f(path,std::ios::binary|std::ios::ate);Require(bool(f),"library-file-unreadable");
    auto size=f.tellg();Require(size>=0&&static_cast<std::uint64_t>(size)<=limit,"library-file-size-limit");
    std::vector<std::uint8_t> b(static_cast<std::size_t>(size));f.seekg(0);
    if(!b.empty())Require(bool(f.read(reinterpret_cast<char*>(b.data()),static_cast<std::streamsize>(b.size()))),"library-file-short-read");
    Require(f.peek()==std::char_traits<char>::eof(),"library-file-changed");return b;
}
std::uint64_t HashFile(const std::filesystem::path& path){
    std::ifstream f(path,std::ios::binary);Require(bool(f),"library-source-unreadable");
    std::array<char,64*1024> buf{};std::uint64_t h=14695981039346656037ULL;std::size_t total=0;
    while(f){f.read(buf.data(),buf.size());auto n=f.gcount();total+=static_cast<std::size_t>(n);Require(total<=AssetLimit,"library-source-size-limit");for(std::streamsize i=0;i<n;++i){h^=static_cast<unsigned char>(buf[static_cast<std::size_t>(i)]);h*=1099511628211ULL;}}
    Require(f.eof(),"library-source-read-error");return h;
}
std::filesystem::path Path(const std::string& resource){auto v=Resource(resource);std::replace(v.begin(),v.end(),'\\','/');return std::filesystem::path(std::u8string(v.begin(),v.end()));}
}
std::string Normalize(std::string s){for(char& c:s)if(c>='A'&&c<='Z')c=static_cast<char>(c-'A'+'a');return s;}
std::string Resource(std::string s){
    s=Normalize(s);std::replace(s.begin(),s.end(),'/','\\');
    Require(!s.empty()&&s.size()<=2048&&s.front()!='\\'&&s.find(':')==std::string::npos,"unsafe-library-resource");
    if(s.starts_with("meshes\\"))s.erase(0,7);
    std::string result;std::size_t at=0;
    while(at<s.size()){const auto end=s.find('\\',at);auto p=s.substr(at,end==std::string::npos?s.size()-at:end-at);
        Require(p!="."&&p!="..","unsafe-library-resource");if(!p.empty()){if(!result.empty())result+='\\';result+=p;}
        if(end==std::string::npos)break;
        at=end+1;}
    Require(!result.empty(),"empty-library-resource");return result;
}
std::string Key(const Identity& id){auto c=Canon(id);return c.armor+'\n'+c.addon+'\n'+c.model;}
std::uint64_t Fingerprint(std::span<const std::uint8_t> b){std::uint64_t h=14695981039346656037ULL;for(auto v:b){h^=v;h*=1099511628211ULL;}return h;}
Index DecodeIndex(std::span<const std::uint8_t> b){
    Require(b.size()<=IndexLimit,"library-index-size-limit");Reader r{b};r.Magic({'V','H','A','I','D','X','1',0});Index x;x.generation=r.String();
    auto ns=r.Count(20000);std::set<std::string> stockKeys;
    for(std::uint32_t i=0;i<ns;++i){auto id=r.ID();Require(stockKeys.insert(Key(id)).second,"duplicate-library-stocking");x.stockings.push_back(std::move(id));}
    auto n=r.Count(4096);std::set<std::string> seen;std::uint64_t total=0;
    for(std::uint32_t i=0;i<n;++i){IndexEntry e;e.shoe=r.ID();e.file=r.String();e.fingerprint=r.UInt(8);e.count=r.Count(20000);
        Require(e.file.size()==68&&e.file.ends_with(".vhs")&&Hex(e.file.substr(0,64)),"unsafe-library-shard-name");
        Require(seen.insert(Key(e.shoe)).second,"duplicate-library-shoe");total+=e.count;Require(total<=200000,"library-member-capacity");x.entries.push_back(std::move(e));}
    r.End();return x;
}
Shard DecodeShard(std::span<const std::uint8_t> b){
    Require(b.size()<=ShardLimit,"library-shard-size-limit");Reader r{b};r.Magic({'V','H','A','S','H','D','1',0});Shard x;x.shoe=r.ID();x.weight=r.Double();Require(x.weight>=0&&x.weight<=100,"library-weight-out-of-range");
    auto n=r.Count(65535);for(std::uint32_t i=0;i<n;++i){Asset a{Resource(r.String()),r.UInt(8)};Require(a.resource.ends_with(".nif")||a.resource.ends_with(".tri"),"invalid-library-source");x.assets.push_back(std::move(a));}
    n=r.Count(20000);for(std::uint32_t i=0;i<n;++i){Controls c{r.Double(),r.Double()};Require(Valid(c),"invalid-library-controls");x.groups.push_back(c);}
    std::set<std::string> seen;n=r.Count(20000);
    for(std::uint32_t i=0;i<n;++i){Member m;m.stocking=r.ID();m.group=r.Count();m.flags=r.Count(7);m.originalNoHeel=r.Double();m.originalHeel=r.Double();m.residual=r.Double();
        Require(m.group<x.groups.size()&&Valid({m.originalNoHeel,m.originalHeel})&&m.residual>=0,"invalid-library-member");Require(seen.insert(Key(m.stocking)).second,"duplicate-library-member");
        std::set<std::uint32_t> sources;auto count=r.Count(64);Require(count>0,"library-member-missing-sources");
        for(std::uint32_t j=0;j<count;++j){auto src=r.Count();Require(src<x.assets.size()&&sources.insert(src).second,"invalid-library-source-index");m.sources.push_back(src);}
        // Every candidate must bind both actual NIFs, not merely an unrelated TRI.
        bool shoe=false,stock=false;for(auto src:m.sources){shoe|=x.assets[src].resource==x.shoe.model;stock|=x.assets[src].resource==m.stocking.model;}
        Require(shoe&&stock,"library-missing-model-binding");x.members.push_back(std::move(m));}
    r.End();return x;
}
struct Library::Impl {
    std::filesystem::path root,meshes;std::function<void()> callback;
    std::mutex mutex;std::condition_variable cv;std::uint64_t epoch=1;std::deque<std::pair<std::uint64_t,Request>> jobs;
    std::set<std::string> knownStockings;std::set<std::string> pending;std::map<std::string,Reply> replies;
    std::atomic<std::uint64_t> indexReads{0},shardReads{0},assetReads{0};
    std::jthread worker;
    // The following are accessed ONLY by the worker.
    std::uint64_t diskEpoch{};Index index;std::string indexError;std::map<std::string,IndexEntry> entries;
    struct Cached {Shard shard;std::uint64_t used{};};std::map<std::string,Cached> shards;std::uint64_t clock{};
    std::map<std::string,bool> sources;
    Impl(std::filesystem::path r,std::filesystem::path m,std::function<void()> cb):root(std::move(r)),meshes(std::move(m)),callback(std::move(cb)),worker([this](std::stop_token stop){Run(stop);}){}
    ~Impl(){worker.request_stop();cv.notify_all();worker.join();}
    std::string Token(const Request&q){return Key(q.shoe)+'\n'+Key(q.stocking)+'\n'+q.context+'\n'+std::to_string(std::bit_cast<std::uint64_t>(q.weight))+'\n'+std::to_string(std::bit_cast<std::uint64_t>(q.heelMax))+(q.allowResidualWarnings?"/warn":"/strict");}
    void Load(std::uint64_t current){
        if(diskEpoch==current)return;
        diskEpoch=current;index={};entries.clear();shards.clear();sources.clear();indexError.clear();
        try{++indexReads;index=DecodeIndex(Read(root/"index.vhi",IndexLimit));for(const auto&e:index.entries)entries.emplace(Key(e.shoe),e);}catch(const std::exception&e){indexError=e.what();}
    }
    Reply Process(const Request&q,std::uint64_t current){
        Load(current);Reply out;out.generation=index.generation;
        if(!indexError.empty()){out.state=indexError=="library-file-unreadable"?"unavailable":"invalid-library";out.detail=indexError;return out;}
        const auto key=Key(q.shoe);auto ie=entries.find(key);if(ie==entries.end()){out.state="not-found";return out;}
        auto it=shards.find(key);
        if(it==shards.end()){
            ++shardReads;auto b=Read(root/ie->second.file,ShardLimit);Require(Fingerprint(b)==ie->second.fingerprint,"library-shard-fingerprint-mismatch");auto shard=DecodeShard(b);
            Require(Key(shard.shoe)==key&&shard.members.size()==ie->second.count,"library-shard-index-mismatch");
            if(shards.size()>=8){auto oldest=std::min_element(shards.begin(),shards.end(),[](const auto&a,const auto&b){return a.second.used<b.second.used;});shards.erase(oldest);}
            it=shards.emplace(key,Cached{std::move(shard),++clock}).first;
        }
        it->second.used=++clock;const auto& shard=it->second.shard;
        auto wanted=Canon(q.stocking);auto member=std::find_if(shard.members.begin(),shard.members.end(),[&](const Member&m){return m.stocking==wanted;});
        if(member==shard.members.end()){out.state="not-found";return out;}
        out.flags=member->flags;out.originalResidual=member->residual;
        if(std::abs(q.weight-shard.weight)>.001){out.state="source-weight-mismatch";return out;}
        if((member->flags&1)&&!q.allowResidualWarnings){out.state="residual-warning-disabled";return out;}
        const auto values=shard.groups[member->group];Require(Valid(values,q.heelMax),"library-controls-exceed-current-limits");
        for(auto i:member->sources){const auto&a=shard.assets[i];const auto token=a.resource+'\n'+std::to_string(a.fingerprint);auto found=sources.find(token);bool valid=false;
            if(found!=sources.end())valid=found->second;
            else{try{++assetReads;valid=HashFile(meshes/Path(a.resource))==a.fingerprint;}catch(...){valid=false;}if(sources.size()>=4096)sources.clear();sources[token]=valid;}
            if(!valid){out.state="stale-or-unreadable-source";out.detail=a.resource;return out;}
        }
        out.values=values;out.state="ready";return out;
    }
    void Run(std::stop_token stop){
        while(true){std::pair<std::uint64_t,Request> job;
            {std::unique_lock lock(mutex);cv.wait(lock,[&]{return stop.stop_requested()||!jobs.empty();});if(stop.stop_requested())return;job=std::move(jobs.front());jobs.pop_front();}
            Reply reply;
            try{if(job.second.shoe.armor.empty()){Load(job.first);reply.state="index-ready";}else reply=Process(job.second,job.first);}catch(const std::exception&e){reply.state="invalid-library";reply.detail=e.what();}
            bool signal=false;{std::scoped_lock lock(mutex);if(epoch==job.first){knownStockings.clear();for(const auto& id:index.stockings)knownStockings.insert(Key(id));
                auto key=job.second.shoe.armor.empty()?std::string("__index__"):Token(job.second);pending.erase(key);if(replies.size()>=256)replies.clear();replies[key]=std::move(reply);signal=true;}}
            if(signal&&callback)try{callback();}catch(...){/* No game operation is performed by this worker. */}
        }
    }
};
Library::Library(std::filesystem::path r,std::filesystem::path m,std::function<void()> cb):impl(std::make_unique<Impl>(std::move(r),std::move(m),std::move(cb))){}
Library::~Library()=default;
Reply Library::Lookup(Request q){
    try{
        q.shoe=Canon(std::move(q.shoe));q.stocking=Canon(std::move(q.stocking));Require(std::isfinite(q.weight)&&q.weight>=0&&q.weight<=100&&std::isfinite(q.heelMax)&&q.heelMax>=1&&q.heelMax<=10&&q.context.size()<=65536,"invalid-library-request");
        std::scoped_lock lock(impl->mutex);auto key=impl->Token(q);auto it=impl->replies.find(key);if(it!=impl->replies.end())return it->second;
        if(impl->jobs.size()<32&&impl->pending.insert(key).second){impl->jobs.emplace_back(impl->epoch,std::move(q));impl->cv.notify_one();}return {};
    }catch(const std::exception&e){Reply r;r.state="invalid-request";r.detail=e.what();return r;}
}
void Library::Reset(){std::scoped_lock lock(impl->mutex);++impl->epoch;impl->jobs.clear();impl->pending.clear();impl->replies.clear();impl->knownStockings.clear();impl->jobs.emplace_back(impl->epoch,Request{});impl->cv.notify_one();}
Stats Library::Counters()const{return {impl->indexReads.load(),impl->shardReads.load(),impl->assetReads.load()};}
bool Library::IsStocking(Identity id){try{auto key=Key(id);std::scoped_lock lock(impl->mutex);return impl->knownStockings.contains(key);}catch(...){return false;}}
namespace {std::atomic<void(*)()> notify{nullptr};Library& Instance(){static Library library("Data/SKSE/Plugins/VanityUBEHeelAdapter/height-library","Data/Meshes",[]{if(auto cb=notify.load())cb();});return library;}}
void Initialize(void(*cb)()){notify.store(cb);Instance().Reset();}
void Reset(){Instance().Reset();}
bool IsStocking(Identity id){return Instance().IsStocking(std::move(id));}
Stats Counters(){return Instance().Counters();}
Reply Lookup(Request q){return Instance().Lookup(std::move(q));}
}
