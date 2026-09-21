#include "../src/FootSnapshotCore.h"
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace vanity_ube_heel_adapter::foot_snapshot_core;
static int checks = 0;
static void Check(bool value, const char* name) {
    ++checks; if (!value) { std::cerr << "FAIL " << name << '\n'; std::exit(1); }
}
int main() {
    std::vector<std::uint8_t> data(3 * 20, 0);
    const float p[9]{0, 0, 0, 1, 0, 0, 0, 1, 0};
    for (int i = 0; i < 3; ++i) std::memcpy(data.data() + 20 * i, p + 3 * i, 12);
    std::vector<std::uint16_t> idx{0, 1, 2};
    const auto good = Decode(data, idx, 3, 20, 4);
    Check(good.Complete(), "float32 interleaved stream");
    Check(good.positions.size() == 3 && good.triangles.size() == 1, "counts");
    Check(good.positions[1][0] == 1 && good.positions[2][1] == 1, "positions");
    Check(!Decode(data, idx, 0, 20, 4).Complete(), "zero vertices");
    Check(!Decode(data, idx, 65536, 20, 4).Complete(), "oversize vertices");
    Check(!Decode(data, idx, 3, 2, 4).Complete(), "short stride");
    Check(!Decode(data, idx, 3, 300, 4).Complete(), "large stride");
    Check(!Decode(data, idx, 3, 20, 1).Complete(), "bad component type");
    Check(!Decode(std::span(data).first(59), idx, 3, 20, 4).Complete(), "truncated stream");
    Check(!Decode(data, {}, 3, 20, 4).Complete(), "empty indices");
    Check(!Decode(data, std::span(idx).first(2), 3, 20, 4).Complete(), "truncated triangles");
    idx[2] = 3; Check(!Decode(data, idx, 3, 20, 4).Complete(), "out of range index"); idx[2] = 2;
    float invalid = std::numeric_limits<float>::quiet_NaN(); std::memcpy(data.data(), &invalid, 4);
    Check(!Decode(data, idx, 3, 20, 4).Complete(), "reject nan");
    invalid = INFINITY; std::memcpy(data.data(), &invalid, 4);
    Check(!Decode(data, idx, 3, 20, 4).Complete(), "reject infinity");
    invalid = 0; std::memcpy(data.data(), &invalid, 4);
    idx = {0, 0, 0}; Check(!Decode(data, idx, 3, 20, 4).Complete(), "all degenerate"); idx = {0, 1, 2};
    Check(Decode(data, idx, 3, 20, 4).topologyHash == good.topologyHash, "stable topology hash");
    idx = {0, 2, 1}; Check(Decode(data, idx, 3, 20, 4).topologyHash != good.topologyHash, "ordered index hash"); idx = {0, 1, 2};
    invalid = 2; std::memcpy(data.data(), &invalid, 4);
    auto changed = Decode(data, idx, 3, 20, 4);
    Check(changed.topologyHash == good.topologyHash && changed.positionHash != good.positionHash, "separate hashes");
    Check(HalfToFloat(0x3C00) == 1 && HalfToFloat(0xC000) == -2, "half normal");
    Check(HalfToFloat(1) > 0 && HalfToFloat(0x7C00) == INFINITY, "half subnormal and infinity");
    std::vector<std::uint8_t> half(24, 0); std::uint16_t one = 0x3C00;
    std::memcpy(half.data() + 8, &one, 2); std::memcpy(half.data() + 18, &one, 2);
    Check(Decode(half, idx, 3, 8, 2).Complete(), "half stream");
    Check(Decode(half, idx, 3, 8, 2).positions == good.positions, "half equivalence");
    std::cout << checks << " checks passed\n";
}
