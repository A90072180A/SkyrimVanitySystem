#pragma once

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
};

using AttachmentChangedCallback = void (*)();

bool Initialize();
bool Available();
void SetAttachmentChangedCallback(AttachmentChangedCallback a_callback);
std::vector<AttachmentRecord> GetPlayerAttachments();
std::vector<SceneBodyTriRecord> ScanPlayerBodyTriNodes();
std::vector<BipedPartRecord> ScanPlayerBipedParts();

// Applies all of the actor's existing morphs to the selected subtree while
// forcing the total NoHeel value to a_targetNoHeel for this single call only.
// The temporary adapter morph key is restored immediately after the local pass.
bool ApplyScopedNoHeel(
    RE::Actor* a_actor,
    RE::NiAVObject* a_root,
    float a_targetNoHeel,
    std::string_view a_context);

}  // namespace vanity_ube_heel_adapter::racemenu
