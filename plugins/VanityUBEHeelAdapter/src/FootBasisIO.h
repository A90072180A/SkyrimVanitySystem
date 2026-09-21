#pragma once
#include <nlohmann/json.hpp>
namespace vanity_ube_heel_adapter::foot_basis_io {
// Worker-only; receives owned JSON. Reads local resource files, never engine objects.
void Enrich(nlohmann::json& document);
}
