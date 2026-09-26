#pragma once
// An indexed library of explicit, source-validated OFFLINE suggestions.
// No game pointers, engine callbacks or JSON parser in the disk worker.
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace vanity_ube_heel_adapter::bulk_height {
struct Identity { std::string armor, addon, model; bool operator==(const Identity&) const = default; };
struct Controls { double noHeel{}, heel{}; };
struct Asset { std::string resource; std::uint64_t fingerprint{}; };
struct Member {
    Identity stocking; std::uint32_t group{}, flags{};
    double originalNoHeel{}, originalHeel{}, residual{};
    std::vector<std::uint32_t> sources;
};
struct Shard { Identity shoe; double weight{}; std::vector<Asset> assets; std::vector<Controls> groups; std::vector<Member> members; };
struct IndexEntry { Identity shoe; std::string file; std::uint64_t fingerprint{}; std::uint32_t count{}; };
struct Index { std::string generation; std::vector<Identity> stockings; std::vector<IndexEntry> entries; };
std::string Normalize(std::string value);
std::string Resource(std::string value);
std::string Key(const Identity& value);
std::uint64_t Fingerprint(std::span<const std::uint8_t> data);
Index DecodeIndex(std::span<const std::uint8_t> data);
Shard DecodeShard(std::span<const std::uint8_t> data);
struct Request {
    Identity shoe, stocking;
    // Changes to the actual RaceMenu context use a different cache key. Weight
    // must agree with the source build; this is not a live OBody recalibration.
    std::string context;
    double weight{}, heelMax{2.0};
    bool allowResidualWarnings{true};
};
struct Reply {
    std::string state{"pending"}, generation, detail;
    Controls values; double originalResidual{}; std::uint32_t flags{};
    bool Ready() const { return state=="ready"; }
};
struct Stats {std::uint64_t indexReads{},shardReads{},assetReads{};};
class Library {
public:
    Library(std::filesystem::path root, std::filesystem::path meshes, std::function<void()> callback);
    ~Library();
    Library(const Library&)=delete;
    Library& operator=(const Library&)=delete;
    // Caller only touches small value snapshots. Disk reads never occur here.
    Reply Lookup(Request request);
    bool IsStocking(Identity identity);
    Stats Counters() const;
    // Also invalidates requests already in flight. Next request reloads index.
    void Reset();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
void Initialize(void (*callback)());
void Reset();
bool IsStocking(Identity identity);
Stats Counters();
Reply Lookup(Request request);
}
