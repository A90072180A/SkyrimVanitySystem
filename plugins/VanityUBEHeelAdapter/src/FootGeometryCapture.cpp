#include "ConfigState.h"
#include "FootGeometryCapture.h"
#include "HeightProfiles.h"
#include "FootSnapshotCore.h"
#include "FootBasisIO.h"
#include "FootCapturePolicy.h"
#include "SkeeAPI.h"
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <d3d11.h>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <thread>
#include <span>
#include <cstring>

namespace vanity_ube_heel_adapter::foot_capture {
namespace {
using Json = nlohmann::json;
namespace core = foot_snapshot_core;
namespace policy = foot_capture_policy;
constexpr auto kDirectory = "Data/SKSE/Plugins/VanityUBEHeelAdapter/geometry";
constexpr auto kVersion = "0.16.0";
std::atomic_bool g_sessionReady{false};
std::atomic<std::uint64_t> g_ticket{0};
IBodyMorphInterface* g_morph{nullptr};
std::mutex g_captureMutex;

bool CopyBytes(void* dst, const void* src, std::size_t bytes) noexcept
{
    if (!dst || !src || !bytes) return false;
    __try { std::memcpy(dst, src, bytes); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
struct View {
    std::uint32_t partitions{}, vertices{}, partitionVertices{}, triangles{};
    std::uint32_t stride{}, componentBytes{}, positionSpan{}, bufferBytes{}, boneCount{};
    std::uint32_t shapeVertices{}, shapeTriangles{}, fault{};
    std::uint64_t descriptor{};
    RE::NiTransform bindTransform{};
    bool bindAvailable{false};
    const std::uint8_t* vertexData{};
    const std::uint16_t* indices{};
    const std::uint16_t* vertexMap{};
    const char* status{"probe-not-run"};
};
// Same conservative decoder gate as 0.10: no new layout or pointer assumptions.
void Probe(RE::BSGeometry* geometry, View* out) noexcept
{
    __try {
        if (auto* tri = geometry->AsTriShape()) {
            const auto& rt = tri->GetTrishapeRuntimeData();
            out->shapeVertices = rt.vertexCount; out->shapeTriangles = rt.triangleCount;
        }
        auto* skin = geometry->GetGeometryRuntimeData().skinInstance.get();
        if (!skin || !skin->skinPartition) { out->status = "no-skin-partition"; return; }
        if (auto* data = skin->skinData.get()) {
            std::memcpy(&out->bindTransform, &data->rootParentToSkin, sizeof(out->bindTransform));
            out->bindAvailable = true;
        }
        auto* master = skin->skinPartition.get();
        out->partitions = master->numPartitions; out->vertices = master->vertexCount;
        if (out->partitions != 1) { out->status = "unsupported-multiple-partitions"; return; }
        auto& part = master->partitions[0];
        out->partitionVertices = part.vertices; out->triangles = part.triangles; out->boneCount = part.numBones;
        if (!out->vertices || out->vertices > 65535 || part.vertices != out->vertices) {
            out->status = "unsupported-partition-index-domain"; return;
        }
        if (part.strips || !part.triangles || !part.triList) { out->status = "no-triangle-list"; return; }
        auto* buffer = part.buffData;
        if (!buffer || !buffer->rawVertexData) { out->status = "cpu-vertex-data-unavailable"; return; }
        std::memcpy(&out->descriptor, &buffer->vertexDesc, sizeof(out->descriptor));
        out->stride = static_cast<std::uint32_t>(out->descriptor & 15ULL) * 4u;
        if (!out->stride || out->stride > 256) { out->status = "invalid-encoded-stride"; return; }
        if (!buffer->vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX)) {
            out->status = "unsupported-dynamic-position-stream"; return;
        }
        out->positionSpan = out->stride;
        for (unsigned i = 1; i < 10; ++i) {
            if (((out->descriptor >> 44) & (1ULL << i)) == 0) continue;
            const auto offset = static_cast<std::uint32_t>((out->descriptor >> (4 * i + 2)) & 0x3CULL);
            if (!offset || offset >= out->stride) { out->status = "invalid-attribute-offset"; return; }
            if (offset < out->positionSpan) out->positionSpan = offset;
        }
        if (out->positionSpan >= 16) out->componentBytes = 4;
        else if (out->positionSpan == 8 && !buffer->vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC)) out->componentBytes = 2;
        else { out->status = "unsupported-position-layout"; return; }
        if (!buffer->vertexBuffer) { out->status = "buffer-capacity-unavailable"; return; }
        D3D11_BUFFER_DESC desc{};
        reinterpret_cast<::ID3D11Buffer*>(buffer->vertexBuffer)->GetDesc(&desc);
        out->bufferBytes = desc.ByteWidth;
        if (static_cast<std::uint64_t>(out->vertices) * out->stride != out->bufferBytes) {
            out->status = "buffer-capacity-mismatch"; return;
        }
        out->vertexData = buffer->rawVertexData; out->indices = part.triList; out->vertexMap = part.vertexMap;
        out->status = "readable";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->fault = GetExceptionCode(); out->status = "metadata-access-fault";
    }
}
std::string SafeName(RE::NiAVObject* object)
{
    const char* name = object ? object->name.c_str() : nullptr;
    return name ? name : "";
}
std::string TypeName(RE::NiAVObject* object)
{
    const auto* rtti = object ? object->GetRTTI() : nullptr;
    return rtti && rtti->GetName() ? rtti->GetName() : "unknown";
}
std::string Stable(RE::TESForm* form)
{
    if (!form || !form->GetFile(0)) return {};
    return std::format("{}|{:08X}", form->GetFile(0)->GetFilename(), form->GetLocalFormID());
}
Json Transform(const RE::NiTransform& t)
{
    return {{"rotation", {{t.rotate.entry[0][0], t.rotate.entry[0][1], t.rotate.entry[0][2]},
                           {t.rotate.entry[1][0], t.rotate.entry[1][1], t.rotate.entry[1][2]},
                           {t.rotate.entry[2][0], t.rotate.entry[2][1], t.rotate.entry[2][2]}}},
            {"translation", {t.translate.x, t.translate.y, t.translate.z}}, {"scale", t.scale}};
}
Json MorphValues(RE::Actor* actor)
{
    struct Visitor final : IBodyMorphInterface::MorphValueVisitor {
        Json values = Json::array();
        void Visit(TESObjectREFR*, const char* name, const char* key, float value) override {
            values.push_back({{"name", name ? name : ""}, {"key", key ? key : ""}, {"value", value}});
        }
    } visitor;
    if (!g_morph) return nullptr;
    g_morph->VisitMorphValues(reinterpret_cast<TESObjectREFR*>(actor), visitor);
    std::sort(visitor.values.begin(), visitor.values.end(), [](const Json& a, const Json& b) {
        return std::make_pair(a["name"].get<std::string>(), a["key"].get<std::string>()) <
               std::make_pair(b["name"].get<std::string>(), b["key"].get<std::string>());
    });
    return visitor.values;
}
struct Options {
    bool enabled{false}, exportStockings{false}, writeSnapshot{false};
    std::string reference;
    std::vector<std::string> morphs, stockings;
};
Options ReadOptions()
{
    try {
        const auto config = config_state::Get();
        Options out;
        out.writeSnapshot = config.value("exportFootGeometry", false);
        out.enabled = out.writeSnapshot || config.value("automaticHeight", true);
        out.exportStockings = config.value("exportStockingCalibration", false) || config.value("automaticHeight", true);
        out.reference = config.value("referenceBodyTri", std::string{});
        const auto requested = config.value("diagnosticFootMorphs", Json::array({"NoHeel", "HiHeelz_CBBE", "HiHeelz_CBBE_to_UBE"}));
        if (requested.is_array()) for (const auto& entry : requested) {
            if (!entry.is_string() || out.morphs.size() >= 4) continue;
            const auto value = entry.get<std::string>();
            if (!value.empty() && value.size() <= 255 && std::find(out.morphs.begin(), out.morphs.end(), value) == out.morphs.end())
                out.morphs.push_back(value);
        }
        if (config.contains("stockings") && config["stockings"].is_array())
            for (const auto& entry : config["stockings"]) if (entry.is_string()) out.stockings.push_back(entry.get<std::string>());
        const auto kinds=config.value("manualItemKinds",Json::object());
        for(auto it=kinds.begin();it!=kinds.end();++it) {
            if(it.value()=="stocking" && std::find(out.stockings.begin(),out.stockings.end(),it.key())==out.stockings.end())out.stockings.push_back(it.key());
            if(it.value()=="ignore" || it.value()=="footwear")std::erase(out.stockings,it.key());
        }
        return out;
    } catch (...) { return {}; }
}
std::vector<std::string> BodyTris(RE::NiAVObject* root)
{
    std::vector<std::string> paths;
    RE::BSVisit::TraverseScenegraphObjects(root, [&](RE::NiAVObject* node) {
        auto* extra = node->GetExtraData<RE::NiStringExtraData>("BODYTRI");
        if (extra && extra->value && *extra->value && std::find(paths.begin(), paths.end(), extra->value) == paths.end())
            paths.emplace_back(extra->value);
        return RE::BSVisit::BSVisitControl::kContinue;
    });
    return paths;
}
std::vector<std::string> InheritedBodyTris(RE::NiAVObject* object)
{
    // First ancestor carrying BODYTRI wins. Bound the walk even for a bad graph.
    for (unsigned depth = 0; object && depth < 64; ++depth, object = object->parent) {
        auto* extra = object->GetExtraData<RE::NiStringExtraData>("BODYTRI");
        if (extra && extra->value && *extra->value) return {extra->value};
    }
    return {};
}
bool ContainsObject(RE::NiAVObject* root, RE::NiAVObject* wanted)
{
    if (!root || !wanted) return false;
    bool found = false;
    RE::BSVisit::TraverseScenegraphObjects(root, [&](RE::NiAVObject* node) {
        if (node == wanted) { found = true; return RE::BSVisit::BSVisitControl::kStop; }
        return RE::BSVisit::BSVisitControl::kContinue;
    });
    return found;
}
struct Observation {
    RE::NiPointer<RE::NiAVObject> root;
    RE::FormID armor{}, addon{};
    std::uint64_t sequence{};
};
std::mutex g_observationMutex;
Observation g_observation;
std::uint64_t g_observationSequence{};
bool g_observerRegistered{false};
class FootAttachmentObserver final : public IAddonAttachmentInterface {
public:
    void OnAttach(TESObjectREFR* refr, TESObjectARMO* armor, TESObjectARMA* addon,
                  NiAVObject* object, bool firstPerson, NiNode*, NiNode*) override {
        if (firstPerson || !refr || !object || !armor || !addon) return;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || reinterpret_cast<RE::TESObjectREFR*>(refr) != player) return;
        auto* actualAddon = reinterpret_cast<RE::TESObjectARMA*>(addon);
        if (!actualAddon->HasPartOf(RE::BGSBipedObjectForm::BipedObjectSlot::kFeet)) return;
        {
            std::scoped_lock lock(g_observationMutex);
            g_observation = {RE::NiPointer<RE::NiAVObject>(reinterpret_cast<RE::NiAVObject*>(object)),
                reinterpret_cast<RE::TESObjectARMO*>(armor)->GetFormID(), actualAddon->GetFormID(), ++g_observationSequence};
        }
        RequestCapture();
    }
};
FootAttachmentObserver g_footObserver;
struct WriteJob { std::string key; Json document; };
class Writer {
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<WriteJob> jobs;
    std::unordered_set<std::string> pending, done;
    std::uint64_t lastSession{};
    std::jthread worker;
    void Write(WriteJob& job) {
        if(job.document.value("heightSession",std::uint64_t{})!=height_profiles::Session())return;
        foot_basis_io::Enrich(job.document);
        if(!job.document.value("writeGeometrySnapshot",true)||job.document.value("heightSession",std::uint64_t{})!=height_profiles::Session())return;
        const auto path = std::filesystem::path(kDirectory) / ("foot-" + job.key + ".json");
        const auto temp = std::filesystem::path(path.wstring() + L".tmp");
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) throw std::runtime_error(ec.message());
        {
            std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
            if (!stream) throw std::runtime_error("cannot create snapshot temp file");
            stream << job.document.dump(2) << '\n'; stream.flush();
            if (!stream) throw std::runtime_error("cannot flush snapshot temp file");
            stream.close(); if (stream.fail()) throw std::runtime_error("cannot close snapshot temp file");
        }
        if (!::MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error(std::format("atomic replace failed: {}", ::GetLastError()));
        logger::info("[foot snapshot] wrote '{}' status={} role={} (diagnostic-only)", path.string(),
            job.document.at("extraction").at("status").get<std::string>(), job.document.at("geometryRole").get<std::string>());
    }
    void Run(std::stop_token stop) {
        for (;;) {
            WriteJob job;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [&] { return stop.stop_requested() || !jobs.empty(); });
                if (jobs.empty()) { if (stop.stop_requested()) return; else continue; }
                job = std::move(jobs.front()); jobs.pop_front();
            }
            bool success = false;
            try { Write(job); success = true; }
            catch (const std::exception& e) { logger::warn("[foot snapshot] write failed: {}", e.what()); }
            catch (...) { logger::warn("[foot snapshot] write failed"); }
            {
                std::scoped_lock lock(mutex); const auto requestKey=std::to_string(job.document.value("heightSession",std::uint64_t{}))+"|"+job.key; pending.erase(requestKey);
                if (success) done.insert(requestKey);
            }
        }
    }
