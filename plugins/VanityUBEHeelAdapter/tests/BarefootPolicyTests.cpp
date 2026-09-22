#include "../src/BarefootPolicy.h"
#include "../src/ScopedMorphTransaction.h"
#include <array>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>
namespace bf=vanity_ube_heel_adapter::barefoot_policy;
namespace tx=vanity_ube_heel_adapter::scoped_morph_transaction;
unsigned checks=0;
void Check(bool ok){++checks;if(!ok)throw std::runtime_error("barefoot check "+std::to_string(checks));}
struct Backend {
    std::map<std::string,float> own, other;
    std::vector<std::array<float,2>> applied;
    bool Has(const std::string& n){return own.contains(n);}
    float Get(const std::string& n){return Has(n)?own.at(n):0;}
    float Effective(const std::string& n){return Get(n)+other[n];}
    void Set(const std::string& n,float x){own[n]=x;}
    void Clear(const std::string& n){own.erase(n);}
    void Apply(){applied.push_back({Effective("NoHeel"),Effective("Heel")});}
};
int main(){
    // Exhaustive identity and validity conditions. Missing foot BODYTRI is not
    // an input: it is deliberately NOT needed to classify a known skin model.
    for(unsigned bits=0;bits<128;++bits)for(unsigned advertised:{0u,1u,3u})for(unsigned live:{0u,1u,2u}){
        bf::Evidence e;
        e.enabled=bits&1;e.snapshotValid=bits&2;e.realFootwearWorn=bits&4;
        e.activeSkinPart=bits&8;e.skinAddonConfirmed=bits&16;
        e.expectedBareFeetModel=bits&32;e.currentSkinnedGeometry=bits&64;
        e.declaredFootwear=advertised;e.liveFootwear=live;
        const bool expected=e.enabled&&e.snapshotValid&&!e.realFootwearWorn&&e.activeSkinPart&&
            e.skinAddonConfirmed&&e.expectedBareFeetModel&&e.currentSkinnedGeometry&&!advertised&&!live;
        Check((bf::Decision(e)=="confirmed-barefoot")==expected);
    }
    bf::Evidence bare{true,true,0,0,false,true,true,true,true};
    Check(bf::Decision(bare)=="confirmed-barefoot");
    auto shoe=bare;shoe.liveFootwear=1;Check(bf::Decision(shoe)=="visible-footwear");
    shoe.liveFootwear=2;Check(bf::Decision(shoe)=="ambiguous-visible-footwear");
    shoe=bare;shoe.declaredFootwear=1;Check(bf::Decision(shoe)=="unresolved-visible-footwear");
    shoe=bare;shoe.realFootwearWorn=true;Check(bf::Decision(shoe)=="real-footwear-still-worn");
    bf::Settler gate;
    Check(!gate.Ready("confirmed-barefoot","skin-A",0));
    Check(!gate.Ready("confirmed-barefoot","skin-A",249));
    Check(gate.Ready("confirmed-barefoot","skin-A",250));
    Check(gate.Ready("confirmed-barefoot","skin-A",300));
    Check(!gate.Ready("visible-footwear","skin-A",310));
    Check(!gate.Ready("confirmed-barefoot","skin-A",320));
    Check(gate.Ready("confirmed-barefoot","skin-A",570));
    Check(!gate.Ready("confirmed-barefoot","rebuilt-skin-B",580));
    Check(gate.Ready("confirmed-barefoot","rebuilt-skin-B",830));
    Check(!gate.Ready("confirmed-barefoot","rebuilt-skin-B",20)); // clock restarted
    gate.Reset();Check(!gate.Ready("confirmed-barefoot","rebuilt-skin-B",1000));
    Check(!gate.Ready("snapshot-unverified","rebuilt-skin-B",1400));
    Check(!gate.Ready("confirmed-barefoot","",1500));
    // Real transaction kernel: a previous Heel pose is replaced, not accumulated.
    // Renderer/engine identity gathering itself still requires in-game acceptance.
    Backend b;b.own["NoHeel"]=.125f;b.other["NoHeel"]=.25f;b.other["Heel"]=.1f;
    const auto saved=b.own;const auto other=b.other;
    std::array<tx::Target,2> high{{{"NoHeel",0},{"Heel",1}}},flat{{{"NoHeel",1},{"Heel",0}}};
    for(unsigned i=0;i<50;++i){
        Check(tx::Run(b,high));Check(b.own==saved);Check(b.other==other);
        gate.Reset();Check(!gate.Ready("confirmed-barefoot","skin",2000));
        Check(gate.Ready("confirmed-barefoot","skin",2250));
        Check(tx::Run(b,flat));Check(b.own==saved);Check(b.other==other);
        Check(b.applied.back()[0]==1&&b.applied.back()[1]==0);
        const auto n=b.applied.size();
        Check(!gate.Ready("unresolved-visible-footwear","skin",2300));
        Check(b.applied.size()==n); // unknown is never flattened
    }
    std::cout<<checks<<" barefoot policy/transaction checks passed\n";
}
