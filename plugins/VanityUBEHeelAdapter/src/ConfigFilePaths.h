#pragma once
// Resolve the actual backing files once per process, not an optional virtual
// filename on every poll. No mod-directory search or user-configuration writes.
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#include <Windows.h>
#endif
namespace vanity_ube_heel_adapter::config_file_paths {
namespace fs=std::filesystem;
inline std::string Text(const fs::path& p) {
    const auto s=p.u8string();return std::string(s.begin(),s.end());
}
#ifdef _WIN32
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    explicit Handle(HANDLE h):value(h){}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
    ~Handle(){if(value!=INVALID_HANDLE_VALUE)::CloseHandle(value);}
};
inline fs::path FinalPath(HANDLE h) {
    auto n=::GetFinalPathNameByHandleW(h,nullptr,0,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!n||n>32768)throw std::runtime_error("cannot resolve configuration backing path");
    std::vector<wchar_t> text(n+1);
    n=::GetFinalPathNameByHandleW(h,text.data(),static_cast<DWORD>(text.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!n||n>=text.size())throw std::runtime_error("configuration backing path query failed");
    // Keep the extended-length prefix internally, including UNC paths.
    return fs::path(std::wstring(text.data(),n));
}
#endif
inline std::optional<fs::path> Existing(const fs::path& path) {
#ifdef _WIN32
    Handle h(::CreateFileW(path.c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(h.value==INVALID_HANDLE_VALUE){auto error=::GetLastError();
        if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)return {};
        throw std::runtime_error("configuration path open failed: "+std::to_string(error));}
    return FinalPath(h.value);
#else
    std::error_code ec;const bool exists=fs::exists(path,ec);
    if(ec)throw std::runtime_error("configuration path inaccessible: "+ec.message());
    if(!exists)return {};return fs::canonical(path);
#endif
}
inline fs::path Output(const fs::path& virtualDirectory) {
    // Prefer the already-winning runtime report to preserve the existing output
    // mod. For a first run, a disposable probe discovers MO2's write destination.
    if(auto file=Existing(virtualDirectory/"runtime-state.json"))return file->parent_path();
    fs::create_directories(virtualDirectory);
    const auto name=".configuration-path-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".tmp";
    const auto probe=virtualDirectory/name;
#ifdef _WIN32
    Handle h(::CreateFileW(probe.c_str(),GENERIC_READ|GENERIC_WRITE,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,nullptr));
    if(h.value==INVALID_HANDLE_VALUE)throw std::runtime_error("cannot discover configuration output path");
    return FinalPath(h.value).parent_path();
#else
    {std::ofstream f(probe,std::ios::binary);if(!f)throw std::runtime_error("cannot discover configuration output path");}
    auto result=fs::canonical(probe).parent_path();fs::remove(probe);return result;
#endif
}
struct Paths {fs::path base,user,output,virtualBase,virtualUser;std::string userMode;};
inline Paths Discover(const fs::path& directory) {
    const auto plugins=fs::absolute(directory)/"Data/SKSE/Plugins";
    Paths p;p.virtualBase=plugins/"VanityUBEHeelAdapter.json";
    p.virtualUser=plugins/"VanityUBEHeelAdapter.user.json";
    auto base=Existing(p.virtualBase);if(!base)throw std::runtime_error("base configuration missing");
    p.base=*base;p.output=Output(plugins/"VanityUBEHeelAdapter");
    if(auto user=Existing(p.virtualUser)){p.user=*user;p.userMode="existing-winning-overlay";}
    else {p.user=p.output.parent_path()/"VanityUBEHeelAdapter.user.json";p.userMode="output-sibling-overlay";}
    return p;
}
}