public:
    Writer() : worker([this](std::stop_token stop) { Run(stop); }) {}
    ~Writer() { worker.request_stop(); condition.notify_all(); worker.join(); }
    void Submit(WriteJob job) {
        std::scoped_lock lock(mutex);
        const auto epoch=job.document.value("heightSession",std::uint64_t{});
        if(epoch!=lastSession){jobs.clear();pending.clear();done.clear();lastSession=epoch;}
        const auto requestKey=std::to_string(epoch)+"|"+job.key;
        if (pending.contains(requestKey) || done.contains(requestKey)) return;
        if (jobs.size() >= 8) { logger::warn("[foot snapshot] writer queue full; retry later"); return; }
        pending.insert(requestKey); jobs.push_back(std::move(job)); condition.notify_one();
    }
};
Writer& GetWriter() { static Writer writer; return writer; }
struct Candidate {
    RE::NiPointer<RE::NiAVObject> root;
    RE::FormID armor{}, addon{};
    std::string selection, role{"foot"};
    std::uint32_t slot{37};
};
void Emit(RE::Actor* player, const Candidate& candidate, RE::BSGeometry* geometry,
          const Options& options, const Json& morphs, unsigned attempt)
{
    auto* base = player->GetActorBase();
    auto* armorForm = RE::TESForm::LookupByID(candidate.armor);
    auto* addonForm = RE::TESForm::LookupByID<RE::TESObjectARMA>(candidate.addon);
    if (!base || !armorForm || !addonForm) return;
    const auto armor = Stable(armorForm), addon = Stable(addonForm);
    const unsigned sexIndex = base->GetSex() == RE::SEX::kFemale ? 1u : 0u;
    const char* model = addonForm->bipedModels[sexIndex].GetModel();
    auto triPaths = InheritedBodyTris(geometry);
    if (triPaths.empty()) triPaths = BodyTris(candidate.root.get());
    View view{}; Probe(geometry, &view);
    core::Result decoded; decoded.status = view.status;
    std::string mapKind = view.vertexMap ? "not-validated" : "absent-single-full-partition";
    if (decoded.status == "readable") {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(view.vertices) * view.stride);
        std::vector<std::uint16_t> indices(static_cast<std::size_t>(view.triangles) * 3);
        bool mappingOK = true;
        if (view.vertexMap) {
            std::vector<std::uint16_t> map(view.vertices);
            mappingOK = CopyBytes(map.data(), view.vertexMap, map.size() * sizeof(std::uint16_t));
            if (mappingOK) for (std::size_t i = 0; i < map.size(); ++i) if (map[i] != i) { mappingOK = false; break; }
            mapKind = mappingOK ? "identity" : "unsupported-nonidentity-or-unreadable";
        }
        if (!mappingOK) decoded.status = "unsupported-vertex-map";
        else if (!CopyBytes(bytes.data(), view.vertexData, bytes.size()) ||
                 !CopyBytes(indices.data(), view.indices, indices.size() * sizeof(std::uint16_t))) decoded.status = "cpu-buffer-copy-fault";
        else decoded = core::Decode(bytes, indices, view.vertices, view.stride, view.componentBytes);
    }
    const Json bind = view.bindAvailable ? Transform(view.bindTransform) : Json(nullptr);
    const auto morphHash = std::format("{:016x}", core::HashText(morphs.dump()));
    Json extraction = {{"status", decoded.status}, {"shapeReportedVertexCount", view.shapeVertices},
        {"shapeReportedTriangleCount", view.shapeTriangles}, {"skinPartitionCount", view.partitions},
        {"skinPartitionVertexCount", view.vertices}, {"partitionLocalVertexCount", view.partitionVertices},
        {"partitionTriangleCount", view.triangles}, {"partitionBoneCount", view.boneCount},
        {"vertexDescriptor", std::format("{:016x}", view.descriptor)}, {"strideBytes", view.stride},
        {"positionSpanBytes", view.positionSpan}, {"componentBytes", view.componentBytes},
        {"gpuAllocationBytes", view.bufferBytes}, {"vertexMap", mapKind}, {"accessFault", view.fault},
        {"positionSource", "skin-partition.buffData.rawVertexData"}, {"indexSource", "skin-partition.triList"}};
    Json document = {{"schema", 3}, {"generatorVersion", kVersion}, {"status", "diagnostic-only"}, {"heightSession", height_profiles::Session()}, {"writeGeometrySnapshot",options.writeSnapshot},
        {"source", "current-graph-calibration-snapshot"}, {"geometryRole", candidate.role},
        {"identity", {{"armor", armor}, {"addon", addon}, {"armaModel", model ? model : ""},
            {"bodyTriPaths", triPaths}, {"slot", candidate.slot}, {"buffered", false}, {"sexIndex", sexIndex},
            {"actorWeight", base->GetWeight()}, {"race", Stable(player->GetRace())}}},
        {"captureSelection", {{"method", candidate.selection}, {"currentGraphConfirmed", true},
            {"retryAttempt", attempt}, {"identityIsCorrelated", candidate.selection == "unique-live-bodytri"}}},
        {"requestedReferenceBodyTri", options.reference}, {"requestedDiagnosticMorphs", options.morphs},
        {"geometry", SafeName(geometry)}, {"rtti", TypeName(geometry)},
        {"coordinateSpace", "CPU vertex stream before bone transforms; NOT certified neutral"},
        {"localMorphState", "not-certified; actor morph keys do not encode prior scoped local passes"},
        {"localTransform", Transform(geometry->local)}, {"rootParentToSkin", bind},
        {"actorMorphValues", morphs}, {"actorMorphStateHash", morphHash}, {"extraction", std::move(extraction)},
        {"geometryFit", {{"status", "not-run"}, {"posture", nullptr}, {"fitResidual", nullptr}}},
        {"hashAlgorithm", "FNV1a64 diagnostic fingerprint, ordered little-endian u32; not a security hash"},
        {"topologyFingerprint", nullptr}, {"positionFingerprint", nullptr},
        {"positions", Json::array()}, {"triangles", Json::array()}};
    if (decoded.Complete()) {
        document["topologyFingerprint"] = std::format("{:016x}", decoded.topologyHash);
        document["positionFingerprint"] = std::format("{:016x}", decoded.positionHash);
        document["positions"] = decoded.positions; document["triangles"] = decoded.triangles;
    }
    const auto keySource = std::string(kVersion) + document["identity"].dump() + "|" + morphHash + "|" +
        std::to_string(decoded.topologyHash) + "|" + std::to_string(decoded.positionHash) + "|" + decoded.status + "|" +
        bind.dump() + "|" + document["localTransform"].dump() + "|" + options.reference + "|" +
        document["requestedDiagnosticMorphs"].dump() + "|" + SafeName(geometry) + "|" + candidate.role + "|" + candidate.selection;
    const auto key = std::format("{:016x}", core::HashText(keySource)); document["captureKey"] = key;
    logger::info("[foot snapshot] armor='{}' geometry='{}' role={} status={} decodedVertices={} decodedTriangles={} selection={} attempt={}",
        armor, SafeName(geometry), candidate.role, decoded.status, decoded.positions.size(), decoded.triangles.size(), candidate.selection, attempt);
    GetWriter().Submit({key, std::move(document)});
}
void Capture(unsigned attempt)
{
    std::unique_lock captureLock(g_captureMutex, std::try_to_lock);
    if (!captureLock.owns_lock()) return;
    const auto options = ReadOptions();
    if (!options.enabled || REL::Module::get().version() != REL::Version{1, 6, 1170, 0}) return;
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player || !player->GetActorBase()) return;
    const auto& biped = player->GetBiped(false);
    auto* scene = player->Get3D(false);
    if (!biped || !scene) return;
    const auto& active = biped->objects[37 - 30];
    Observation observed;
    { std::scoped_lock lock(g_observationMutex); observed = g_observation; }
    std::vector<RE::NiPointer<RE::NiAVObject>> liveRoots;
    liveRoots.emplace_back(scene);
    for (unsigned i = 0; i < static_cast<unsigned>(RE::BIPED_OBJECTS::kTotal); ++i)
        if (biped->objects[i].partClone) liveRoots.push_back(biped->objects[i].partClone);
    auto live = [&](RE::NiAVObject* object) {
        return std::any_of(liveRoots.begin(), liveRoots.end(), [&](const auto& root) { return ContainsObject(root.get(), object); });
    };
    std::vector<Candidate> candidates;
    if (observed.root) candidates.push_back({observed.root, observed.armor, observed.addon, "live-attachment", "foot", 37});
    if (active.partClone && active.item && active.addon)
        candidates.push_back({active.partClone, active.item->GetFormID(), active.addon->GetFormID(), "active-biped", "foot", 37});
    const auto morphs = MorphValues(player);
    std::unordered_set<RE::BSGeometry*> captured;
    std::size_t footCount = 0, emitted = 0;
    auto process = [&](const Candidate& candidate) {
        RE::BSVisit::TraverseScenegraphGeometries(candidate.root.get(), [&](RE::BSGeometry* geometry) {
            if (!geometry || captured.contains(geometry) || emitted >= 8) return RE::BSVisit::BSVisitControl::kContinue;
            if (candidate.role == "foot" && SafeName(geometry) != "Feet") return RE::BSVisit::BSVisitControl::kContinue;
            if (!live(geometry)) return RE::BSVisit::BSVisitControl::kContinue;
            captured.insert(geometry); ++emitted;
            if (candidate.role == "foot") ++footCount;
            Emit(player, candidate, geometry, options, morphs, attempt);
            return RE::BSVisit::BSVisitControl::kContinue;
        });
    };
    for (const auto& candidate : candidates) process(candidate);
    if (!footCount && observed.root) {
        std::vector<policy::Evidence> evidence;
        std::unordered_map<std::uintptr_t, RE::BSGeometry*> geometryByID;
        std::unordered_set<RE::NiAVObject*> visited;
        for (const auto& root : liveRoots) RE::BSVisit::TraverseScenegraphObjects(root.get(), [&](RE::NiAVObject* node) {
            if (SafeName(node) != "Feet" || !visited.insert(node).second) return RE::BSVisit::BSVisitControl::kContinue;
            auto* geometry = node->AsGeometry();
            const auto paths = InheritedBodyTris(node);
            logger::info("[foot selection] live node='{}' rtti='{}' BSGeometry={} BODYTRIs={} attempt={}",
                SafeName(node), TypeName(node), geometry != nullptr, paths.size(), attempt);
            const auto id = reinterpret_cast<std::uintptr_t>(node);
            evidence.push_back({id, true, geometry != nullptr, paths});
            if (geometry) geometryByID.emplace(id, geometry);
            return RE::BSVisit::BSVisitControl::kContinue;
        });
        const auto expected = BodyTris(observed.root.get());
        const auto selected = policy::UniqueResourceMatch(evidence, expected);
        logger::info("[foot selection] observedRoot='{}' rtti='{}' liveFeetNodes={} expectedBODYTRIs={} uniqueResourceMatch={} attempt={}",
            SafeName(observed.root.get()), TypeName(observed.root.get()), evidence.size(), expected.size(), selected.has_value(), attempt);
        if (selected) process({RE::NiPointer<RE::NiAVObject>(geometryByID.at(*selected)), observed.armor, observed.addon,
                               "unique-live-bodytri", "foot", 37});
    }
    if (!footCount) logger::info("[foot snapshot] selection=none activeClone={} activeItem={} activeAddon={} observedSequence={} attempt={}; no detached or name-only node exported",
        static_cast<bool>(active.partClone), active.item != nullptr, active.addon != nullptr, observed.sequence, attempt);
    // Calibration donor: explicitly configured stockings only, in ACTIVE biped
    // trees. Never equate its NoHeel to a foot's similarly named morph.
    if (options.exportStockings) {
        std::unordered_set<RE::NiAVObject*> visited;
        for (unsigned i = 0; i < static_cast<unsigned>(RE::BIPED_OBJECTS::kTotal) && emitted < 8; ++i) {
            const auto& part = biped->objects[i];
            if (!part.item || !part.addon || !part.partClone || !visited.insert(part.partClone.get()).second) continue;
            const auto id = Stable(part.item);
            if (std::find(options.stockings.begin(), options.stockings.end(), id) == options.stockings.end()) continue;
            process({part.partClone, part.item->GetFormID(), part.addon->GetFormID(), "active-configured-stocking", "stocking", 30u + i});
        }
    }
}
void QueueAttempt(std::uint64_t ticket, unsigned attempt)
{
    if (!g_sessionReady.load() || !policy::CurrentTicket(ticket, g_ticket.load())) return;
    if (auto* tasks = SKSE::GetTaskInterface()) tasks->AddTask([ticket, attempt] {
        if (!g_sessionReady.load() || !policy::CurrentTicket(ticket, g_ticket.load())) return;
        try { Capture(attempt); }
        catch (const std::exception& e) { logger::warn("[foot snapshot] capture failed: {}", e.what()); }
        catch (...) { logger::warn("[foot snapshot] capture failed"); }
    });
}
// This thread owns only time/tickets; every game-object read stays in SKSE tasks.
class RetryTimer {
    std::mutex mutex;
    std::condition_variable condition;
    std::uint64_t current{};
    std::chrono::steady_clock::time_point start;
    std::jthread worker;
    void Run(std::stop_token stop) {
        std::unique_lock lock(mutex);
        while (!stop.stop_requested()) {
            condition.wait(lock, [&] { return stop.stop_requested() || current != 0; });
            if (stop.stop_requested()) break;
            const auto ticket = current;
            const auto origin = start;
            for (unsigned i = 0; i < policy::RetryMilliseconds.size(); ++i) {
                const auto interrupted = condition.wait_until(lock, origin + std::chrono::milliseconds(policy::RetryMilliseconds[i]),
                    [&] { return stop.stop_requested() || current != ticket; });
                if (interrupted) break;
                lock.unlock(); QueueAttempt(ticket, i + 1); lock.lock();
            }
            if (current == ticket) current = 0;
        }
    }
public:
    RetryTimer() : worker([this](std::stop_token stop) { Run(stop); }) {}
    ~RetryTimer() { worker.request_stop(); condition.notify_all(); worker.join(); }
    void Arm(std::uint64_t ticket) {
        std::scoped_lock lock(mutex);
        if (!g_sessionReady.load() || !policy::CurrentTicket(ticket, g_ticket.load())) return;
        current = ticket; start = std::chrono::steady_clock::now(); condition.notify_all();
    }
};
RetryTimer& GetRetryTimer() { static RetryTimer timer; return timer; }
} // namespace
void SetMorphInterface(IBodyMorphInterface* value)
{
    g_morph = value;
    if (g_observerRegistered) return;
    auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging) return;
    InterfaceExchangeMessage exchange{};
    if (!messaging->Dispatch(InterfaceExchangeMessage::kMessage_ExchangeInterface, &exchange, sizeof(exchange), "SKEE") || !exchange.interfaceMap) return;
    auto* updates = static_cast<IActorUpdateManager*>(exchange.interfaceMap->QueryInterface("ActorUpdateManager"));
    if (!updates) return;
    updates->AddInterface(&g_footObserver); g_observerRegistered = true;
    logger::info("[foot snapshot] registered live slot37 observer; timed retries=150/500/1500ms; no morph aliases");
}
void SetSessionReady(bool ready){g_sessionReady.store(ready);if(!ready)g_ticket.fetch_add(1);}
void RequestCapture()
{
    if(!g_sessionReady.load())return;
    const auto ticket = g_ticket.fetch_add(1) + 1;
    QueueAttempt(ticket, 0);
    GetRetryTimer().Arm(ticket);
}
} // namespace vanity_ube_heel_adapter::foot_capture
