#pragma once

#include <map>
#include <string>
#include <string_view>

namespace alr::runtime {

struct GuestEnvironmentInput {
    std::string package_name;
    std::string rootfs_dir;
    std::string home = "/root";
    std::string tmpdir = "/tmp";
    std::string path = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
};

struct GuestEnvironment {
    std::map<std::string, std::string> values;
};

// Build the minimal deterministic environment shared by ALR runtime backends.
// Backend-specific variables can be layered on top by launch-plan code.
GuestEnvironment build_guest_environment(const GuestEnvironmentInput& input);

}  // namespace alr::runtime
