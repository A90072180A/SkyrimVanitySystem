#pragma once
#include <nlohmann/json.hpp>
namespace vanity_ube_heel_adapter::stocking_clearance_diagnostics {
// Called only by the existing snapshot writer, AFTER source/local reconstruction.
void Process(nlohmann::json& document);
}
