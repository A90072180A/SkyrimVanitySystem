#pragma once
#include <cmath>
#include <string>
#include <span>
#include <vector>
#include <unordered_set>
namespace vanity_ube_heel_adapter::scoped_morph_transaction {
struct Target { std::string name; float value{}; };
// Backend methods are Has/Get/Effective/Set/Clear/Apply. Only our own key is
// changed. Verify the actual aggregate rather than assuming RaceMenu uses sums.
// No callbacks escape this synchronous scope. A geometry update is NOT proof
// of visual acceptance; this transaction guarantees key restoration only.
template<class Backend> bool Run(Backend& backend, std::span<const Target> values) {
    if(values.empty() || values.size()>8) return false;
    std::unordered_set<std::string> names;
    struct Saved { std::string name; bool existed{}; float own{}, effective{}; };
    std::vector<Saved> saved;
    for(const auto& v:values) {
        if(v.name.empty() || !std::isfinite(v.value) || !names.insert(v.name).second) return false;
        const float own=backend.Get(v.name),effective=backend.Effective(v.name);
        if(!std::isfinite(own) || !std::isfinite(effective)) return false;
        saved.push_back({v.name,backend.Has(v.name),own,effective});
    }
    struct Restore {
        Backend& backend; const std::vector<Saved>& saved;
        ~Restore() noexcept {
            for(auto i=saved.rbegin();i!=saved.rend();++i) {
                try {if(i->existed) backend.Set(i->name,i->own);else backend.Clear(i->name);} catch(...) {}
            }
        }
    } restore{backend,saved};
    for(std::size_t i=0;i<values.size();++i) {
        const float own=values[i].value-(saved[i].effective-saved[i].own);
        if(!std::isfinite(own)) return false;
        backend.Set(values[i].name,own);
    }
    // Non-additive aggregation (e.g. a maximum) may make the requested reduction
    // impossible without deleting another mod's key. Refuse instead of doing it.
    for(const auto& v:values) {
        const auto actual=backend.Effective(v.name);
        if(!std::isfinite(actual) || std::abs(actual-v.value)>1e-5f) return false;
    }
    backend.Apply();
    return true;
}
} // namespace vanity_ube_heel_adapter::scoped_morph_transaction
