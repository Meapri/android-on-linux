#pragma once

#include <string>
#include <string_view>

#include "alr_runtime/alr_path.hpp"

namespace alr::runtime {

struct PathHookSmokeResult {
    PathTranslation translation;
    bool opened = false;
    bool stated = false;
    long long size_bytes = -1;
    std::string first_bytes;
    std::string error;
    std::string report;
};

// Host-testable path hook smoke. This is the first clean-room hook layer:
// translate a guest path into the app-private rootfs and then perform direct
// host open/stat calls against the translated path. It does not claim full
// guest execution or LD_PRELOAD interposition yet.
PathHookSmokeResult run_path_hook_smoke(
    std::string_view rootfs_dir,
    std::string_view cwd,
    std::string_view path,
    std::size_t max_read_bytes = 128);

}  // namespace alr::runtime
