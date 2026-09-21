#include "FootGeometryCapture.h"
#include "FootSnapshotCore.h"
#include "SkeeAPI.h"
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <d3d11.h>
#include <condition_variable>
#include <deque>
#include <thread>
#include <span>
#include <cstring>

namespace vanity_ube_heel_adapter::foot_capture {
namespace {
using Json = nlohmann::json;
namespace core = foot_snapshot_core;
constexpr auto kDirectory = "Data/SKSE/Plugins/VanityUBEHeelAdapter/geometry";
std::atomic_bool g_queued{false};
IBodyMorphInterface* g_morph{nullptr};
std::mutex g_captureMutex;

// A leaf SEH frame never contains objects requiring C++ unwinding. Validation of
// lengths happens BEFORE this copy. SEH is a last guard, not a length check.
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
// Deliberately supports only one full, non-strip partition. No GPU readback,
// no reinterpretation of legacy NiGeometry, no guessed vertex-map semantics.
void Probe(RE::BSGeometry* geometry, View* out) noexcept
{
    __try {
        if (auto* tri = geometry->AsTriShape()) {
            const auto& rt = tri->GetTrishapeRuntimeData();
            out->shapeVertices = rt.vertexCount;
            out->shapeTriangles = rt.triangleCount;
        }
        auto* skin = geometry->GetGeometryRuntimeData().skinInstance.get();
        if (!skin || !skin->skinPartition) { out->status = "no-skin-partition"; return; }
        if (auto* data = skin->skinData.get()) {
            std::memcpy(&out->bindTransform, &data->rootParentToSkin, sizeof(out->bindTransform));
            out->bindAvailable = true;
        }
        auto* master = skin->skinPartition.get();
        out->partitions = master->numPartitions;
        out->vertices = master->vertexCount;
        if (out->partitions != 1) { out->status = "unsupported-multiple-partitions"; return; }
        auto& part = master->partitions[0];
        out->partitionVertices = part.vertices;
        out->triangles = part.triangles;
        out->boneCount = part.numBones;
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
        // Position is at byte zero for this packed stream. VA_POSITION's nibble
        // stores stride, so GetAttributeOffset(VA_POSITION) is NOT an offset.
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
        out->vertexData = buffer->rawVertexData;
        out->indices = part.triList;
        out->vertexMap = part.vertexMap;
        out->status = "readable";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->fault = GetExceptionCode(); out->status = "metadata-access-fault";
    }
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
bool Enabled()
{
    // Small config read only; no BodySlide walk, NIF load, or TRI parse here.
    try {
        std::ifstream stream("Data/SKSE/Plugins/VanityUBEHeelAdapter.json");
        if (!stream) return false;
        const auto config = Json::parse(stream);
        return config.value("exportFootGeometry", false);
    } catch (...) { return false; }
}
struct WriteJob { std::string key; Json document; };
class Writer {
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<WriteJob> jobs;
    std::unordered_set<std::string> pending, done;
    std::jthread worker;
    bool Write(const WriteJob& job) {
        const auto path = std::filesystem::path(kDirectory) / ("foot-" + job.key + ".json");
        const auto temp = std::filesystem::path(path.wstring() + L".tmp");
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) throw std::runtime_error(ec.message());
        {
            std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
            if (!stream) throw std::runtime_error("cannot create snapshot temp file");
            stream << job.document.dump(2) << '\n';
            stream.flush();
            if (!stream) throw std::runtime_error("cannot flush snapshot temp file");
            stream.close();
            if (stream.fail()) throw std::runtime_error("cannot close snapshot temp file");
        }
        // Never delete the existing final file before replacement.
        if (!::MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error(std::format("atomic replace failed: {}", ::GetLastError()));
        logger::info("[foot snapshot] wrote '{}' status={} (diagnostic-only)", path.string(), job.document.at("extraction").at("status").get<std::string>());
        return true;
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
            try { success = Write(job); }
            catch (const std::exception& e) { logger::warn("[foot snapshot] write failed: {}", e.what()); }
            catch (...) { logger::warn("[foot snapshot] write failed"); }
            {
                std::scoped_lock lock(mutex);
                pending.erase(job.key);
                if (success) done.insert(job.key);
            }
        }
    }
public:
    Writer() : worker([this](std::stop_token stop) { Run(stop); }) {}
    ~Writer() { worker.request_stop(); condition.notify_all(); worker.join(); }
    void Submit(WriteJob job) {
        std::scoped_lock lock(mutex);
        if (pending.contains(job.key) || done.contains(job.key)) return;
        if (jobs.size() >= 4) { logger::warn("[foot snapshot] writer queue full; retry after next equip event"); return; }
        pending.insert(job.key); jobs.push_back(std::move(job)); condition.notify_one();
    }
};
Writer& GetWriter() { static Writer writer; return writer; }

void Capture()
{
    std::unique_lock captureLock(g_captureMutex, std::try_to_lock);
    if (!captureLock.owns_lock() || !Enabled()) return;
    if (REL::Module::get().version() != REL::Version{1, 6, 1170, 0}) return;
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* base = player ? player->GetActorBase() : nullptr;
    if (!base) return;
    const auto& biped = player->GetBiped(false);
    if (!biped) return;
    const auto& part = biped->objects[37 - 30];
    RE::NiPointer<RE::NiAVObject> root = part.partClone;
    if (!root || !part.item || !part.addon) return;
    const auto armor = Stable(part.item), addon = Stable(part.addon);
    if (armor.empty() || addon.empty()) return;
    const auto sex = base->GetSex();
    const unsigned sexIndex = sex == RE::SEX::kFemale ? 1u : 0u;
    const char* model = part.addon->bipedModels[sexIndex].GetModel();
    const auto modelPath = std::string(model ? model : "");
    Json triPaths = Json::array();
    RE::BSVisit::TraverseScenegraphObjects(root.get(), [&](RE::NiAVObject* node) {
        if (auto* extra = node->GetExtraData<RE::NiStringExtraData>("BODYTRI"); extra && extra->value && *extra->value)
            triPaths.push_back(std::string(extra->value));
        return RE::BSVisit::BSVisitControl::kContinue;
    });
    const auto morphs = MorphValues(player);
    const auto morphHash = std::format("{:016x}", core::HashText(morphs.dump()));
    RE::BSVisit::TraverseScenegraphGeometries(root.get(), [&](RE::BSGeometry* geometry) {
        if (!geometry || std::string_view(geometry->name.c_str()) != "Feet") return RE::BSVisit::BSVisitControl::kContinue;
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
        Json extraction = {{"status", decoded.status}, {"shapeReportedVertexCount", view.shapeVertices},
            {"shapeReportedTriangleCount", view.shapeTriangles}, {"skinPartitionCount", view.partitions},
            {"skinPartitionVertexCount", view.vertices}, {"partitionLocalVertexCount", view.partitionVertices},
            {"partitionTriangleCount", view.triangles}, {"partitionBoneCount", view.boneCount},
            {"vertexDescriptor", std::format("{:016x}", view.descriptor)}, {"strideBytes", view.stride},
            {"positionSpanBytes", view.positionSpan}, {"componentBytes", view.componentBytes},
            {"gpuAllocationBytes", view.bufferBytes}, {"vertexMap", mapKind}, {"accessFault", view.fault},
            {"positionSource", "skin-partition.buffData.rawVertexData"}, {"indexSource", "skin-partition.triList"}};
        // Do not dereference skin/skinData again outside the guarded probe.
        const Json bind = view.bindAvailable ? Transform(view.bindTransform) : Json(nullptr);
        Json document = {{"schema", 1}, {"generatorVersion", "0.9.0"},
            {"source", "active-biped-foot-snapshot"}, {"status", "diagnostic-only"},
            {"identity", {{"armor", armor}, {"addon", addon}, {"armaModel", modelPath},
                {"bodyTriPaths", triPaths}, {"slot", 37}, {"buffered", false}, {"sexIndex", sexIndex},
                {"actorWeight", base->GetWeight()}, {"race", Stable(player->GetRace())}}},
            {"geometry", "Feet"}, {"coordinateSpace", "CPU vertex stream before bone transforms; NOT certified neutral"},
            {"localTransform", Transform(geometry->local)}, {"rootParentToSkin", bind},
            {"actorMorphValues", morphs}, {"actorMorphStateHash", morphHash},
            {"extraction", std::move(extraction)},
            {"geometryFit", {{"status", "not-run"}, {"posture", nullptr}, {"fitResidual", nullptr}}},
            {"hashAlgorithm", "FNV1a64 diagnostic fingerprint, ordered little-endian u32; not a security hash"},
            {"topologyFingerprint", nullptr}, {"positionFingerprint", nullptr},
            {"positions", Json::array()}, {"triangles", Json::array()}};
        if (decoded.Complete()) {
            document["topologyFingerprint"] = std::format("{:016x}", decoded.topologyHash);
            document["positionFingerprint"] = std::format("{:016x}", decoded.positionHash);
            document["positions"] = decoded.positions;
            document["triangles"] = decoded.triangles;
        }
        const std::string keySource = document["identity"].dump() + "|" + morphHash + "|" +
            std::to_string(decoded.topologyHash) + "|" + std::to_string(decoded.positionHash) + "|" +
            decoded.status + "|" + bind.dump() + "|" + document["localTransform"].dump();
        const auto key = std::format("{:016x}", core::HashText(keySource));
        document["captureKey"] = key;
        logger::info("[foot snapshot] armor='{}' addon='{}' status={} decodedVertices={} decodedTriangles={} map={}",
            armor, addon, decoded.status, decoded.positions.size(), decoded.triangles.size(), mapKind);
        GetWriter().Submit({key, std::move(document)});
        return RE::BSVisit::BSVisitControl::kContinue;
    });
}
} // namespace
void SetMorphInterface(IBodyMorphInterface* a_interface) { g_morph = a_interface; }
void RequestCapture()
{
    if (g_queued.exchange(true)) return;
    if (auto* tasks = SKSE::GetTaskInterface()) tasks->AddTask([] {
        g_queued.store(false);
        try { Capture(); }
        catch (const std::exception& e) { logger::warn("[foot snapshot] capture failed: {}", e.what()); }
        catch (...) { logger::warn("[foot snapshot] capture failed"); }
    });
    else g_queued.store(false);
}
} // namespace vanity_ube_heel_adapter::foot_capture
