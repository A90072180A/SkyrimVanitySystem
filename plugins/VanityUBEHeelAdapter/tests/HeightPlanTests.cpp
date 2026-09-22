#include "../src/HeightPlanCore.h"
#include "../src/ScopedMorphTransaction.h"
#include <iostream>
#include <map>
#include <stdexcept>
using namespace vanity_ube_heel_adapter;
namespace hp=height_plan_core;
namespace tx=scoped_morph_transaction;
int checks=0;
void Check(bool v){++checks;if(!v)throw std::runtime_error("height check "+std::to_string(checks));}
struct Fake {
    std::map<std::string,float> own,other;
    std::vector<std::array<float,2>> applied;
    bool throwApply=false,maximum=false;
    bool Has(const std::string&n){return own.contains(n);}
    float Get(const std::string&n){auto it=own.find(n);return it==own.end()?0:it->second;}
    float Effective(const std::string&n){return maximum?(std::max)(Get(n),other[n]):Get(n)+other[n];}
    void Set(const std::string&n,float v){own[n]=v;}
    void Clear(const std::string&n){own.erase(n);}
    void Apply(){applied.push_back({Effective("NoHeel"),Effective("Heel")});if(throwApply)throw std::runtime_error("mock renderer failure");}
};
int main(){
    using Vec=surface_posture_core::Vec;
    for(double bad:{-2.,1.0001,double(INFINITY),double(-INFINITY),double(NAN)})Check(!hp::FromSigned(bad));
    for(double v:{-1.,-.7,-.01,0.,.01,.7,1.}){auto c=hp::FromSigned(v);Check(c.has_value());Check(hp::Valid(*c));Check(c->noHeel==0||c->heel==0);}
    Check(!hp::Valid({1,1}));Check(!hp::Valid({NAN,0}));
    std::vector<Vec> n(300,Vec{0,0,3}),h(300,Vec{0,0,-2}),desired(300);
    for(int j=-20;j<=20;++j){double v=j/20.;auto c=*hp::FromSigned(v);
        for(std::size_t i=0;i<desired.size();++i)desired[i]={0,0,3*c.noHeel-2*c.heel};
        auto f=hp::Solve(desired,n,h);Check(f.status=="bounded-height-fit");Check(hp::Valid(f.controls));
        Check(std::abs(f.controls.noHeel-c.noHeel)<1e-10);Check(std::abs(f.controls.heel-c.heel)<1e-10);
        Check(f.rms<1e-10);Check(hp::Decision(f)=="accepted-measured-height");
    }
    for(auto&d:desired)d={0,0,-2.1};auto limited=hp::Solve(desired,n,h);
    Check(limited.controls.heel==1);Check(limited.saturated);Check(limited.heel.raw>1);Check(hp::Decision(limited)=="height-range-exceeded");
    Check(hp::Decision(limited,.15,true)=="accepted-endpoint-approximation");
    for(auto&d:desired)d={2,0,-2.1};limited=hp::Solve(desired,n,h);Check(hp::Decision(limited,.15,true)=="height-residual-too-large");
    std::vector<Vec> zero(300);Check(hp::Solve(desired,zero,zero).status=="no-usable-height-morph");
    Check(hp::Solve(desired,n,std::span<const Vec>{}).status=="invalid-height-input");
    desired[0][0]=NAN;Check(hp::Solve(desired,n,h).status=="nonfinite-height-input");
    Fake backend;backend.own={{"NoHeel",.125f},{"untouched",.7f}};backend.other={{"NoHeel",.25f},{"Heel",.5f}};
    const auto saved=backend.own, others=backend.other;
    // Both requested values must coexist during ONE renderer call. Alternating
    // branches never leaves a stale opposing slider and never erases OBody keys.
    for(int j=0;j<60;++j){auto c=*hp::FromSigned(j%2?.8:-.6);std::array<tx::Target,2> goals{{{"NoHeel",float(c.noHeel)},{"Heel",float(c.heel)}}};
        Check(tx::Run(backend,goals));Check(backend.own==saved);Check(backend.applied.size()==std::size_t(j+1));
        Check(std::abs(backend.applied.back()[0]-c.noHeel)<1e-6);Check(std::abs(backend.applied.back()[1]-c.heel)<1e-6);
    }
    backend.throwApply=true;bool threw=false;
    try{std::array<tx::Target,2> t{{{"NoHeel",1},{"Heel",0}}};tx::Run(backend,t);}catch(...){threw=true;}
    Check(threw);Check(backend.own==saved);Check(backend.other.at("NoHeel")==others.at("NoHeel"));
    backend.throwApply=false;backend.maximum=true;const auto count=backend.applied.size();
    std::array<tx::Target,2> t{{{"NoHeel",0},{"Heel",0}}};Check(!tx::Run(backend,t));Check(backend.applied.size()==count);Check(backend.own==saved);
    t={{{"NoHeel",NAN},{"Heel",1}}};Check(!tx::Run(backend,t));Check(backend.own==saved);
    t={{{"NoHeel",0},{"NoHeel",1}}};Check(!tx::Run(backend,t));Check(backend.own==saved);
    std::cout<<checks<<" height/transaction checks passed\n";
}
