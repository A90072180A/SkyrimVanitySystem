#include "../src/HeightPlanCore.h"
#include "../src/ScopedMorphTransaction.h"
#include <iostream>
#include <map>
using namespace vanity_ube_heel_adapter;
int checks=0;void Check(bool v){++checks;if(!v)throw std::runtime_error("extended check "+std::to_string(checks));}
struct Backend {
 std::map<std::string,float> keys{{"NoHeel",.2f},{"Heel",.3f}};std::array<float,2> last{};
 bool Has(const std::string& n){return keys.contains(n);}float Get(const std::string& n){return keys.contains(n)?keys[n]:0;}
 float Effective(const std::string& n){return Get(n)+.1f;}void Set(const std::string& n,float v){keys[n]=v;}void Clear(const std::string& n){keys.erase(n);}
 void Apply(){last={Effective("NoHeel"),Effective("Heel")};}
};
int main(){namespace hp=height_plan_core;using V=surface_posture_core::Vec;
 std::vector<V> n(300,V{0,0,3}),h(300,V{0,0,-2}),target(300);Backend back;auto saved=back.keys;
 for(double cap:{1.,1.5,2.,5.,10.})for(int i=0;i<=100;++i){double q=cap*i/100.;for(auto& t:target)t={0,0,-2*q};
  auto r=hp::Solve(target,n,h,cap);Check(r.status=="bounded-height-fit");Check(std::abs(r.controls.heel-q)<1e-9);Check(r.controls.noHeel==0);Check(!r.saturated);Check(hp::Valid(r.controls,cap));
  std::array<scoped_morph_transaction::Target,2> targets{{{"NoHeel",0},{"Heel",float(q)}}};Check(scoped_morph_transaction::Run(back,targets));Check(back.keys==saved);Check(std::abs(back.last[1]-q)<2e-6);
 }
 for(double cap:{1.,2.,10.})for(double q:{1.001,1.5,2.}){for(auto&t:target)t={0,0,3*q};auto r=hp::Solve(target,n,h,cap);Check(r.controls.noHeel==1);Check(r.saturated);Check(!hp::Valid({q,0},cap));}
 for(double cap:{.99,11.,double(INFINITY),double(NAN)})Check(hp::Solve(target,n,h,cap).status=="invalid-height-input");
 Check(hp::FromSigned(-1.107,2.).has_value());Check(!hp::FromSigned(1.107,2.));Check(!hp::FromSigned(-2.01,2.));
 std::cout<<checks<<" asymmetric-height and transaction checks passed\n";
}
