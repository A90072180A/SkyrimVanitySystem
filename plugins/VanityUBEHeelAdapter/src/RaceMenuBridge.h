#pragma once
#include "ScopedMorphTransaction.h"

namespace vanity_ube_heel_adapter::racemenu {

struct AttachmentRecord {
    RE::FormID actorFormID{0};
    RE::FormID armorFormID{0};
    RE::FormID armorAddonFormID{0};
    RE::NiPointer<RE::NiAVObject> object;
    std::string bodyTriPath;
    std::uint64_t sequence{0};
};

struct SceneBodyTriRecord {
    RE::NiPointer<RE::NiAVObject> object;
    std::string bodyTriPath;
    std::string nodeName;
};

struct GeometryDiagnosticRecord {
    std::string name;
    std::string rttiName;
    std::uint32_t vertexCount{0};
    std::uint32_t triangleCount{0};
    bool hasSkin{false};
    std::uint32_t skinPartitionCount{0};
    std::uint32_t skinPartitionVertexCount{0};
    float modelBoundCenterX{0.0F};
    float modelBoundCenterY{0.0F};
    float modelBoundCenterZ{0.0F};
    float modelBoundRadius{0.0F};
    float worldBoundCenterX{0.0F};
    float worldBoundCenterY{0.0F};
    float worldBoundCenterZ{0.0F};
    float worldBoundRadius{0.0F};
};

struct BipedPartRecord {
    std::uint32_t slotIndex{0};
    std::uint32_t slotNumber{0};
    bool buffered{false};
    RE::FormID itemFormID{0};
    RE::FormID addonFormID{0};
    RE::NiPointer<RE::NiAVObject> partClone;
    std::string rootName;
    std::vector<std::string> bodyTriPaths;
    std::vector<std::string> geometryNames;
    std::vector<GeometryDiagnosticRecord> geometryDetails;
};

using AttachmentChangedCallback = void (*)();

bool Initialize();
bool Available();
void ResetAttachmentCache();
std::string ActorMorphContext(RE::Actor* actor);
bool IsLive(RE::NiAVObject* object);
bool RestoreScopedMorphs(RE::Actor* actor, RE::NiAVObject* root);
bool ApplyScopedMorphs(RE::Actor* actor, RE::NiAVObject* root,
    std::span<const scoped_morph_transaction::Target> targets, std::string_view context);
void SetAttachmentChangedCallback(AttachmentChangedCallback a_callback);
std::vector<AttachmentRecord> GetPlayerAttachments();
std::vector<SceneBodyTriRecord> ScanPlayerBodyTriNodes();
std::vector<BipedPartRecord> ScanPlayerBipedParts();

// Applies all of the actor's existing morphs to the selected subtree while
// forcing one named morph to a_targetValue for this single call only.
// The temporary adapter morph key is restored immediately after the local pass.
bool ApplyScopedMorph(
    RE::Actor* a_actor,
    RE::NiAVObject* a_root,
    std::string_view a_morphName,
    float a_targetValue,
    std::string_view a_context);

}  // namespace vanity_ube_heel_adapter::racemenu
