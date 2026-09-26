// Compile this test without any Skyrim or nlohmann dependency. Python supplies
// fixtures encoded by the shipped writer, then invokes this exact disk worker.
#include "../src/BulkHeightLibrary.cpp"
#include <chrono>
#include <iostream>
using namespace vanity_ube_heel_adapter;
namespace bh=bulk_height;
int main(int argc,char**argv){
    try{
        if(argc>=3&&std::string(argv[1])=="inspect"){
            auto index=bh::DecodeIndex(bh::Read(argv[2],8*1024*1024));std::size_t members=0;
            for(const auto&e:index.entries)members+=e.count;
            std::cout<<index.entries.size()<<' '<<members<<' '<<index.stockings.size()<<'\n';return 0;
        }
        if(argc>=3&&std::string(argv[1])=="shard"){
            auto shard=bh::DecodeShard(bh::Read(argv[2],16*1024*1024));
            std::cout<<shard.groups.size()<<' '<<shard.members.size()<<'\n';return 0;
        }
        if(argc!=12)throw std::runtime_error("usage: query|cache root meshes shoeID shoeAddon shoeModel sockID sockAddon sockModel weight allowResidual");
        bh::Request request{{argv[4],argv[5],argv[6]},{argv[7],argv[8],argv[9]},"context-one",std::stod(argv[10]),2.,std::string(argv[11])=="1"};
        std::atomic<unsigned> notifications=0;
        bh::Library library(argv[2],argv[3],[&]{++notifications;});library.Reset();
        auto wait=[&]{
            auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);bh::Reply r;
            do {r=library.Lookup(request);if(r.state!="pending")return r;std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(std::chrono::steady_clock::now()<deadline);
            throw std::runtime_error("worker timeout");
        };
        auto r=wait();auto before=library.Counters();
        for(unsigned i=0;i<500;++i){auto again=library.Lookup(request);if(again.state!=r.state)throw std::runtime_error("cache lost stable reply");}
        auto after=library.Counters();
        if(before.indexReads!=after.indexReads||before.shardReads!=after.shardReads||before.assetReads!=after.assetReads)throw std::runtime_error("repeated lookup caused IO");
        if(std::string(argv[1])=="cache"&&r.Ready()){
            // Changing body context causes a new decision, but source-verified
            // shards/assets are reused within the same library generation.
            request.context="context-two";auto c=wait();if(!c.Ready())throw std::runtime_error("context cache reuse");
            auto a=library.Counters();if(a.shardReads!=after.shardReads||a.assetReads!=after.assetReads)throw std::runtime_error("context change reread stable files");
            library.Reset();auto d=wait();if(!d.Ready())throw std::runtime_error("reload lookup failed");
            auto e=library.Counters();if(e.indexReads<=a.indexReads||e.shardReads<=a.shardReads||e.assetReads<=a.assetReads)throw std::runtime_error("reset did not invalidate IO caches");
            if(!library.IsStocking(request.stocking))throw std::runtime_error("index classification missing");
        }
        std::cout<<r.state<<' '<<r.values.noHeel<<' '<<r.values.heel<<' '<<r.flags<<' '<<r.originalResidual<<' '<<r.detail<<'\n';return 0;
    }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 2;}
}
