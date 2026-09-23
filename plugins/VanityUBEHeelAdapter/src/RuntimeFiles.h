#pragma once
#include <nlohmann/json.hpp>
#include <optional>
#include <cstdint>
namespace vanity_ube_heel_adapter::runtime_files {
using Json=nlohmann::json;
void Initialize(void(*notify)());
// Initial bounded configuration read; never scans meshes or BodySlide libraries.
std::optional<Json> ReadInitial();
std::optional<Json> TakePending();
// Main-thread acknowledgement; the editor must not equate a file save with this.
void Acknowledge(const Json& effective,std::uint64_t revision);
// Explicit requests are validated on the worker even if effective values did not
// change. Taking a request is not its acknowledgement; delivery can be retried.
struct ForcedReload { Json effective; std::uint64_t requestId{}; };
std::uint64_t RequestReload();
std::optional<ForcedReload> TakeForcedReload();
void AcknowledgeForcedReload(std::uint64_t requestId, std::uint64_t revision);
void Status(Json snapshot); // owned values, no game pointers; background atomic I/O
}
