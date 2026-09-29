#pragma once
#include "PreparedCpbCore.h"
#include "BulkHeightLibrary.h"
#include <filesystem>
#include <memory>

namespace vanity_ube_heel_adapter::prepared_cpb {
struct Snapshot {
    std::string status{"pending"},error,generation,triFingerprint,agataError,glassError;
    bool ready{},agataReady{},glassReady{};
    std::vector<std::string> agataBlockedMorphs,glassBlockedMorphs;
    std::vector<bulk_height::SourceAlias> aliases;
};
// Probe is portable and contains only file/format operations; called by the
// worker (and tests), never by a Skyrim task. No model data is uploaded.
Snapshot Probe(const std::filesystem::path& profile,const std::filesystem::path& meshes);
void Initialize(void (*callback)());
void Reset();
std::shared_ptr<const Snapshot> Current();
} // namespace vanity_ube_heel_adapter::prepared_cpb
