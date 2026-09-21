#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vanity_ube_heel_adapter::bodyslide {

struct PostureCandidate {
    float posture{0.0F};
    float lowWeightValue{0.0F};
    float highWeightValue{0.0F};
    bool highConfidence{false};
    std::string source;
    std::string setName;
    std::string modelStem;
    std::string sourceFile;
};

using IndexReadyCallback = void (*)();

void Reset();
void PrepareAsync(
    bool a_diagnostics,
    IndexReadyCallback a_readyCallback = nullptr);
bool IsReady();

std::optional<PostureCandidate> ResolvePosture(
    std::span<const std::string> a_modelPaths,
    float a_actorWeight,
    bool a_diagnostics);

}  // namespace vanity_ube_heel_adapter::bodyslide
