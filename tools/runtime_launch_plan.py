from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import PurePosixPath


CONFIG_FORMAT = "alr-config-v1"


@dataclass(frozen=True)
class LaunchPlan:
    executable: str
    native_library_dir: str
    rootfs_dir: str
    argv: list[str]
    env: dict[str, str] = field(default_factory=dict)

    def __post_init__(self) -> None:
        native_dir = _normalize(self.native_library_dir)
        executable = _normalize(self.executable)
        if not executable.startswith(native_dir + "/"):
            raise ValueError("executable must live in native_library_dir")


@dataclass(frozen=True)
class OptionalRuntimeBackendProbe:
    framework_status: str
    available_status: str
    source: str
    backend: str
    candidate_path: str
    can_execute: bool
    reason: str


@dataclass(frozen=True)
class RuntimeConfig:
    package_name: str
    rootfs_dir: str
    cwd: str
    program: str
    env: dict[str, str] = field(default_factory=dict)
    binds: list[tuple[str, str]] = field(default_factory=list)
    hook_path: str = ""
    interposer_path: str = ""
    bridge_path: str = ""
    fake_root: bool = False
    verbose: int = 0
    trace_path: bool = False
    trace_exec: bool = False


@dataclass(frozen=True)
class SerializedRuntimeConfig:
    text: str
    checksum_hex: str


def probe_optional_runtime_backend(
    *,
    backend: str = "none",
    candidate_path: str = "",
) -> OptionalRuntimeBackendProbe:
    """Describe optional low-overhead runtime backend availability without executing it.

    This is intentionally absent-safe scaffolding: no external runtime is bundled,
    launched, or required. A later OSS/proroot-style backend can replace this with
    a real file/signature/smoke probe while preserving these report fields.
    """
    if not candidate_path:
        return OptionalRuntimeBackendProbe(
            framework_status="PASS",
            available_status="SKIP",
            source="none",
            backend="none",
            candidate_path="",
            can_execute=False,
            reason="no optional external runtime backend configured",
        )
    if not candidate_path.startswith("/"):
        raise ValueError("absolute optional runtime backend path required")
    return OptionalRuntimeBackendProbe(
        framework_status="PASS",
        available_status="PASS",
        source="external",
        backend=backend or "external",
        candidate_path=_normalize(candidate_path),
        can_execute=False,
        reason="external candidate declared for future probe integration; not executed by absent-safe scaffolding",
    )


def build_launch_plan(
    *,
    package_name: str,
    native_library_dir: str,
    app_files_dir: str,
    rootfs_name: str,
    program: str,
    backend: str = "loader",
) -> LaunchPlan:
    if not package_name:
        raise ValueError("package_name is required")
    if not program.startswith("/"):
        raise ValueError("program must be an absolute path inside the rootfs")

    native_dir = _normalize(native_library_dir)
    files_dir = _normalize(app_files_dir)
    rootfs_dir = _normalize(str(PurePosixPath(files_dir) / "rootfs" / rootfs_name))
    if backend == "loader":
        executable = _normalize(str(PurePosixPath(native_dir) / "libalr-loader.so"))
        argv = [
            executable,
            "--rootfs",
            rootfs_dir,
            "--program",
            program,
        ]
    elif backend == "alr-runtime":
        executable = _normalize(str(PurePosixPath(native_dir) / "libalr_runtime_launcher.so"))
        argv = [
            executable,
            "--rootfs",
            rootfs_dir,
            "--cwd",
            "/",
            "--program",
            program,
            "--dry-run",
        ]
    elif backend == "proot":
        executable = _normalize(str(PurePosixPath(native_dir) / "libalr_proot.so"))
        argv = [
            executable,
            "-R",
            rootfs_dir,
            "-w",
            "/",
            program,
        ]
    else:
        raise ValueError(f"unknown launch backend: {backend}")

    env = {
        "ALR_PACKAGE": package_name,
        "ALR_ROOTFS": rootfs_dir,
        "ALR_PROGRAM": program,
        "ALR_BACKEND": backend,
        "HOME": "/root",
        "TMPDIR": "/tmp",
        "PATH": "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
    }
    if backend == "proot":
        env["PROOT_NO_SECCOMP"] = "1"
    if backend == "alr-runtime":
        env["ALR_HOOK_PATH"] = _normalize(str(PurePosixPath(native_dir) / "libalr_runtime_hook.so"))
        env["ALR_INTERPOSER_PATH"] = _normalize(str(PurePosixPath(native_dir) / "libalr_runtime_interposer.so"))
        env["ALR_BRIDGE_PATH"] = _normalize(str(PurePosixPath(native_dir) / "libalr_runtime_bridge.so"))
        env["ALR_CONFIG_FORMAT"] = CONFIG_FORMAT
        env["ALR_FAKE_ROOT"] = "0"
        env["ALR_VERBOSE"] = "0"
        env["ALR_TRACE_PATH"] = "0"
        env["ALR_TRACE_EXEC"] = "0"
    return LaunchPlan(
        executable=executable,
        native_library_dir=native_dir,
        rootfs_dir=rootfs_dir,
        argv=argv,
        env=env,
    )


