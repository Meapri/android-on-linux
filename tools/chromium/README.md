# chromium NETWORK overlay (CR-1 / CR-2)

Host-side builder + assets that unblock the next two chromium-on-ALR run
milestones. **HOST-ONLY** — the actual on-device render/load is the
integration/device gate (WS-1 stages the tar).

| milestone | what it proves | launch (device, WS-1) |
|---|---|---|
| **CR-1** | single-process headless render of a *local* page, **zero network** | `chromium-headless-shell --single-process --no-zygote --no-sandbox --dump-dom file:///root/cr-test.html` (or a `data:` URL) |
| **CR-2** | the same, loading a *real* `https://` URL (DNS + TLS work) | `chromium-headless-shell ... --dump-dom https://example.com` |

## Files

- **`cr-test.html`** — a self-contained CR-1 page: an `<h1>`, a CSS-gradient
  `<div>`, and an inline `<svg>`. No external fetch (no `link rel`, `script src`,
  `img src`, web font, or CSS URL import), so it renders with networking entirely
  absent. Packed into the overlay at `/root/cr-test.html`.
- **`../build_chromium_net_overlay.py`** — packs `chromium-net-stage.tar`
  (rootfs-absolute, §5-E `./`-rooted). Members:
  - `/etc/resolv.conf` — `nameserver 8.8.8.8` + `1.1.1.1`
  - `/etc/nsswitch.conf` — `hosts: files dns`
  - `/etc/ssl/certs/ca-certificates.crt` — Mozilla CA bundle (assembled from the
    noble `ca-certificates` .deb; see below)
  - `/root/cr-test.html` — the CR-1 page

## Build

```sh
# full pack (network: fetches the noble ca-certificates .deb, assembles the bundle)
python -m tools.build_chromium_net_overlay --out /tmp/chromium-net-stage.tar

# dry-run (no download): resolve the .deb + print the planned members
python -m tools.build_chromium_net_overlay --list

# offline selftest
python -m tools.build_chromium_net_overlay --selftest
```

Host-verified pack: **4 files**, CA bundle **146 certs / ~214 KiB**, from
`pool/main/c/ca-certificates/ca-certificates_20240203_all.deb`; `stage_tar_spec`
CONFORMANT (0 errors / 0 warnings).

### Why the CA bundle is *assembled* host-side

The noble `ca-certificates` .deb does **not** ship a pre-built
`/etc/ssl/certs/ca-certificates.crt`. It ships the individual trusted-CA PEMs
under `/usr/share/ca-certificates/mozilla/*.crt`; the *bundle* is normally built at
install time by the `update-ca-certificates` postinst (concatenating the enabled
certs per `/etc/ca-certificates.conf`). The .deb ships **no** `ca-certificates.conf`
either — debconf generates it at install with all Mozilla CAs enabled by default.
So `assemble_ca_bundle` reproduces that default: it concatenates every PEM `.crt`
the .deb ships, sorted by path, each newline-terminated — exactly the bundle a
normal `apt install ca-certificates` would land. No on-device postinst/maintscript
run is needed (which matters: maintainer-script fork/exec under the ALR loader is
still gated — see the apt/dpkg notes).

## ANALYSIS — ALR does NOT mediate sockets; network "just works" once staged

The ALR loader's seccomp design traces **only two syscall families** (read from
`app/src/main/cpp/runtime_report.cpp`):

- `alr_install_path_trace_filter` → `RET_TRACE` for the 9 **path** syscalls
  (`openat`, `openat2`, `newfstatat`, `statx`, `faccessat{,2}`, `readlinkat`,
  `mkdirat`, `unlinkat`); `RET_ALLOW` for everything else. (PCGATE A/B baseline.)
- `alr_install_execve_trace_filter` → `RET_TRACE` for `execve` / `execveat` only;
  `RET_ALLOW` for everything else. (PCGATE default; the in-process LD_PRELOAD
  interposer installs its own PC-gated *path* filter on top.)

**Neither filter names `socket` / `connect` / `bind` / `sendto` / `recvfrom` /
`sendmsg` / `recvmsg` / `getsockopt`**, and seccomp's default action for an
un-named syscall in these filters is `RET_ALLOW`. So chromium's TCP/UDP sockets
pass straight through to the Android kernel **un-traced, at native speed** — there
is no socket mediation, no userspace proxy, no per-packet supervision.
(untrusted_app holds `INTERNET`, so the kernel/SELinux side permits outbound
sockets too.)

The only thing the loader's path mediation touches that the network path needs is
the **file reads** of `/etc/resolv.conf`, `/etc/nsswitch.conf`, `/etc/hosts`, and
the CA bundle — which the path filter rewrites into the rootfs. So the entire
"network gap" is **name resolution config + TLS roots**, both plain files this
overlay supplies:

- **DNS** — glibc reads `/etc/resolv.conf` (→ 8.8.8.8 / 1.1.1.1) + `nsswitch.conf`
  (→ `dns`), then opens a UDP socket to `:53` (un-traced, native).
- **TLS** — chromium/openssl reads `/etc/ssl/certs/ca-certificates.crt` for the
  root store, then does the TLS handshake over a TCP socket (un-traced).

**Bottom line for WS-1:** once `chromium-net-stage.tar` is staged, network should
"just work" — there is no socket-mediation work to do in the loader. If you prefer
env injection over rootfs files, the equivalents are a `resolv.conf` path /
`RES_OPTIONS` for DNS and `SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt` /
`CURL_CA_BUNDLE` for TLS — but shipping the files is the zero-env, "looks like a
normal rootfs" path and is what this overlay does.

## Device gate (WS-1)

1. Stage `chromium-net-stage.tar` (its own `.staged` marker).
2. **CR-1:** `--dump-dom file:///root/cr-test.html` headless single-process →
   DOM contains `ALR chromium renders.` + the SVG (no network touched).
3. **CR-2:** `--dump-dom https://<real-url>` → page loads (DNS resolves via
   resolv.conf, TLS verifies against the CA bundle).
