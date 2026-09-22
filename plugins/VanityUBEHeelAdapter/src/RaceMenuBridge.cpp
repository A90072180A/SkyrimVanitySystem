#include "RaceMenuBridge.h"
#include "SkeeAPI.h"
#include "FootGeometryCapture.h"
#include <nlohmann/json.hpp>

namespace vanity_ube_heel_adapter::racemenu {
namespace {
constexpr auto kMorphKey = "VanityUBEHeelAdapter";

IBodyMorphInterface* g_bodyMorph{nullptr};
IActorUpdateManager* g_actorUpdates{nullptr};
std::mutex g_mutex;
std::vector<AttachmentRecord> g_attachments;
std::uint64_t g_sequence{0};
AttachmentChangedCallback g_attachmentChanged{nullptr};
std::atomic_bool g_insideApply{false};

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
    if(g_bodyMorph->GetVersion()>=5)g_bodyMorph->AddMorphShapeCallback(
        [](TESObjectREFR* ref, NiAVObject*, BSGeometry*, NiSkinPartition*, NiBinaryExtraData*) {
            if(g_insideApply.load()||ref!=reinterpret_cast<TESObjectREFR*>(RE::PlayerCharacter::GetSingleton()))return;
            AttachmentChangedCallback cb=nullptr;
            {std::scoped_lock lock(g_mutex);cb=g_attachmentChanged;}
            if(cb)cb(); // queue only: RaceMenu may invoke this off the game thread.
            foot_capture::RequestCapture();
        });
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

void ResetAttachmentCache(){std::scoped_lock lock(g_mutex);g_attachments.clear();}
bool IsLive(RE::NiAVObject* wanted)
{
    auto* player=RE::PlayerCharacter::GetSingleton();if(!player||!wanted)return false;
    auto contains=[&](RE::NiAVObject* root){bool found=false;
        RE::BSVisit::TraverseScenegraphObjects(root,[&](RE::NiAVObject* node){
            if(node==wanted){found=true;return RE::BSVisit::BSVisitControl::kStop;}
            return RE::BSVisit::BSVisitControl::kContinue;});return found;};
    if(contains(player->Get3D(false)))return true;
    const auto& biped=player->GetBiped(false);if(!biped)return false;
    for(unsigned i=0;i<static_cast<unsigned>(RE::BIPED_OBJECTS::kTotal);++i)
        if(biped->objects[i].partClone&&contains(biped->objects[i].partClone.get()))return true;
    return false;
}
std::string ActorMorphContext(RE::Actor* actor)
{
    using Json=nlohmann::json;
    if(!g_bodyMorph||!actor||!actor->GetActorBase()||!actor->GetRace())return {};
    struct Visitor final:IBodyMorphInterface::MorphValueVisitor {
        Json rows=Json::array();bool valid=true;
        void Visit(TESObjectREFR*,const char* name,const char* key,float value)override {
            if(!name||!key||!std::isfinite(value)){valid=false;return;}
            rows.push_back({{"name",name},{"key",key},{"value",value}});
        }
    } visitor;
    g_bodyMorph->VisitMorphValues(reinterpret_cast<TESObjectREFR*>(actor),visitor);
    if(!visitor.valid)return {};
    std::sort(visitor.rows.begin(),visitor.rows.end(),[](const Json&a,const Json&b){
        return std::make_pair(a.at("name").get<std::string>(),a.at("key").get<std::string>())<
            std::make_pair(b.at("name").get<std::string>(),b.at("key").get<std::string>());});
    auto* race=actor->GetRace();auto* file=race->GetFile(0);if(!file)return {};
    const auto raceID=std::format("{}|{:08X}",file->GetFilename(),race->GetLocalFormID());
    return Json::array({raceID,actor->GetActorBase()->GetSex()==RE::SEX::kFemale?1u:0u,
        actor->GetActorBase()->GetWeight(),visitor.rows}).dump();
}
bool ApplyScopedMorphs(RE::Actor* actor,RE::NiAVObject* root,
    std::span<const scoped_morph_transaction::Target> targets,std::string_view context)
{
    if(!g_bodyMorph||!actor||!root||!IsLive(root)||FindBodyTriPath(root).empty())return false;
    if(g_insideApply.exchange(true))return false;
    struct Leave {~Leave(){g_insideApply.store(false);}} leave;
    struct Backend {
        TESObjectREFR* ref;NiAVObject* root;
        bool Has(const std::string&n){return g_bodyMorph->HasBodyMorph(ref,n.c_str(),kMorphKey);}
        float Get(const std::string&n){return g_bodyMorph->GetMorph(ref,n.c_str(),kMorphKey);}
        float Effective(const std::string&n){return g_bodyMorph->GetBodyMorphs(ref,n.c_str());}
        void Set(const std::string&n,float v){g_bodyMorph->SetMorph(ref,n.c_str(),kMorphKey,v);}
        void Clear(const std::string&n){g_bodyMorph->ClearMorph(ref,n.c_str(),kMorphKey);}
        void Apply(){g_bodyMorph->ApplyVertexDiff(ref,root,false);}
    } backend{reinterpret_cast<TESObjectREFR*>(actor),reinterpret_cast<NiAVObject*>(root)};
    try {
        const bool ok=scoped_morph_transaction::Run(backend,targets);
        if(!ok)logger::warn("[height apply] refused nonfinite/duplicate target or incompatible aggregate: {}",context);
        return ok;
    }catch(const std::exception&e){logger::warn("[height apply] exception after restoring temporary keys: {}",e.what());return false;}
    catch(...){logger::warn("[height apply] exception after restoring temporary keys");return false;}
}
bool RestoreScopedMorphs(RE::Actor* actor,RE::NiAVObject* root)
{
    if(!g_bodyMorph||!actor||!root||!IsLive(root)||FindBodyTriPath(root).empty()||g_insideApply.exchange(true))return false;
    struct Leave {~Leave(){g_insideApply.store(false);}} leave;
    try {g_bodyMorph->ApplyVertexDiff(reinterpret_cast<TESObjectREFR*>(actor),reinterpret_cast<NiAVObject*>(root),false);return true;}
    catch(...){return false;}
}
bool ApplyScopedMorph(RE::Actor* actor, RE::NiAVObject* root,
    std::string_view name,float value,std::string_view context)
{
    const scoped_morph_transaction::Target target{std::string(name),value};
    return ApplyScopedMorphs(actor,root,std::span{&target,1},context);
}
} // namespace vanity_ube_heel_adapter::racemenu