def build_runtime_config(plan: LaunchPlan, *, cwd: str = "/") -> RuntimeConfig:
    if plan.env.get("ALR_BACKEND") != "alr-runtime":
        raise ValueError("runtime config requires alr-runtime launch plan")
    return RuntimeConfig(
        package_name=plan.env["ALR_PACKAGE"],
        rootfs_dir=plan.env["ALR_ROOTFS"],
        cwd=_normalize_guest(cwd),
        program=_normalize_guest(plan.env["ALR_PROGRAM"]),
        env=dict(sorted(plan.env.items())),
        binds=[],
        hook_path=plan.env["ALR_HOOK_PATH"],
        interposer_path=plan.env["ALR_INTERPOSER_PATH"],
        bridge_path=plan.env["ALR_BRIDGE_PATH"],
        fake_root=plan.env["ALR_FAKE_ROOT"] == "1",
        verbose=int(plan.env["ALR_VERBOSE"]),
        trace_path=plan.env["ALR_TRACE_PATH"] == "1",
        trace_exec=plan.env["ALR_TRACE_EXEC"] == "1",
    )


def serialize_runtime_config(config: RuntimeConfig) -> SerializedRuntimeConfig:
    _require(config.package_name, "package_name")
    rootfs_dir = _normalize(config.rootfs_dir)
    cwd = _normalize_guest(config.cwd)
    program = _normalize_guest(config.program)
    lines = [CONFIG_FORMAT]
    fields = {
        "package_name": config.package_name,
        "rootfs_dir": rootfs_dir,
        "cwd": cwd,
        "program": program,
        "hook_path": _normalize_optional(config.hook_path),
        "interposer_path": _normalize_optional(config.interposer_path),
        "bridge_path": _normalize_optional(config.bridge_path),
    }
    for key, value in fields.items():
        lines.append(f"field\t{_escape(key)}\t{_escape(value)}")
    lines.append(f"flag\tfake_root\t{'1' if config.fake_root else '0'}")
    lines.append(f"flag\tverbose\t{config.verbose}")
    lines.append(f"flag\ttrace_path\t{'1' if config.trace_path else '0'}")
    lines.append(f"flag\ttrace_exec\t{'1' if config.trace_exec else '0'}")
    for key, value in sorted(config.env.items()):
        _validate_env_key(key)
        lines.append(f"env\t{_escape(key)}\t{_escape(value)}")
    for guest_path, host_path in sorted((_normalize_guest(g), _normalize(h)) for g, h in config.binds):
        lines.append(f"bind\t{_escape(guest_path)}\t{_escape(host_path)}")
    text = "\n".join(lines) + "\n"
    return SerializedRuntimeConfig(text=text, checksum_hex=_checksum_hex(text))


def _normalize(path: str) -> str:
    if not path.startswith("/"):
        raise ValueError(f"absolute path required: {path}")
    return str(PurePosixPath(path))


def _normalize_optional(path: str) -> str:
    if not path:
        return ""
    return _normalize(path)


def _normalize_guest(path: str) -> str:
    return _normalize(path)


def _require(value: str, name: str) -> None:
    if not value:
        raise ValueError(f"{name} is required")


def _validate_env_key(key: str) -> None:
    _require(key, "env key")
    if "=" in key or any(ord(ch) < 0x20 for ch in key):
        raise ValueError("env key contains an invalid character")


def _escape(value: str) -> str:
    out = []
    for ch in value:
        code = ord(ch)
        if ch in "%\n\r\t" or code < 0x20 or code == 0x7F:
            out.append(f"%{code:02X}")
        else:
            out.append(ch)
    return "".join(out)


def _checksum_hex(text: str) -> str:
    hash_value = 0xCBF29CE484222325
    for byte in text.encode():
        hash_value ^= byte
        hash_value = (hash_value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f"{hash_value:016x}"
