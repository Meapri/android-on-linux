from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/validate-host.sh"


def test_validate_host_script_uses_python_310_or_newer():
    text = SCRIPT.read_text(encoding="utf-8")
    assert "find_python()" in text
    assert "python3.13 python3.12 python3.11 python3.10 python3" in text
    assert "sys.version_info < (3, 10)" in text
    assert "import pytest" in text
    assert 'PYTHON_BIN="${PYTHON:-$(find_python)}"' in text


def test_validate_host_script_requires_current_architecture_docs():
    text = SCRIPT.read_text(encoding="utf-8")
    for path in [
        "docs/research/prior-art.md",
        "docs/architecture/device-evidence.md",
        "docs/architecture/execution-backend.md",
        "docs/architecture/gpu-display-bridge.md",
        "docs/adr/0001-runtime-model.md",
        "docs/adr/0002-gpu-public-api.md",
    ]:
        assert path in text
