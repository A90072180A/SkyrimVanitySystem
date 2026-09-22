#pragma once
// The only accepted configuration snapshot. Workers never independently read a
// half-written user file; the controller publishes a complete validated pair.
#include <nlohmann/json.hpp>
#include <mutex>
namespace vanity_ube_heel_adapter::config_state {
inline std::mutex mutex;
inline nlohmann::json active={{"applyMorph",false},{"automaticHeight",false}};
inline nlohmann::json Get(){std::scoped_lock lock(mutex);return active;}
inline void Set(nlohmann::json value){std::scoped_lock lock(mutex);active=std::move(value);}
}
