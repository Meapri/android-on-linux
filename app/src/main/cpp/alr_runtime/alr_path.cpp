#include "alr_runtime/alr_path.hpp"

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The path rule that is on the product path and has tests behind it.
extern "C" {
#include "alr_path_rule.h"
}

namespace alr::runtime {
namespace {

void reject_nul(std::string_view value, const char* name) {
    if (value.find('\0') != std::string_view::npos) {
        throw std::invalid_argument(std::string(name) + " must not contain NUL bytes");
    }
}

std::string trim_trailing_slashes(std::string_view path) {
    std::string out(path);
    while (out.size() > 1 && out.back() == '/') {
        out.pop_back();
    }
    return out;
}

// Components are stored as views into the original path/cwd buffers (which the
// caller keeps alive for the whole normalize call), so a multi-segment path is
// split without a heap allocation per segment. Only the final joined string
// allocates.
void append_components(std::vector<std::string_view>& components, std::string_view path) {
    std::size_t pos = 0;
    while (pos <= path.size()) {
        const std::size_t slash = path.find('/', pos);
        const std::size_t end = slash == std::string_view::npos ? path.size() : slash;
        const std::string_view component = path.substr(pos, end - pos);
        if (component.empty() || component == ".") {
            // Ignore empty components from repeated or boundary slashes.
        } else if (component == "..") {
            if (!components.empty()) {
                components.pop_back();
            }
        } else {
            components.emplace_back(component);
        }
        if (slash == std::string_view::npos) {
            break;
        }
        pos = slash + 1;
    }
}

std::string components_to_absolute_path(const std::vector<std::string_view>& components) {
    if (components.empty()) {
        return "/";
    }
    std::size_t total = 0;
    for (const auto& component : components) {
        total += 1 + component.size();
    }
    std::string out;
    out.reserve(total);
    for (const auto& component : components) {
        out += "/";
        out += component;
    }
    return out;
}

}  // namespace

bool is_guest_absolute_path(std::string_view path) {
    return !path.empty() && path.front() == '/';
}

std::string normalize_guest_path(std::string_view path, std::string_view cwd) {
    reject_nul(path, "path");
    reject_nul(cwd, "cwd");
    if (!is_guest_absolute_path(cwd)) {
        throw std::invalid_argument("cwd must be an absolute guest path");
    }

    std::vector<std::string_view> components;
    if (is_guest_absolute_path(path)) {
        append_components(components, path);
    } else {
        append_components(components, cwd);
        append_components(components, path);
    }
    return components_to_absolute_path(components);
}

PathTranslation translate_rootfs_path(
    std::string_view rootfs_dir,
    std::string_view cwd,
    std::string_view path) {
    reject_nul(rootfs_dir, "rootfs_dir");
    if (!is_guest_absolute_path(rootfs_dir)) {
        throw std::invalid_argument("rootfs_dir must be an absolute host path");
    }

    const std::string rootfs = trim_trailing_slashes(rootfs_dir);
    const std::string guest = normalize_guest_path(path, cwd);

    // TWO CASES THIS USED TO GET WRONG, both found by a differential test
    // against runtime/alr's alr_rw() -- the same rule, on the product path,
    // with 73 host assertions behind it. 7 of 14 shared cases disagreed.
    //
    //   sysdirs   /proc, /sys and /dev must resolve against ANDROID. The guest
    //             rootfs has empty stubs for them, and prefixing sent the
    //             loader to read /proc/self/exe out of a directory that has no
    //             kernel behind it.
    //
    //   already-host  a path that is ALREADY under the rootfs was prefixed a
    //             SECOND time:
    //               <R>/etc/os-release -> <R><R>/etc/os-release
    //             which is how a host path handed back to us turns into a file
    //             that does not exist.
    //
    // Delegating to alr_rw rather than re-deriving the rule here: one of these
    // implementations is tested and on the product path, and it is not this
    // one. Divergence between two copies of a path rule is not a tidiness
    // problem -- it resolves to two different files and nothing reports it.
    char buf[4096];
    int err = 0;
    const char* rewritten =
        alr_rw(guest.c_str(), rootfs.c_str(), rootfs.size(), buf, sizeof buf, &err);
    const std::string host =
        (rewritten == nullptr) ? (guest == "/" ? rootfs : rootfs + guest)
                               : std::string(rewritten);
    return PathTranslation{
        .guest_path = guest,
        .host_path = host,
        .escaped_rootfs = false,
    };
}

}  // namespace alr::runtime
