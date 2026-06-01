#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

find_python() {
  for candidate in python3.13 python3.12 python3.11 python3.10 python3; do
    command -v "$candidate" >/dev/null 2>&1 || continue
    "$candidate" - <<'PY' >/dev/null 2>&1 && {
import sys
if sys.version_info < (3, 10):
    raise SystemExit(1)
try:
    import pytest  # noqa: F401
except ModuleNotFoundError:
    raise SystemExit(1)
PY
      echo "$candidate"
      return 0
    }
  done
  echo "missing Python >= 3.10 with pytest for host tests; set PYTHON=/path/to/python" >&2
  return 127
}

PYTHON_BIN="${PYTHON:-$(find_python)}"
"$PYTHON_BIN" -m pytest tests/ -q
./scripts/test-native-core.sh

required=(
  "settings.gradle.kts"
  "build.gradle.kts"
  "app/build.gradle.kts"
  "app/src/main/AndroidManifest.xml"
  "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"
  "app/src/main/cpp/CMakeLists.txt"
  "app/src/main/cpp/runtime_plan.cpp"
  "app/src/main/cpp/runtime_plan.hpp"
  "app/src/main/cpp/runtime_report.cpp"
  "docs/architecture.md"
  "docs/architecture/device-evidence.md"
  "docs/architecture/execution-backend.md"
  "docs/architecture/gpu-display-bridge.md"
  "docs/adr/0001-runtime-model.md"
  "docs/adr/0002-gpu-public-api.md"
  "docs/build-environment.md"
  "docs/poc-roadmap.md"
  "docs/research/prior-art.md"
  "docs/rootfs-extraction-safety.md"
  "rootfs/manifests/debian-arm64-bookworm-slim.json"
)

for path in "${required[@]}"; do
  test -f "$path" || { echo "missing: $path" >&2; exit 1; }
done

echo "host validation ok"
