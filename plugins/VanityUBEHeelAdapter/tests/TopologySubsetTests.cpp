#include "../src/TopologySubsetCore.h"
#include <iostream>
using namespace vanity_ube_heel_adapter;using P=surface_posture_core::Point;using T=surface_posture_core::Triangle;
int checks=0;void Check(bool b){++checks;if(!b)throw std::runtime_error("subset check "+std::to_string(checks));}
int main(){std::vector<P>a;std::vector<T>at;
 // 60 connected fan vertices plus a disconnected 3-vertex island. Island
 // indices are interleaved, so a simple index offset is NOT a valid mapping.
 for(unsigned i=0;i<63;++i)a.push_back({float(i),float(i%3),0});
 std::vector<unsigned> core;for(unsigned i=0;i<63;++i)if(i!=1&&i!=5&&i!=62)core.push_back(i);
 for(unsigned j=1;j+1<core.size();++j)at.push_back({core[0],core[j],core[j+1]});at.push_back({1,5,62});
 std::vector<P>b;for(auto i:core)b.push_back({a[i][0],a[i][1],2});std::vector<T>bt;for(unsigned j=1;j+1<b.size();++j)bt.push_back({0,j,j+1});
 auto r=topology_subset_core::Align(a,at,b,bt);Check(r.Complete());Check(r.removedVertices==3);Check(r.removedComponents==1);Check(r.commonTriangles.size()==bt.size());
 for(unsigned i=0;i<b.size();++i){Check(r.anchorToTarget[core[i]]==i);Check(r.alignedTarget[core[i]]==b[i]);}
 Check(r.anchorToTarget[5]==UINT32_MAX);
 auto changed=bt;std::swap(changed[0][1],changed[0][2]);Check(!topology_subset_core::Align(a,at,b,changed).Complete());
 changed=bt;changed[0][1]=999;Check(!topology_subset_core::Align(a,at,b,changed).Complete());
 auto corrupt=b;corrupt[0][0]=NAN;Check(!topology_subset_core::Align(a,at,corrupt,bt).Complete());
 Check(topology_subset_core::Align(a,at,a,at).Complete());Check(!topology_subset_core::Align(b,bt,a,at).Complete());
 Check(!topology_subset_core::Align(a,at,b,std::span<const T>{}).Complete());
 std::cout<<checks<<" component subset checks passed\n";
}
