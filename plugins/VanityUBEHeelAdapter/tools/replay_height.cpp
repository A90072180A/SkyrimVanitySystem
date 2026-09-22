// Offline regression runner. Input is deliberately a small documented binary
// envelope written by replay_height.py. No proprietary meshes are bundled.
#include "../src/HeightPlanCore.h"
#include "../src/TopologySubsetCore.h"
#include <fstream>
#include <iostream>
#include <iomanip>
#include <stdexcept>
using namespace vanity_ube_heel_adapter;
namespace s=surface_posture_core;
unsigned U32(std::istream&f){unsigned v{};if(!f.read(reinterpret_cast<char*>(&v),4))throw std::runtime_error("truncated header");return v;}
template<class T> std::vector<T> Read(std::istream&f){auto n=U32(f);if(!n||n>65535)throw std::runtime_error("count out of range");std::vector<T> out(n);if(!f.read(reinterpret_cast<char*>(out.data()),n*sizeof(T)))throw std::runtime_error("truncated array");return out;}
int main(int argc,char**argv){try{
    if(argc!=2)throw std::runtime_error("usage: replay-height input.bin");
    std::ifstream f(argv[1],std::ios::binary);if(U32(f)!=0x48544732)throw std::runtime_error("invalid header");
    const auto anchor=Read<s::Point>(f),target=Read<s::Point>(f);const auto triangles=Read<s::Triangle>(f), targetTriangles=Read<s::Triangle>(f);
    const auto stock=Read<s::Point>(f),n=Read<s::Point>(f),h=Read<s::Point>(f);
    if(f.peek()!=std::char_traits<char>::eof())throw std::runtime_error("trailing data");
    if(stock.size()!=n.size()||n.size()!=h.size())throw std::runtime_error("donor shape mismatch");
    auto selector=n;for(std::size_t i=0;i<n.size();++i)if(s::Norm2(s::V(h[i]))>s::Norm2(s::V(n[i])))selector[i]=h[i];
    auto aligned=topology_subset_core::Align(anchor,triangles,target,targetTriangles);
    if(!aligned.Complete()){std::cout<<"{\"decision\":\""<<aligned.status<<"\"}\n";return 0;}
    auto map=s::BuildMap(anchor,aligned.commonTriangles,stock,selector);
    auto fit=height_plan_core::FitSurface(map,anchor,aligned.commonTriangles,aligned.alignedTarget,aligned.commonTriangles,n,h,1.,2.);
    std::cout<<std::setprecision(14)<<"{\"map\":\""<<map.status<<"\",\"matched\":"<<map.entries.size()
       <<",\"NoHeel\":"<<fit.controls.noHeel<<",\"Heel\":"<<fit.controls.heel<<",\"rawHeel\":"<<fit.heel.raw
       <<",\"rms\":"<<fit.rms<<",\"normalizedResidual\":"<<fit.normalizedResidual
       <<",\"topology\":\""<<aligned.status<<"\",\"removedVertices\":"<<aligned.removedVertices
       <<",\"decision\":\""<<height_plan_core::Decision(fit,.15,false,2.)<<"\"}\n";
    if(fit.status!="bounded-height-fit")return 2;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
