#include "alr_runtime/alr_env.hpp"

#include <stdexcept>

namespace alr::runtime {
namespace {

void require_non_empty(const std::string& value, const char* name) {
    if (value.empty()) {
        throw std::invalid_argument(std::string(name) + " must not be empty");
    }
}

}  // namespace

GuestEnvironment build_guest_environment(const GuestEnvironmentInput& input) {
    require_non_empty(input.package_name, "package_name");
    require_non_empty(input.rootfs_dir, "rootfs_dir");
    require_non_empty(input.home, "home");
    require_non_empty(input.tmpdir, "tmpdir");
    require_non_empty(input.path, "path");

    return GuestEnvironment{
        .values = {
            {"ALR_PACKAGE", input.package_name},
            {"ALR_ROOTFS", input.rootfs_dir},
            {"HOME", input.home},
            {"TMPDIR", input.tmpdir},
            {"PATH", input.path},
        },
    };
}

}  // namespace alr::runtime
