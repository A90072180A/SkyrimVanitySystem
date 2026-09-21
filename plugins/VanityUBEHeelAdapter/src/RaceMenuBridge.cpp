#include "RaceMenuBridge.h"
#include "SkeeAPI.h"
#include "FootGeometryCapture.h"

namespace vanity_ube_heel_adapter::racemenu {
namespace {
constexpr auto kMorphKey = "VanityUBEHeelAdapter";

IBodyMorphInterface* g_bodyMorph{nullptr};
IActorUpdateManager* g_actorUpdates{nullptr};
std::mutex g_mutex;
std::vector<AttachmentRecord> g_attachments;
std::uint64_t g_sequence{0};
AttachmentChangedCallback g_attachmentChanged{nullptr};

std::string FindBodyTriPath(RE::NiAVObject* a_root)
{
    if (!a_root) {
        return {};
    }
    std::string path;
    RE::BSVisit::TraverseScenegraphObjects(
        a_root,
        [&](RE::NiAVObject* a_object) {
            auto* extra = a_object->GetExtraData<RE::NiStringExtraData>("BODYTRI");
            if (extra && extra->value && *extra->value) {
                path = extra->value;
                return RE::BSVisit::BSVisitControl::kStop;
            }
            return RE::BSVisit::BSVisitControl::kContinue;
        });
    return path;
}

class AttachmentObserver final : public IAddonAttachmentInterface
{
public:
    void OnAttach(TESObjectREFR* a_refr, TESObjectARMO* a_armor,
        TESObjectARMA* a_addon, NiAVObject* a_object,
        bool a_isFirstPerson, NiNode*, NiNode*) override
    {
        if (a_isFirstPerson || !a_refr || !a_object) return;
        auto* refr = reinterpret_cast<RE::TESObjectREFR*>(a_refr);
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || refr != player) return;
        auto* armor = reinterpret_cast<RE::TESObjectARMO*>(a_armor);
        auto* addon = reinterpret_cast<RE::TESObjectARMA*>(a_addon);
        auto* object = reinterpret_cast<RE::NiAVObject*>(a_object);
        AttachmentRecord record;
        record.actorFormID = player->GetFormID();
        record.armorFormID = armor ? armor->GetFormID() : 0;
        record.armorAddonFormID = addon ? addon->GetFormID() : 0;
        record.object = RE::NiPointer<RE::NiAVObject>(object);
        record.bodyTriPath = FindBodyTriPath(object);
        AttachmentChangedCallback callback = nullptr;
        {
            std::scoped_lock lock(g_mutex);
            record.sequence = ++g_sequence;
            std::erase_if(g_attachments, [&](const AttachmentRecord& a_existing) {
                return a_existing.armorFormID == record.armorFormID &&
                       a_existing.armorAddonFormID == record.armorAddonFormID;
            });
            g_attachments.push_back(record);
            if (g_attachments.size() > 128) {
                g_attachments.erase(g_attachments.begin(), g_attachments.begin() +
                    static_cast<std::ptrdiff_t>(g_attachments.size() - 128));
            }
            callback = g_attachmentChanged;
        }
        logger::info("[RaceMenu attach] seq={} ARMO={:08X} ARMA={:08X} node='{}' BODYTRI='{}'",
            record.sequence, record.armorFormID, record.armorAddonFormID,
            object->name.c_str(), record.bodyTriPath);
        if (callback) callback();
        // Read the CURRENT active biped in a task, not this borrowed attach node.
        foot_capture::RequestCapture();
    }
};
AttachmentObserver g_observer;
} // namespace

