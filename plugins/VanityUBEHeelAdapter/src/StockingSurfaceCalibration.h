#pragma once
#include "SourceGeometryEvidence.h"
namespace vanity_ube_heel_adapter::stocking_surface_calibration {
// Called only by the existing background snapshot writer. No engine objects,
// callbacks, model mutation, or morph application are used by this module.
void Process(nlohmann::json& document,
             const source_geometry_evidence::ReadMorph& readMorph);
}
