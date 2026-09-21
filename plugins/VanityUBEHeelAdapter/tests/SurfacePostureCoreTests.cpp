#include "../src/SurfacePostureCore.h"
#include <iostream>
#include <stdexcept>
using namespace vanity_ube_heel_adapter::surface_posture_core;
int count=0;
void Check(bool b){++count;if(!b)throw std::runtime_error("check "+std::to_string(count));}
int main(){
    auto a=Project({.25,.25,1},{0,0,0},{1,0,0},{0,1,0});Check(std::abs(a.distance2-1)<1e-12);Check(std::abs(a.weights[0]-.5)<1e-12);
    auto b=Project({2,0,0},{0,0,0},{1,0,0},{0,1,0});Check(std::abs(b.distance2-1)<1e-12);
    auto c=Project({0,1,0},{0,0,0},{0,0,0},{1,0,0});Check(std::abs(c.distance2-1)<1e-12);
    std::vector<Point> foot{{0,0,0},{1,0,0},{0,1,0}},stock{{.1f,.1f,.1f},{.2f,.2f,.1f},{.3f,.3f,.1f}},delta(3,Point{0,0,2});
    std::vector<Triangle> tri{{0,1,2}};
    auto map=BuildMap(foot,tri,stock,delta,.2,.01,3);Check(map.status=="mapped");Check(map.entries.size()==3);Check(map.coverage==1);
    auto out=FitTarget(map,foot,tri,foot,tri,1);Check(out.rawNoHeel==1);Check(out.rmsResidual==0);Check(!out.hasMotion);
    auto target=foot;for(auto& p:target)p[2]-=1;
    out=FitTarget(map,foot,tri,target,tri,1);Check(std::abs(out.rawNoHeel-.5)<1e-7);Check(out.rmsResidual<1e-7);Check(Recommendation(out)=="candidate-needs-visual-validation");
    for(auto& p:target){p[2]-=3;}
    out=FitTarget(map,foot,tri,target,tri,1);Check(out.rawNoHeel<0);Check(Recommendation(out)=="outside-configured-0-1-range");
    target=foot;for(auto& p:target)p[0]+=1;out=FitTarget(map,foot,tri,target,tri,1);Check(out.rawNoHeel==1);Check(out.rmsResidual>.9);Check(Recommendation(out)=="residual-too-large");
    Check(BuildMap(foot,tri,stock,delta,.01,.01,3).status=="insufficient-correspondences");
    Check(BuildMap(foot,tri,stock,delta,-1,.01,3).status=="invalid-map-input");
    auto bad=tri;bad[0][0]=100;Check(BuildMap(foot,bad,stock,delta,.2,.01,3).status=="invalid-reference-surface");
    bad=tri;bad[0]={0,2,1};Check(FitTarget(map,foot,tri,foot,bad,1).status=="target-topology-mismatch");
    target=foot;target[0][0]=NAN;Check(FitTarget(map,foot,tri,target,tri,1).status=="nonfinite-foot");
    auto ds=delta;ds[0][0]=INFINITY;Check(BuildMap(foot,tri,stock,ds,.2,.01,3).status=="nonfinite-donor");
    auto badmap=map;badmap.entries[0].triangle[0]=999;Check(FitTarget(badmap,foot,tri,foot,tri,1).status=="invalid-correspondence");
    badmap=map;badmap.entries[0].weights[0]=4;Check(FitTarget(badmap,foot,tri,foot,tri,1).status=="invalid-correspondence");
    badmap=map;for(auto& v:badmap.entries)v.delta={0,0,0};Check(FitTarget(badmap,foot,tri,foot,tri,1).status=="singular-donor");
    // Exercise a multi-level BVH and compare its answer to exhaustive projection.
    std::vector<Point> grid;std::vector<Triangle> gridtri;
    for(int i=0;i<40;++i){auto n=static_cast<unsigned>(grid.size());grid.push_back({float(i),0,0});grid.push_back({float(i)+.5f,0,0});grid.push_back({float(i),.5f,0});gridtri.push_back({n,n+1,n+2});}
    Surface surf(grid,gridtri);Check(surf.valid);
    for(int i=0;i<25;++i){Vec q{1.51*i-.3,.25,1};auto best=surf.Nearest(q);double ref=INFINITY;
      for(auto t:gridtri)ref=std::min(ref,Project(q,V(grid[t[0]]),V(grid[t[1]]),V(grid[t[2]])).distance2);
      Check(std::abs(best.distance2-ref)<1e-10);}
    std::cout<<count<<" checks passed\n";
}
