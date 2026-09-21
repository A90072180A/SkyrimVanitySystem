#pragma once
// Read-only, deliberately bounded SSE NIF source reader (20.2.0.7/user12/BS100).
// Format references: nifly BasicTypes.cpp, Objects.cpp, Geometry.cpp, Skin.cpp.
// Does not execute controllers, resolve textures, load engine objects or write NIFs.
#include "FootSnapshotCore.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
namespace vanity_ube_heel_adapter::nif_source_core {
namespace core = foot_snapshot_core;
struct Transform { std::array<float,9> rotation{}; core::Point translation{}; float scale{}; };
struct Shape {
    std::string name;
    std::vector<std::string> bodyTris;
    Transform local, skin;
    core::Result mesh;
};
struct Result {
    std::string status{"not-parsed"}, error;
    std::vector<Shape> shapes;
    bool Complete() const { return status == "complete"; }
};
class Reader {
    std::span<const std::uint8_t> bytes; std::size_t pos{};
public:
    explicit Reader(std::span<const std::uint8_t> b):bytes(b){}
    std::size_t Left()const{return bytes.size()-pos;}
    std::span<const std::uint8_t> Take(std::size_t n){if(n>Left())throw std::runtime_error("truncated-nif");auto r=bytes.subspan(pos,n);pos+=n;return r;}
    std::uint8_t U8(){return Take(1)[0];}
    std::uint16_t U16(){auto b=Take(2);return std::uint16_t(b[0]|(unsigned(b[1])<<8));}
    std::uint32_t U32(){auto a=U16();return a|(std::uint32_t(U16())<<16);}
    std::uint64_t U64(){auto a=U32();return a|(std::uint64_t(U32())<<32);}
    float F32(){auto v=std::bit_cast<float>(U32());if(!std::isfinite(v))throw std::runtime_error("nonfinite-nif-value");return v;}
    bool Bool(){auto b=U8();if(b>1)throw std::runtime_error("invalid-nif-bool");return b!=0;}
    std::string Text(std::size_t n){if(n>65536)throw std::runtime_error("nif-string-limit");auto b=Take(n);return std::string(reinterpret_cast<const char*>(b.data()),b.size());}
    std::vector<std::uint16_t> U16s(std::size_t n){if(n>196605)throw std::runtime_error("nif-index-limit");std::vector<std::uint16_t> v;v.reserve(n);for(std::size_t i=0;i<n;++i)v.push_back(U16());return v;}
};
struct Block{std::string type;std::span<const std::uint8_t> bytes;};
struct Object{std::string name;std::vector<std::uint32_t> extra;Transform local;};
inline std::uint32_t Count(Reader& r,std::uint32_t max){auto n=r.U32();if(n>max)throw std::runtime_error("nif-count-limit");return n;}
inline std::string StringAt(const std::vector<std::string>& s,std::uint32_t id){if(id==0xffffffffu)return {};if(id>=s.size())throw std::runtime_error("nif-string-index");return s[id];}
inline Transform ReadTransform(Reader& r,bool translationFirst){Transform t;if(translationFirst)for(auto&v:t.translation)v=r.F32();for(auto&v:t.rotation)v=r.F32();if(!translationFirst)for(auto&v:t.translation)v=r.F32();t.scale=r.F32();return t;}
inline Object ReadObject(Reader& r,const std::vector<std::string>& strings){Object o;o.name=StringAt(strings,r.U32());auto n=Count(r,4096);for(unsigned i=0;i<n;++i)o.extra.push_back(r.U32());r.U32();r.U32();o.local=ReadTransform(r,true);r.U32();return o;}
inline std::vector<std::string> ExtraPaths(const Object& o,const std::vector<Block>& blocks,const std::vector<std::string>& strings){
    std::vector<std::string> paths;
    for(auto id:o.extra){if(id>=blocks.size())throw std::runtime_error("nif-extra-index");const auto& b=blocks[id];if(b.type!="NiStringExtraData")continue;Reader r(b.bytes);auto name=StringAt(strings,r.U32());auto value=StringAt(strings,r.U32());if(r.Left())throw std::runtime_error("nif-string-extra-size");if(name=="BODYTRI"&&!value.empty())paths.push_back(value);}
    return paths;
}
inline core::Result ReadPartition(std::span<const std::uint8_t> bytes){
    Reader r(bytes);auto count=r.U32();if(count!=1)throw std::runtime_error("unsupported-nif-multiple-partitions");
    const auto size=r.U32(),stride=r.U32();const auto desc=r.U64();
    if(stride<16||stride>256||size%stride||(desc&15u)*4u!=stride)throw std::runtime_error("invalid-nif-stride");
    auto vertices=size/stride;if(!vertices||vertices>65535)throw std::runtime_error("invalid-nif-vertices");
    // SSE NiSkinPartition stores full-precision positions regardless of VF_FULLPREC.
    if(!((desc>>44)&1u))throw std::runtime_error("unsupported-nif-position-stream");
    unsigned first=stride;for(unsigned i=1;i<10;++i)if((desc>>44)&(1u<<i)){auto offset=unsigned((desc>>(4*i+2))&0x3cu);if(offset<16||offset>=stride)throw std::runtime_error("invalid-nif-attribute");first=(std::min)(first,offset);}
    if(first!=16)throw std::runtime_error("unsupported-nif-position-size");
    auto raw=r.Take(size);auto nv=r.U16(),nt=r.U16(),nb=r.U16(),ns=r.U16(),nw=r.U16();
    if(nv!=vertices||!nt||nb>256||ns!=0||nw!=4)throw std::runtime_error("unsupported-nif-partition-domain");
    r.Take(std::size_t(nb)*2);
    if(r.Bool()){auto map=r.U16s(nv);for(unsigned i=0;i<nv;++i)if(map[i]!=i)throw std::runtime_error("unsupported-nif-vertex-map");}
    if(r.Bool())r.Take(std::size_t(nv)*nw*4);
    if(!r.Bool())throw std::runtime_error("nif-no-faces");
    auto triangles=r.U16s(std::size_t(nt)*3);
    if(r.Bool())r.Take(std::size_t(nv)*nw);
    r.U8();r.U8();auto pd=r.U64();if(pd!=desc)throw std::runtime_error("nif-partition-descriptor-mismatch");
    const auto trueTriangles=r.U16s(std::size_t(nt)*3);
    if(triangles!=trueTriangles||r.Left())throw std::runtime_error("nif-triangle-domain-mismatch");
    return core::Decode(raw,triangles,vertices,stride,4);
}
inline Result Parse(std::span<const std::uint8_t> bytes){
    Result out;
    try{
        if(bytes.size()>64u*1024u*1024u)throw std::runtime_error("nif-file-limit");
        Reader r(bytes);std::string header;while(header.size()<128){auto c=r.U8();header+=char(c);if(c=='\n')break;}
        if(header!="Gamebryo File Format, Version 20.2.0.7\n")throw std::runtime_error("unsupported-nif-header");
        if(r.U32()!=0x14020007||r.U8()!=1||r.U32()!=12)throw std::runtime_error("unsupported-nif-version");
        const auto n=Count(r,4096);if(!n||r.U32()!=100)throw std::runtime_error("unsupported-nif-stream");
        for(int i=0;i<3;++i)r.Take(r.U8());
        auto typesN=r.U16();if(!typesN||typesN>4096)throw std::runtime_error("nif-type-count");std::vector<std::string> types;
        for(unsigned i=0;i<typesN;++i)types.push_back(r.Text(r.U32()));
        auto ids=r.U16s(n);std::vector<std::uint32_t> sizes;for(unsigned i=0;i<n;++i)sizes.push_back(r.U32());
        const auto ns=Count(r,65536);r.U32();std::vector<std::string> strings;for(unsigned i=0;i<ns;++i)strings.push_back(r.Text(r.U32()));
        auto groups=Count(r,4096);for(unsigned i=0;i<groups;++i)r.U32();
        std::vector<Block> blocks;for(unsigned i=0;i<n;++i){if(ids[i]>=types.size())throw std::runtime_error("nif-type-index");blocks.push_back({types[ids[i]],r.Take(sizes[i])});}
        auto rootsN=Count(r,4096);std::vector<std::uint32_t> roots;for(unsigned i=0;i<rootsN;++i){auto id=r.U32();if(id>=n)throw std::runtime_error("nif-root-index");roots.push_back(id);}if(r.Left())throw std::runtime_error("nif-trailing-data");
        std::vector<std::vector<std::string>> paths(n);std::vector<int> parents(n,-1);
        for(unsigned i=0;i<n;++i){const auto& b=blocks[i];if(b.type!="NiNode"&&b.type!="BSTriShape")continue;Reader br(b.bytes);auto obj=ReadObject(br,strings);paths[i]=ExtraPaths(obj,blocks,strings);if(b.type=="NiNode"){auto children=Count(br,4096);for(unsigned j=0;j<children;++j){auto c=br.U32();if(c==0xffffffffu)continue;if(c>=n||parents[c]!=-1)throw std::runtime_error("nif-ambiguous-parent");parents[c]=int(i);}}}
        for(unsigned i=0;i<n;++i){const auto& b=blocks[i];if(b.type!="BSTriShape")continue;
            Shape shape;Reader sr(b.bytes);auto obj=ReadObject(sr,strings);shape.name=obj.name;shape.local=obj.local;for(int j=0;j<4;++j)sr.F32();auto skinID=sr.U32();sr.U32();sr.U32();sr.U64();sr.U16();sr.U16();auto dataSize=sr.U32();
            if(dataSize!=0){shape.mesh.status="unsupported-inline-nif-geometry";out.shapes.push_back(std::move(shape));continue;}
            if(sr.U32()!=0||sr.Left())throw std::runtime_error("unsupported-nif-particle-data");
            int ancestor=int(i);for(unsigned depth=0;ancestor>=0&&depth<=n;++depth){if(depth==n)throw std::runtime_error("nif-parent-cycle");if(!paths[ancestor].empty()){shape.bodyTris=paths[ancestor];break;}ancestor=parents[ancestor];}
            if(skinID>=n||(blocks[skinID].type!="BSDismemberSkinInstance"&&blocks[skinID].type!="NiSkinInstance")){shape.mesh.status="unsupported-nif-skin";out.shapes.push_back(std::move(shape));continue;}
            Reader skin(blocks[skinID].bytes);auto dataID=skin.U32(),partID=skin.U32();
            if(dataID>=n||partID>=n||blocks[dataID].type!="NiSkinData"||blocks[partID].type!="NiSkinPartition")throw std::runtime_error("nif-skin-links");
            Reader data(blocks[dataID].bytes);shape.skin=ReadTransform(data,false);
            try{shape.mesh=ReadPartition(blocks[partID].bytes);}catch(const std::exception& e){shape.mesh.status=e.what();}
            out.shapes.push_back(std::move(shape));
        }
        out.status=out.shapes.empty()?"no-supported-shapes":"complete";
    }catch(const std::exception& e){out.status="unreadable-or-unsupported-nif";out.error=e.what();out.shapes.clear();}
    return out;
}
} // namespace vanity_ube_heel_adapter::nif_source_core
