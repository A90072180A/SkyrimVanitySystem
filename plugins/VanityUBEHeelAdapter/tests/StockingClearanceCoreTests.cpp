#if defined(_WIN32)
#include <Windows.h>
#endif
#include "../src/StockingClearanceCore.h"
#include <iostream>
#include <stdexcept>
namespace s=vanity_ube_heel_adapter::surface_posture_core;
namespace c=vanity_ube_heel_adapter::stocking_clearance_core;
int checks=0;
void Check(bool ok){++checks;if(!ok)throw std::runtime_error("check "+std::to_string(checks));}
int main(){
 std::vector<s::Point> foot{{0,0,0},{2,0,0},{0,2,0}};std::vector<s::Triangle> tris{{0,1,2}};
 std::vector<s::Point> sock{{.3f,.3f,.5f},{.8f,.3f,.5f},{.3f,.8f,.5f}};
 std::vector<s::Point> delta{{0,1,-1},{0,1,-1},{0,1,-1}};
 auto map=s::BuildMap(foot,tris,sock,delta,1.,.01,1);Check(map.status=="mapped");
 auto measured=c::Measure(map,foot,sock);Check(measured.status=="measured");Check(measured.count==3);
 Check(std::abs(measured.rmsGap-.5)<1e-10);Check(measured.rmsTangentialGap<1e-10);Check(measured.negativeSide==0);
 auto self=c::Compare(map,foot,tris,foot,tris,sock,1.);Check(self.status=="compared-not-applied");
 Check(self.rotatingGap.rmsResidual==0);Check(self.rotatingGap.rawNoHeel==1.);Check(self.gapRotationRms==0);
 auto translated=foot;for(auto&p:translated)p[2]+=1;
 auto tr=c::Compare(map,foot,tris,translated,tris,sock,1.);Check(tr.gapRotationRms<1e-12);Check(tr.rawCoefficientSensitivity<1e-12);
 auto rotation=[](s::Point p){return s::Point{p[0],-p[2],p[1]};};
 std::vector<s::Point> rotatedFoot,rotatedSock;for(auto p:foot)rotatedFoot.push_back(rotation(p));
 for(auto p:sock)rotatedSock.push_back(rotation(p));
 for(std::size_t i=0;i<delta.size();++i)for(unsigned k=0;k<3;++k)delta[i][k]=rotatedSock[i][k]-sock[i][k];
 map=s::BuildMap(foot,tris,sock,delta,1.,.01,1);
 auto rotated=c::Compare(map,foot,tris,rotatedFoot,tris,sock,0.);
 Check(rotated.status=="compared-not-applied");Check(std::abs(rotated.rotatingGap.rawNoHeel-1)<1e-6);
 Check(rotated.rotatingGap.rmsResidual<1e-6);Check(rotated.fixedGap.rmsResidual>0.01);
 Check(rotated.gapRotationRms>.7);Check(rotated.rawCoefficientSensitivity>.1);
 auto from=c::TriangleFrame(s::V(foot[0]),s::V(foot[1]),s::V(foot[2]));
 auto to=c::TriangleFrame(s::V(rotatedFoot[0]),s::V(rotatedFoot[1]),s::V(rotatedFoot[2]));
 auto g=c::RotateGap({0,0,.5},*from,*to);Check(std::abs(g[1]+.5)<1e-12);Check(std::abs(g[2])<1e-12);
 auto bad=map;bad.entries[0].donorIndex=999;Check(c::Measure(bad,foot,sock).status=="invalid-donor");
 Check(c::Compare(bad,foot,tris,foot,tris,sock,0).status=="invalid-donor");
 bad=map;bad.entries[0].triangle[0]=100;Check(c::Measure(bad,foot,sock).status=="invalid-correspondence");
 bad=map;bad.entries[0].weights[0]=2;Check(c::Measure(bad,foot,sock).status=="invalid-correspondence");
 auto degenerate=foot;degenerate[1]=degenerate[0];Check(c::Compare(map,foot,tris,degenerate,tris,sock,0).status=="degenerate-triangle-frame");
 auto changed=tris;changed[0]={0,2,1};Check(c::Compare(map,foot,tris,foot,changed,sock,0).status=="target-topology-mismatch");
 auto nan=sock;nan[0][0]=NAN;Check(c::Measure(map,foot,nan).status=="invalid-donor");
 Check(!c::TriangleFrame({0,0,0},{0,0,0},{1,0,0}));Check(c::Quantile({1,2,3},.5)==2);
 std::vector<s::Point> surface{{-1,-1,0},{11,-1,0},{-1,11,0}},cloth,normal;
 std::vector<s::Triangle> clothTris;
 for(unsigned y=0;y<9;++y)for(unsigned x=0;x<9;++x){cloth.push_back({float(x)*.5f,float(y)*.5f,.5f});normal.push_back({0,0,1});}
 for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){unsigned i=y*9+x;clothTris.push_back({i,i+1,i+9});clothTris.push_back({i+1,i+10,i+9});}
 for(unsigned y=0;y<9;++y)for(unsigned x=0;x<9;++x)if(x==0||y==0||x==8||y==8)normal[y*9+x]={0,0,0};
 auto flatMap=s::BuildMap(surface,tris,cloth,normal,1,.01,1);
 auto proposal=c::Propose(flatMap,surface,cloth,clothTris,.15,.25,3);
 Check(proposal.status=="preview-only-not-installable");Check(!proposal.corrections.empty());Check(proposal.maxDisplacement<=.25+1e-12);
 for(const auto&p:proposal.corrections){Check(p.index%9>1&&p.index%9<7&&p.index/9>1&&p.index/9<7);Check(p.displacement[2]<=0);Check(.5+p.displacement[2]>=.15-1e-10);}
 Check(c::Propose(flatMap,surface,cloth,clothTris,-1,.25).status=="invalid-proposal-input");
 auto wrong=clothTris;wrong[0][0]=900;Check(c::Propose(flatMap,surface,cloth,wrong).status=="invalid-donor-triangles");
 Check(c::Propose(flatMap,surface,cloth,clothTris,.75,.25).status=="no-safe-correction");
 std::cout<<checks<<" clearance checks passed\n";
}