bool Initialize()
{
    if (g_bodyMorph && g_actorUpdates) return true;
    auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging) return false;
    InterfaceExchangeMessage exchange{};
    const bool dispatched = messaging->Dispatch(
        InterfaceExchangeMessage::kMessage_ExchangeInterface,
        std::addressof(exchange), sizeof(exchange), "SKEE");
    if (!dispatched || !exchange.interfaceMap) {
        logger::warn("RaceMenu/skee interface exchange unavailable; scoped morphing disabled");
        return false;
    }
    g_bodyMorph = static_cast<IBodyMorphInterface*>(exchange.interfaceMap->QueryInterface("BodyMorph"));
    g_actorUpdates = static_cast<IActorUpdateManager*>(exchange.interfaceMap->QueryInterface("ActorUpdateManager"));
    if (!g_bodyMorph) { logger::warn("RaceMenu BodyMorph interface unavailable"); return false; }
    if (!g_actorUpdates) { logger::warn("RaceMenu ActorUpdateManager interface unavailable"); return false; }
    if (g_bodyMorph->GetVersion() < 4) {
        logger::warn("RaceMenu BodyMorph interface version {} is too old; require >= 4", g_bodyMorph->GetVersion());
        g_bodyMorph = nullptr;
        g_actorUpdates = nullptr;
        return false;
    }
    foot_capture::SetMorphInterface(g_bodyMorph);
    g_actorUpdates->AddInterface(std::addressof(g_observer));
    logger::info("RaceMenu integration ready: BodyMorph v{} ActorUpdateManager v{}",
        g_bodyMorph->GetVersion(), g_actorUpdates->GetVersion());
    return true;
}

bool Available() { return g_bodyMorph && g_actorUpdates; }
void SetAttachmentChangedCallback(const AttachmentChangedCallback a_callback)
{
    std::scoped_lock lock(g_mutex);
    g_attachmentChanged = a_callback;
}
std::vector<AttachmentRecord> GetPlayerAttachments()
{
    std::scoped_lock lock(g_mutex);
    return g_attachments;
}

std::vector<SceneBodyTriRecord> ScanPlayerBodyTriNodes()
{
    std::vector<SceneBodyTriRecord> records;
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* root = player ? player->Get3D(false) : nullptr;
    if (!root) return records;
    RE::BSVisit::TraverseScenegraphObjects(root, [&](RE::NiAVObject* a_object) {
        if (!a_object) return RE::BSVisit::BSVisitControl::kContinue;
        auto* extra = a_object->GetExtraData<RE::NiStringExtraData>("BODYTRI");
        if (extra && extra->value && *extra->value) {
            records.push_back(SceneBodyTriRecord{
                .object = RE::NiPointer<RE::NiAVObject>(a_object),
                .bodyTriPath = extra->value,
                .nodeName = a_object->name.c_str()});
        }
        return RE::BSVisit::BSVisitControl::kContinue;
    });
    return records;
}

