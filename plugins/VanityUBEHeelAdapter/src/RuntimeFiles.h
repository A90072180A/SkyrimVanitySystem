#pragma once
#include <nlohmann/json.hpp>
#include <optional>
namespace vanity_ube_heel_adapter::runtime_files {
using Json=nlohmann::json;
void Initialize(void(*notify)());
// Initial bounded configuration read; never scans meshes or BodySlide libraries.
std::optional<Json> ReadInitial();
std::optional<Json> TakePending();
void Status(Json snapshot); // owned values, no game pointers; background atomic I/O
}
