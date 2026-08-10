#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "alr_runtime/alr_config.hpp"

namespace {

const char* env_or_none(const char* key) {
    const char* value = std::getenv(key);
    return value == nullptr || value[0] == '\0' ? "none" : value;
}

std::string required_env(const char* key) {
    const char* value = std::getenv(key);
    if (value == nullptr || value[0] == '\0') {
        throw std::runtime_error(std::string("missing env ") + key);
    }
    return value;
}

int run_preflight() {
    // Not a verdict. Reaching this line means the packaged trampoline was
    // exec'd, which is worth printing -- but it can never print FAIL, so it
    // must not wear a PASS/FAIL token. The line that DOES decide is
    // ALR PACKAGED TRAMPOLINE CONTINUE EXECUTION, which checks a child's
    // stdout for this program's own marker.
    std::cout << "ALR TRAMPOLINE PREFLIGHT: reached (the trampoline is running)\n";
    std::cout << "alr trampoline mode=" << env_or_none("ALR_TRAMPOLINE_MODE") << "\n";
    std::cout << "alr trampoline config checksum=" << env_or_none("ALR_CONFIG_CHECKSUM") << "\n";
    std::cout << "alr trampoline target guest=" << env_or_none("ALR_TRAMPOLINE_TARGET_GUEST_PATH") << "\n";
    std::cout << "alr trampoline target host=" << env_or_none("ALR_TRAMPOLINE_TARGET_HOST_PATH") << "\n";
    std::cout << "alr trampoline elf status=" << env_or_none("ALR_TRAMPOLINE_ELF_STATUS") << "\n";
    return 0;
}

int run_continue_exec(int argc, char** argv) {
    try {
        std::string kind = "missing";
        std::string target_arg = "missing";
        int extra_argc = 0;
        for (int index = 2; index < argc; ++index) {
            const std::string_view arg = argv[index];
            if (arg == "--kind" && index + 1 < argc) {
                kind = argv[++index];
            } else if (arg == "--" && index + 1 < argc) {
                target_arg = argv[++index];
                extra_argc = argc - index - 1;
                break;
            }
        }

        const auto config_text = required_env("ALR_CONFIG_TEXT");
        const auto expected_checksum = required_env("ALR_CONFIG_CHECKSUM");
        const auto parsed = alr::runtime::parse_runtime_config(config_text);
        const auto actual_checksum = alr::runtime::runtime_config_checksum_hex(config_text);
        const bool checksum_ok = actual_checksum == expected_checksum;
        const auto continuation_kind = required_env("ALR_CONTINUATION_KIND");
        const auto continuation_target = required_env("ALR_CONTINUATION_TARGET_GUEST_PATH");
        const bool target_ok = parsed.program == continuation_target && target_arg == continuation_target;
        const bool kind_ok = kind == continuation_kind;

        std::cout << "ALR TRAMPOLINE CONTINUE EXEC: "
                  << (checksum_ok && target_ok && kind_ok ? "PASS" : "FAIL") << "\n";
        std::cout << "ALR TRAMPOLINE CONTINUE CONFIG: " << (target_ok ? "PASS" : "FAIL") << "\n";
        std::cout << "ALR TRAMPOLINE CONTINUE CHECKSUM: " << (checksum_ok ? "PASS" : "FAIL") << "\n";
        std::cout << "ALR TRAMPOLINE CONTINUE KIND: " << (kind_ok ? "PASS" : "FAIL") << "\n";
        std::cout << "alr trampoline continue mode=dry-run\n";
        std::cout << "alr trampoline continue kind=" << kind << "\n";
        std::cout << "alr trampoline continue target guest=" << continuation_target << "\n";
        std::cout << "alr trampoline continue argv target=" << target_arg << "\n";
        std::cout << "alr trampoline continue extra argc=" << extra_argc << "\n";
        std::cout << "alr trampoline continue rootfs=" << parsed.rootfs_dir << "\n";
        return checksum_ok && target_ok && kind_ok ? 0 : 3;
    } catch (const std::exception& exc) {
        std::cerr << "ALR TRAMPOLINE CONTINUE EXEC: FAIL\n";
        std::cerr << "alr trampoline continue error=" << exc.what() << "\n";
        return 4;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc <= 1) {
        std::cerr << "ALR TRAMPOLINE MODE: FAIL\n";
        std::cerr << "alr trampoline reason=missing mode\n";
        return 2;
    }
    const std::string_view mode = argv[1];
    if (mode == "--preflight") {
        return run_preflight();
    }
    if (mode == "--continue-exec") {
        return run_continue_exec(argc, argv);
    }
    std::cerr << "ALR TRAMPOLINE MODE: FAIL\n";
    std::cerr << "alr trampoline reason=unsupported mode\n";
    return 2;
}
