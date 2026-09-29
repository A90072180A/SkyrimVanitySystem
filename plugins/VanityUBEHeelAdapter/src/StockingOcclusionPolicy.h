#pragma once
#include <cstddef>
#include <string_view>
namespace vanity_ube_heel_adapter::stocking_occlusion {
inline bool ShouldHide(bool enabled,std::size_t liveShoes,std::string_view coverage){
    return enabled&&liveShoes==1&&coverage=="opaque-closed";
}
}
