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

using AttachmentChangedCallback = void (*)();

bool Initialize();
bool Available();
void SetAttachmentChangedCallback(AttachmentChangedCallback a_callback);
std::vector<AttachmentRecord> GetPlayerAttachments();

// Applies all of the actor's existing morphs to the selected subtree while
// forcing the total NoHeel value to a_targetNoHeel for this single call only.
// The temporary adapter morph key is restored immediately after the local pass.
bool ApplyScopedNoHeel(
    RE::Actor* a_actor,
    RE::NiAVObject* a_root,
    float a_targetNoHeel,
    std::string_view a_context);

}  // namespace vanity_ube_heel_adapter::racemenu
