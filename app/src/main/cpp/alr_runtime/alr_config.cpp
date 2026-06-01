#include "alr_runtime/alr_config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "alr_runtime/alr_path.hpp"

namespace alr::runtime {
namespace {

constexpr std::string_view kFormat = "alr-config-v1";

bool is_absolute_host_path(std::string_view path) {
    return !path.empty() && path.front() == '/';
}

void reject_empty(std::string_view value, const char* name) {
    if (value.empty()) {
        throw std::invalid_argument(std::string(name) + " must not be empty");
    }
}

void reject_nul(std::string_view value, const char* name) {
    if (value.find('\0') != std::string_view::npos) {
        throw std::invalid_argument(std::string(name) + " must not contain NUL bytes");
    }
}

void require_absolute_host(std::string_view value, const char* name) {
    reject_empty(value, name);
    reject_nul(value, name);
    if (!is_absolute_host_path(value)) {
        throw std::invalid_argument(std::string(name) + " must be an absolute host path");
    }
}

void require_optional_absolute_host(std::string_view value, const char* name) {
    reject_nul(value, name);
    if (!value.empty() && !is_absolute_host_path(value)) {
        throw std::invalid_argument(std::string(name) + " must be an absolute host path when set");
    }
}

void validate_env_key(std::string_view key) {
    reject_empty(key, "env key");
    reject_nul(key, "env key");
    for (const char c : key) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (c == '=' || std::iscntrl(uc) != 0) {
            throw std::invalid_argument("env key contains an invalid character");
        }
    }
}

std::string normalize_required_guest_path(std::string_view path, const char* name) {
    reject_empty(path, name);
    reject_nul(path, name);
    if (!is_guest_absolute_path(path)) {
        throw std::invalid_argument(std::string(name) + " must be an absolute guest path");
    }
    return normalize_guest_path(path);
}

std::string escape_field(std::string_view value) {
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setfill('0');
    for (const unsigned char c : value) {
        if (c == '%' || c == '\n' || c == '\r' || c == '\t' || c < 0x20 || c == 0x7f) {
            out << '%' << std::setw(2) << static_cast<int>(c);
        } else {
            out << static_cast<char>(c);
        }
    }
    return out.str();
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

std::string unescape_field(std::string_view value) {
    std::string out;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '%') {
            out.push_back(value[i]);
            continue;
        }
        if (i + 2 >= value.size()) {
            throw std::invalid_argument("truncated escape in runtime config");
        }
        const int high = hex_value(value[i + 1]);
        const int low = hex_value(value[i + 2]);
        if (high < 0 || low < 0) {
            throw std::invalid_argument("invalid escape in runtime config");
        }
        out.push_back(static_cast<char>((high << 4) | low));
        i += 2;
    }
    return out;
}

std::vector<std::string_view> split_tabs(std::string_view line) {
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (start <= line.size()) {
        const std::size_t tab = line.find('\t', start);
        if (tab == std::string_view::npos) {
            fields.push_back(line.substr(start));
            break;
        }
        fields.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
    return fields;
}

std::string bool_text(bool value) {
    return value ? "1" : "0";
}

bool parse_bool(std::string_view value, const char* name) {
    if (value == "0") return false;
    if (value == "1") return true;
    throw std::invalid_argument(std::string(name) + " must be 0 or 1");
}

int parse_int(std::string_view value, const char* name) {
    if (value.empty() || value.size() > 2) {
        throw std::invalid_argument(std::string(name) + " is out of range");
    }
    int parsed = 0;
    for (const char c : value) {
        if (c < '0' || c > '9') {
            throw std::invalid_argument(std::string(name) + " must be numeric");
        }
        parsed = parsed * 10 + (c - '0');
    }
    return parsed;
}

void validate_runtime_config(RuntimeConfig& config) {
    reject_empty(config.package_name, "package_name");
    reject_nul(config.package_name, "package_name");
    require_absolute_host(config.rootfs_dir, "rootfs_dir");
    config.cwd = normalize_required_guest_path(config.cwd, "cwd");
    config.program = normalize_required_guest_path(config.program, "program");
    require_optional_absolute_host(config.hook_path, "hook_path");
    require_optional_absolute_host(config.interposer_path, "interposer_path");
    require_optional_absolute_host(config.bridge_path, "bridge_path");
    if (config.verbose < 0 || config.verbose > 2) {
        throw std::invalid_argument("verbose must be between 0 and 2");
    }
    for (const auto& [key, value] : config.env) {
        validate_env_key(key);
        reject_nul(value, "env value");
    }
    for (auto& bind : config.binds) {
        bind.guest_path = normalize_required_guest_path(bind.guest_path, "bind guest_path");
        require_absolute_host(bind.host_path, "bind host_path");
    }
    std::sort(config.binds.begin(), config.binds.end(), [](const BindMount& left, const BindMount& right) {
        if (left.guest_path == right.guest_path) {
            return left.host_path < right.host_path;
        }
        return left.guest_path < right.guest_path;
    });
}

void append_record(std::ostringstream& out, std::string_view kind, std::string_view key, std::string_view value) {
    out << kind << '\t' << escape_field(key) << '\t' << escape_field(value) << '\n';
}

}  // namespace