std::vector<BipedPartRecord> ScanPlayerBipedParts()
{
    std::vector<BipedPartRecord> records;
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) return records;
    const auto& biped = player->GetBiped(false);
    if (!biped) return records;
    auto appendUniqueString = [](std::vector<std::string>& a_values, std::string a_value) {
        if (a_value.empty()) return;
        if (std::ranges::find(a_values, a_value) == a_values.end()) a_values.push_back(std::move(a_value));
    };
    auto collect = [&](const RE::BIPOBJECT& a_object, const std::uint32_t a_index, const bool a_buffered) {
        if (!a_object.item && !a_object.addon && !a_object.partClone) return;
        BipedPartRecord record;
        record.slotIndex = a_index;
        record.slotNumber = 30u + a_index;
        record.buffered = a_buffered;
        record.itemFormID = a_object.item ? a_object.item->GetFormID() : 0;
        record.addonFormID = a_object.addon ? a_object.addon->GetFormID() : 0;
        record.partClone = a_object.partClone;
        auto* root = a_object.partClone.get();
        if (root) {
            record.rootName = root->name.c_str();
            RE::BSVisit::TraverseScenegraphObjects(root, [&](RE::NiAVObject* a_node) {
                if (!a_node) return RE::BSVisit::BSVisitControl::kContinue;
                auto* extra = a_node->GetExtraData<RE::NiStringExtraData>("BODYTRI");
                if (extra && extra->value && *extra->value) appendUniqueString(record.bodyTriPaths, extra->value);
                return RE::BSVisit::BSVisitControl::kContinue;
            });
            RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_geometry) {
                if (!a_geometry) return RE::BSVisit::BSVisitControl::kContinue;
                GeometryDiagnosticRecord detail;
                detail.name = a_geometry->name.c_str();
                appendUniqueString(record.geometryNames, detail.name);
                if (const auto* rtti = a_geometry->GetRTTI()) detail.rttiName = rtti->GetName();
                if (auto* tri = a_geometry->AsTriShape()) {
                    const auto& triData = tri->GetTrishapeRuntimeData();
                    detail.vertexCount = triData.vertexCount;
                    detail.triangleCount = triData.triangleCount;
                }
                auto& runtime = a_geometry->GetGeometryRuntimeData();
                if (auto* skin = runtime.skinInstance.get()) {
                    detail.hasSkin = true;
                    if (auto* partition = skin->skinPartition.get()) {
                        detail.skinPartitionCount = partition->numPartitions;
                        detail.skinPartitionVertexCount = partition->vertexCount;
                    }
                }
                const auto& modelBound = a_geometry->GetModelData().modelBound;
                detail.modelBoundCenterX = modelBound.center.x;
                detail.modelBoundCenterY = modelBound.center.y;
                detail.modelBoundCenterZ = modelBound.center.z;
                detail.modelBoundRadius = modelBound.radius;
                detail.worldBoundCenterX = a_geometry->worldBound.center.x;
                detail.worldBoundCenterY = a_geometry->worldBound.center.y;
                detail.worldBoundCenterZ = a_geometry->worldBound.center.z;
                detail.worldBoundRadius = a_geometry->worldBound.radius;
                record.geometryDetails.push_back(std::move(detail));
                return RE::BSVisit::BSVisitControl::kContinue;
            });
        }
        records.push_back(std::move(record));
    };
    for (std::uint32_t index = 0; index < static_cast<std::uint32_t>(RE::BIPED_OBJECTS::kTotal); ++index) {
        collect(biped->objects[index], index, false);
        collect(biped->bufferedObjects[index], index, true);
    }
    return records;
}

bool ApplyScopedMorph(RE::Actor* a_actor, RE::NiAVObject* a_root,
    const std::string_view a_morphName, const float a_targetValue,
    const std::string_view a_context)
{
    if (!g_bodyMorph || !a_actor || !a_root || a_morphName.empty()) return false;
    const auto bodyTri = FindBodyTriPath(a_root);
    if (bodyTri.empty()) {
        logger::warn("[local morph] skipped: target '{}' has no BODYTRI", a_context);
        return false;
    }
    const std::string morphName(a_morphName);
    auto* refr = reinterpret_cast<TESObjectREFR*>(a_actor);
    const bool hadOwnKey = g_bodyMorph->HasBodyMorph(refr, morphName.c_str(), kMorphKey);
    const float previousOwn = g_bodyMorph->GetMorph(refr, morphName.c_str(), kMorphKey);
    const float totalBefore = g_bodyMorph->GetBodyMorphs(refr, morphName.c_str());
    const float otherKeys = totalBefore - previousOwn;
    const float temporaryOwn = a_targetValue - otherKeys;
    g_bodyMorph->SetMorph(refr, morphName.c_str(), kMorphKey, temporaryOwn);
    g_bodyMorph->ApplyVertexDiff(refr, reinterpret_cast<NiAVObject*>(a_root), false);
    if (hadOwnKey) g_bodyMorph->SetMorph(refr, morphName.c_str(), kMorphKey, previousOwn);
    else g_bodyMorph->ClearMorph(refr, morphName.c_str(), kMorphKey);
    logger::info("[local morph] applied context='{}' BODYTRI='{}' morph='{}' targetValue={:.3f} totalBefore={:.3f} otherKeys={:.3f} temporaryAdapterKey={:.3f}",
        a_context, bodyTri, morphName, a_targetValue, totalBefore, otherKeys, temporaryOwn);
    return true;
}
} // namespace vanity_ube_heel_adapter::racemenu
