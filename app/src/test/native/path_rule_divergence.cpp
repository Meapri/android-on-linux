// Differential test: the app's translate_rootfs_path vs alr's alr_rw.
//
// Two implementations of the same rule live in this repo -- runtime/alr's
// alr_rw() (73 host assertions, on the product path) and the in-process
// loader's translate_rootfs_path(). Duplication is tolerable; DIVERGENCE is
// not, because a path that the loader and the runtime disagree about resolves
// to two different files and nothing reports it.
#include <cstdio>
#include <cstring>
#include <string>
#include "alr_runtime/alr_path.hpp"
extern "C" {
#include "alr_path_rule.h"
}
int main() {
    const char* R = "/data/user/0/pkg/files/rootfs/u";
    struct Case { const char* cwd; const char* in; } cases[] = {
        {"/", "/etc/os-release"}, {"/", "/"}, {"/", "/usr/bin/env"},
        {"/", "/proc/self/exe"},  {"/", "/sys/kernel"}, {"/", "/dev/null"},
        {"/root", "rel/path"},    {"/root", "."},       {"/root", ".."},
        {"/", "/etc/../etc/os-release"}, {"/", "//etc//os-release"},
        {"/", "/etc/"},           {"/", "/../../etc/passwd"},
        {"/", "/data/user/0/pkg/files/rootfs/u/etc/os-release"},
    };
    // Relative inputs are an INTENDED difference, not a divergence: alr never
    // rewrites them (the kernel resolves them against the process cwd, which is
    // already inside the rootfs), while the in-process loader has no kernel doing
    // that for it and must resolve them itself. Listed explicitly so the
    // exemption is a decision rather than a gap.
    auto intended = [](const char* in) {
        return in[0] != '/';
    };
    int diff = 0, n = 0, exempt = 0;
    for (auto& c : cases) {
        char buf[4096];
        int err = 0;
        const char* alr = alr_rw(c.in, R, strlen(R), buf, sizeof buf, &err);
        std::string app;
        try {
            app = alr::runtime::translate_rootfs_path(R, c.cwd, c.in).host_path;
        } catch (const std::exception& e) { app = std::string("THROW:") + e.what(); }
        const std::string a = alr ? alr : "(null)";
        n++;
        if (a != app && intended(c.in)) { exempt++; continue; }
        if (a != app) {
            diff++;
            printf("  DIVERGE cwd=%-6s in=%-46s\n     alr=%s\n     app=%s\n",
                   c.cwd, c.in, a.c_str(), app.c_str());
        }
    }
    printf("%d cases, %d divergent, %d intended (relative inputs)\n", n, diff, exempt);
    if (diff) {
        printf("ALR PATH RULE DIVERGENCE: FAIL\n");
        return 1;
    }
    printf("ALR PATH RULE DIVERGENCE: PASS\n");
    return 0;
}