std::string runtime_config_checksum_hex(std::string_view text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << hash;
    return out.str();
}

SerializedRuntimeConfig serialize_runtime_config(const RuntimeConfig& input) {
    RuntimeConfig config = input;
    validate_runtime_config(config);

    std::ostringstream out;
    out << kFormat << '\n';
    append_record(out, "field", "package_name", config.package_name);
    append_record(out, "field", "rootfs_dir", config.rootfs_dir);
    append_record(out, "field", "cwd", config.cwd);
    append_record(out, "field", "program", config.program);
    append_record(out, "field", "hook_path", config.hook_path);
    append_record(out, "field", "interposer_path", config.interposer_path);
    append_record(out, "field", "bridge_path", config.bridge_path);
    append_record(out, "flag", "fake_root", bool_text(config.fake_root));
    append_record(out, "flag", "verbose", std::to_string(config.verbose));
    append_record(out, "flag", "trace_path", bool_text(config.trace_path));
    append_record(out, "flag", "trace_exec", bool_text(config.trace_exec));
    for (const auto& [key, value] : config.env) {
        append_record(out, "env", key, value);
    }
    for (const auto& bind : config.binds) {
        append_record(out, "bind", bind.guest_path, bind.host_path);
    }

    const std::string text = out.str();
    return SerializedRuntimeConfig{
        .text = text,
        .checksum_hex = runtime_config_checksum_hex(text),
    };
}

RuntimeConfig parse_runtime_config(std::string_view text) {
    reject_empty(text, "runtime config");
    reject_nul(text, "runtime config");

    RuntimeConfig config;
    bool saw_header = false;
    std::size_t line_start = 0;
    while (line_start <= text.size()) {
        const std::size_t newline = text.find('\n', line_start);
        const std::size_t line_end = newline == std::string_view::npos ? text.size() : newline;
        const std::string_view line = text.substr(line_start, line_end - line_start);
        if (!saw_header) {
            if (line != kFormat) {
                throw std::invalid_argument("unsupported runtime config format");
            }
            saw_header = true;
        } else if (!line.empty()) {
            const auto fields = split_tabs(line);
            if (fields.size() != 3) {
                throw std::invalid_argument("runtime config record must have three fields");
            }
            const std::string kind = unescape_field(fields[0]);
            const std::string key = unescape_field(fields[1]);
            const std::string value = unescape_field(fields[2]);
            if (kind == "field") {
                if (key == "package_name") config.package_name = value;
                else if (key == "rootfs_dir") config.rootfs_dir = value;
                else if (key == "cwd") config.cwd = value;
                else if (key == "program") config.program = value;
                else if (key == "hook_path") config.hook_path = value;
                else if (key == "interposer_path") config.interposer_path = value;
                else if (key == "bridge_path") config.bridge_path = value;
                else throw std::invalid_argument("unknown runtime config field");
            } else if (kind == "flag") {
                if (key == "fake_root") config.fake_root = parse_bool(value, "fake_root");
                else if (key == "verbose") config.verbose = parse_int(value, "verbose");
                else if (key == "trace_path") config.trace_path = parse_bool(value, "trace_path");
                else if (key == "trace_exec") config.trace_exec = parse_bool(value, "trace_exec");
                else throw std::invalid_argument("unknown runtime config flag");
            } else if (kind == "env") {
                config.env[key] = value;
            } else if (kind == "bind") {
                config.binds.push_back(BindMount{
                    .guest_path = key,
                    .host_path = value,
                });
            } else {
                throw std::invalid_argument("unknown runtime config record kind");
            }
        }
        if (newline == std::string_view::npos) {
            break;
        }
        line_start = newline + 1;
    }
    validate_runtime_config(config);
    return config;
}

}  // namespace alr::runtime
