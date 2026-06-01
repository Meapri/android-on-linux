#pragma once

#include <string>
#include <string_view>

namespace alr::runtime {

struct PathTranslation {
    std::string guest_path;
    std::string host_path;
    bool escaped_rootfs;
};

// Returns true when path starts at the guest root namespace.
bool is_guest_absolute_path(std::string_view path);

// Normalize a guest path using deterministic lexical rules:
// - relative paths are resolved against cwd
// - repeated slashes and "." components are removed
// - ".." components pop one component but clamp at guest root
// - the returned path is always absolute and has no trailing slash except "/"
// Throws std::invalid_argument for empty/relative cwd or embedded NUL bytes.
std::string normalize_guest_path(std::string_view path, std::string_view cwd = "/");

// Translate a guest path to a host path under rootfs_dir without consulting the
// host filesystem. The resulting host_path is rootfs_dir + guest_path. ".."
// traversal is clamped at guest root, so escaped_rootfs is false for every
// successful translation.
PathTranslation translate_rootfs_path(
    std::string_view rootfs_dir,
    std::string_view cwd,
    std::string_view path);

}  // namespace alr::runtime
