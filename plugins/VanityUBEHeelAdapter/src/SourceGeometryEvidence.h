#pragma once
#include <nlohmann/json.hpp>
#include <functional>
#include <cstdint>
#include <string>
namespace vanity_ube_heel_adapter::source_geometry_evidence {
using ReadMorph = std::function<nlohmann::json(const std::string&, const std::string&, const std::string&, std::uint32_t)>;
// Both calls run ONLY on the existing snapshot writer and own their data.
void Resolve(nlohmann::json& document);
void Measure(nlohmann::json& document, const ReadMorph& readMorph);
}
