#pragma once
// Exact ordered topology correspondence after deletion of whole islands components.
// Does NOT spatially guess or rescale feet, and never changes an original mesh.
#include "SurfacePostureCore.h"
#include <map>
#include <functional>
namespace vanity_ube_heel_adapter::topology_subset_core {
using Point=surface_posture_core::Point;
using Triangle=surface_posture_core::Triangle;
struct Result {
    std::string status{"unsupported-topology"};
    std::vector<Point> alignedTarget;
    std::vector<Triangle> commonTriangles;
    std::vector<std::uint32_t> anchorToTarget;
    std::size_t removedVertices{},removedComponents{};
    bool Complete() const { return status=="exact-topology"||status=="exact-ordered-component-subset"; }
};
inline Result Align(std::span<const Point> anchor,std::span<const Triangle> at,
                    std::span<const Point> target,std::span<const Triangle> tt) {
    Result out;
    if(anchor.empty()||target.empty()||anchor.size()>65535||target.size()>anchor.size()||at.empty()||tt.empty()||at.size()>65535||tt.size()>at.size())return out;
    for(auto p:anchor)if(!surface_posture_core::Finite(surface_posture_core::V(p)))return out;
    for(auto p:target)if(!surface_posture_core::Finite(surface_posture_core::V(p)))return out;
    for(auto t:at)for(auto i:t)if(i>=anchor.size())return out;
    for(auto t:tt)for(auto i:t)if(i>=target.size())return out;
    if(anchor.size()==target.size()) {
        if(at.size()!=tt.size()||!std::equal(at.begin(),at.end(),tt.begin()))return out;
        out.status="exact-topology";out.alignedTarget.assign(target.begin(),target.end());out.commonTriangles.assign(at.begin(),at.end());
        out.anchorToTarget.resize(anchor.size());std::iota(out.anchorToTarget.begin(),out.anchorToTarget.end(),0);return out;
    }
    const auto missing=anchor.size()-target.size();
    // This narrowly covers deleted islands islands (e.g. nails), not decimation.
    if(missing>anchor.size()/10)return out;
    std::vector<std::uint32_t> parent(anchor.size());std::iota(parent.begin(),parent.end(),0);
    auto root=[&](std::uint32_t i){while(parent[i]!=i){parent[i]=parent[parent[i]];i=parent[i];}return i;};
    for(auto t:at){auto r=root(t[0]);parent[root(t[1])]=r;parent[root(t[2])]=r;}
    std::map<std::uint32_t,std::vector<std::uint32_t>> components;
    for(std::uint32_t i=0;i<anchor.size();++i)components[root(i)].push_back(i);
    std::vector<std::vector<std::uint32_t>> islands;
    for(auto&[r,c]:components)if(c.size()<=256&&c.size()<=missing)islands.push_back(c);
    if(islands.empty()||islands.size()>16)return out;
    std::vector<std::size_t> suffix(islands.size()+1);
    for(auto i=islands.size();i-->0;)suffix[i]=suffix[i+1]+islands[i].size();
    std::vector<bool> removed(anchor.size());std::size_t attempts=0,matches=0;
    auto verify=[&](std::size_t count){
        if(++attempts>128)return;
        std::vector<std::uint32_t> map(anchor.size(),UINT32_MAX);std::uint32_t next=0;
        for(std::size_t i=0;i<map.size();++i)if(!removed[i])map[i]=next++;
        if(next!=target.size())return;
        std::vector<Triangle> common;common.reserve(tt.size());std::size_t j=0;
        for(auto t:at){if(removed[t[0]]||removed[t[1]]||removed[t[2]])continue;
            if(j>=tt.size()||Triangle{map[t[0]],map[t[1]],map[t[2]]}!=tt[j++])return;common.push_back(t);}
        if(j!=tt.size())return;
        if(++matches>1)return;
        out.status="exact-ordered-component-subset";out.anchorToTarget=std::move(map);out.commonTriangles=std::move(common);
        out.alignedTarget.assign(anchor.begin(),anchor.end());
        for(std::size_t i=0;i<anchor.size();++i)if(!removed[i])out.alignedTarget[i]=target[out.anchorToTarget[i]];
        out.removedVertices=missing;out.removedComponents=count;
    };
    std::function<void(std::size_t,std::size_t,std::size_t)> visit=[&](auto index,auto left,auto count){
        if(matches>1||attempts>128||left>suffix[index])return;
        if(!left){verify(count);return;}
        if(index==islands.size())return;
        visit(index+1,left,count);
        if(islands[index].size()<=left){for(auto i:islands[index])removed[i]=true;visit(index+1,left-islands[index].size(),count+1);for(auto i:islands[index])removed[i]=false;}
    };
    visit(0,missing,0);
    if(matches!=1||attempts>128){out={};out.status=attempts>128?"subset-search-budget-exceeded":matches>1?"ambiguous-component-subset":"unsupported-topology";}
    return out;
}
}
