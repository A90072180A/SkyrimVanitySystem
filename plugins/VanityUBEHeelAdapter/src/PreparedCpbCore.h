#pragma once
// Pure policy for the exact, privately prepared CPB surface bundle. No game objects.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vanity_ube_heel_adapter::prepared_cpb {
inline constexpr std::string_view Model0="!ube\\caenarvon\\cosplay\\cpb_sb_0.nif";
inline constexpr std::string_view Model1="!ube\\caenarvon\\cosplay\\cpb_sb_1.nif";
inline constexpr std::string_view OriginalTri="!ube\\caenarvon\\cosplay\\cpb_sb.tri";
inline constexpr std::string_view PreparedTri="!ube\\caenarvon\\cosplay\\vha\\cpb_sb_surface_v1.tri";
inline constexpr std::string_view MainShape="SB_bodystocking";
inline constexpr std::string_view FootShape="VHA_CPB_CoveredFoot";
inline constexpr std::string_view FitMorph="VHA_GlassFootFit";
inline constexpr std::uint32_t VertexCount=23340;
inline constexpr std::string_view AgataArmor="witchy agata heels.esp|00000802";
inline constexpr std::string_view AgataAddon="witchy agata heels ube patch.esp|00000800";
inline constexpr std::string_view AgataModel="!ube\\[spaz490]\\witchy agata heels\\agata_1.nif";
inline constexpr std::string_view GlassArmor="glass high-heeled shoes.esp|00000804";
inline constexpr std::string_view GlassAddon="glass high-heeled shoes ube patch.esp|00000800";
inline constexpr std::string_view GlassModel="!ube\\blacksmith\\heelspack\\flamingoshoes\\flamingoshoes_1.nif";
inline std::string Canon(std::string_view in) {
    std::string s(in);
    for(auto& c:s){if(c=='/')c='\\';else if(c>='A'&&c<='Z')c=static_cast<char>(c-'A'+'a');}
    if(s.starts_with("meshes\\"))s.erase(0,7);
    return s;
}
inline bool IsModel(std::string_view model){const auto s=Canon(model);return s==Model0||s==Model1;}
inline bool ResourceMatches(std::string_view model,std::string_view tri){return IsModel(model)&&Canon(tri)==PreparedTri;}
inline bool IsAgata(std::string_view armor,std::string_view addon,std::string_view model){return Canon(armor)==AgataArmor&&Canon(addon)==AgataAddon&&Canon(model)==AgataModel;}
inline bool IsGlass(std::string_view armor,std::string_view addon,std::string_view model){return Canon(armor)==GlassArmor&&Canon(addon)==GlassAddon&&Canon(model)==GlassModel;}
struct Shape {std::string name;std::uint32_t vertices{},maxIndex{};bool noHeel{},heel{};};
inline bool CompleteShapes(std::span<const Shape> shapes){
    if(shapes.size()!=2)return false;
    unsigned main=0,foot=0;
    for(const auto& s:shapes){
        if(s.vertices!=VertexCount||s.maxIndex>=s.vertices||!s.noHeel||!s.heel)return false;
        if(s.name==MainShape)++main;else if(s.name==FootShape)++foot;else return false;
    }
    return main==1&&foot==1;
}
struct MorphValue {std::string name;double value{};};
inline bool ContextSafe(double weight,unsigned sex,std::span<const MorphValue> values,std::span<const std::string> blocked){
    // Only source weight zero was measured. No interpolation/negative clamping.
    if(!std::isfinite(weight)||weight<0||weight>0.001||sex!=1)return false;
    for(const auto& v:values){
        if(!std::isfinite(v.value))return false;
        if(std::abs(v.value)<=1e-6)continue;
        if(v.name==FitMorph)return false; // Never replace another owner's fit key.
        if(v.name=="NoHeel"||v.name=="Heel")continue; // Scoped height is validated separately.
        if(std::find(blocked.begin(),blocked.end(),v.name)!=blocked.end())return false;
    }
    return true;
}
inline bool AtHeight(double noHeel,double heel,double expected){return std::isfinite(noHeel)&&std::isfinite(heel)&&std::abs(noHeel)<=1e-6&&std::abs(heel-expected)<=1e-5;}
// Compare-and-restore ownership. A visible external write while we own a hidden
// node yields for the remainder of this eligibility interval; never fights it.
// An unobservable same-value write by another plugin cannot carry ownership data.
struct VisibilityLease {
    bool owned{},original{},last{},yielded{};
    std::optional<bool> Hide(bool current){
        if(yielded)return {};
        if(owned){if(current!=last){owned=false;yielded=true;}return {};}
        original=current;last=true;owned=true;
        return current?std::optional<bool>{}:std::optional<bool>{true};
    }
    std::optional<bool> Release(bool current){
        auto result=owned&&current==last&&current!=original?std::optional<bool>{original}:std::optional<bool>{};
        *this={};return result;
    }
};
} // namespace vanity_ube_heel_adapter::prepared_cpb
