#include "../src/NifSourceCore.h"
#include "../src/NativeFootFitCore.h"
#include <iostream>
#include <cstring>
namespace n=vanity_ube_heel_adapter::nif_source_core;
namespace f=vanity_ube_heel_adapter::native_foot_fit_core;
using Bytes=std::vector<std::uint8_t>;
void u8(Bytes&b,unsigned v){b.push_back(static_cast<std::uint8_t>(v));}
void u16(Bytes&b,unsigned v){u8(b,v);u8(b,v>>8);}
void u32(Bytes&b,std::uint32_t v){u16(b,v);u16(b,v>>16);}
void u64(Bytes&b,std::uint64_t v){u32(b,std::uint32_t(v));u32(b,std::uint32_t(v>>32));}
void fl(Bytes&b,float v){u32(b,std::bit_cast<std::uint32_t>(v));}
void text(Bytes&b,const std::string&s){u32(b,static_cast<std::uint32_t>(s.size()));b.insert(b.end(),s.begin(),s.end());}
void xform(Bytes&b,bool first){if(first)for(int i=0;i<3;++i)fl(b,0);for(int i=0;i<9;++i)fl(b,i%4==0?1.f:0.f);if(!first)for(int i=0;i<3;++i)fl(b,0);fl(b,1);}
Bytes Fixture(){
 std::vector<Bytes>b(5);auto&shape=b[0];u32(shape,0);u32(shape,1);u32(shape,1);u32(shape,0xffffffffu);u32(shape,14);xform(shape,true);u32(shape,0xffffffffu);for(int i=0;i<4;++i)fl(shape,1);u32(shape,2);u32(shape,0xffffffffu);u32(shape,0xffffffffu);u64(shape,0x100000000004);u16(shape,0);u16(shape,0);u32(shape,0);u32(shape,0);
 u32(b[1],1);u32(b[1],2);u32(b[2],3);u32(b[2],4);u32(b[2],0xffffffffu);u32(b[2],0);xform(b[3],false);u32(b[3],0);u8(b[3],0);
 auto&p=b[4];u32(p,1);u32(p,48);u32(p,16);u64(p,0x100000000004);for(int v=0;v<3;++v){fl(p,float(v));fl(p,float(v*v));fl(p,0);fl(p,0);}u16(p,3);u16(p,1);u16(p,0);u16(p,0);u16(p,4);u8(p,1);for(unsigned i=0;i<3;++i)u16(p,i);u8(p,0);u8(p,1);for(unsigned i=0;i<3;++i)u16(p,i);u8(p,0);u8(p,0);u8(p,0);u64(p,0x100000000004);for(unsigned i=0;i<3;++i)u16(p,i);
 Bytes out;std::string h="Gamebryo File Format, Version 20.2.0.7\n";out.insert(out.end(),h.begin(),h.end());u32(out,0x14020007);u8(out,1);u32(out,12);u32(out,5);u32(out,100);for(int i=0;i<3;++i)u8(out,0);
 const char*types[]={"BSTriShape","NiStringExtraData","NiSkinInstance","NiSkinData","NiSkinPartition"};u16(out,5);for(auto t:types)text(out,t);for(unsigned i=0;i<5;++i)u16(out,i);for(auto&v:b)u32(out,static_cast<std::uint32_t>(v.size()));u32(out,3);u32(out,20);text(out,"Feet");text(out,"BODYTRI");text(out,"sample.tri");u32(out,0);for(auto&v:b)out.insert(out.end(),v.begin(),v.end());u32(out,1);u32(out,0);return out;
}
int main(){unsigned checks=0;auto check=[&](bool ok){++checks;if(!ok){std::cerr<<"check "<<checks<<" failed\n";std::exit(1);}};
 auto b=Fixture();auto r=n::Parse(b);check(r.Complete());check(r.shapes.size()==1);check(r.shapes[0].mesh.Complete());check(r.shapes[0].mesh.positions.size()==3);check(r.shapes[0].bodyTris==std::vector<std::string>{"sample.tri"});check(r.shapes[0].name=="Feet");
 for(std::size_t i=0;i<b.size();++i)check(!n::Parse(std::span(b).first(i)).Complete());
 auto bad=b;bad.push_back(1);check(!n::Parse(bad).Complete());bad=b;bad[0]='X';check(!n::Parse(bad).Complete());
 std::vector<f::Point> base(4),corr(4),a(4),c(4),y(4);for(unsigned i=0;i<4;++i){a[i]={float(i+1),0,1};c[i]={0,float(i+1),-1};corr[i]={.1f,.2f,.3f};for(unsigned j=0;j<3;++j)y[i][j]=corr[i][j]+1.5f*a[i][j]-.25f*c[i][j];}
 auto fit=f::Fit(y,base,corr,a,c);check(fit.status=="diagnostic-fit");check(std::abs(fit.first-1.5)<1e-6);check(std::abs(fit.second+.25)<1e-6);check(fit.rms<1e-6);check(f::Fit(y,base,corr,a,a).status=="degenerate-basis");y[0][0]=NAN;check(f::Fit(y,base,corr,a,c).status=="nonfinite-input");check(f::Fit({},base,corr,a,c).status=="invalid-array-size");std::cout<<checks<<" checks passed\n";
}
