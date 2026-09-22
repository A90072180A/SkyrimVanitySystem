#pragma once
#include "HeightPlanCore.h"
#include <nlohmann/json.hpp>
#include <functional>
#include <optional>
#include <string>
#include <vector>
namespace vanity_ube_heel_adapter::height_profiles {
using Json=nlohmann::json;
struct ShapeCapability { std::string name; bool noHeel{},heel{}; std::uint32_t maxIndex{}; };
struct Capability { std::string status,resource,fingerprint;std::vector<ShapeCapability> shapes; };
void Initialize(void (*requestSnapshot)());
std::uint64_t BeginSession();
std::uint64_t Session();
// Called from SKSE tasks; all resource I/O stays in background workers.
std::optional<Capability> RequestCapability(const std::string& resource);
// Called only by the geometry writer with owned JSON; never contains pointers.
void ObserveFoot(const Json& document);
void Publish(Json profile);
std::optional<Json> Lookup(const std::string& footwear,const std::string& footwearAddon,
    const std::string& stocking,const std::string& stockingAddon,const std::string& context);
}
