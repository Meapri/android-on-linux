#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace alr::runtime {

struct BindMount {
    std::string guest_path;
    std::string host_path;
};

struct RuntimeConfig {
    std::string package_name;
    std::string rootfs_dir;
    std::string cwd = "/";
    std::string program;
    std::map<std::string, std::string> env;
    std::vector<BindMount> binds;
    std::string hook_path;
    std::string interposer_path;
    std::string bridge_path;
    bool fake_root = false;
    int verbose = 0;
    bool trace_path = false;
    bool trace_exec = false;
};

struct SerializedRuntimeConfig {
    std::string text;
    std::string checksum_hex;
};

// Deterministic config contract for passing ALR state through launcher and
// future exec/spawn boundaries. The format is line-record based and escaped so
// it can be written to an fd or env-backed payload without depending on JSON.
SerializedRuntimeConfig serialize_runtime_config(const RuntimeConfig& config);
RuntimeConfig parse_runtime_config(std::string_view text);
std::string runtime_config_checksum_hex(std::string_view text);

}  // namespace alr::runtime
