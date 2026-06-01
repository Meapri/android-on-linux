"""WS-5 host tests: the §5 pre-merge gate deliverable (doc + self-check script).

Verifies the gate doc and the worktree-local host-gate script exist and carry the
load-bearing tokens the INTEGRATION session relies on (uvx pytest entrypoint,
CP-1/CP-2 device gates, the no-regression `bench gate`, and the HOST GATE marker
the script prints on success). The script must also be owner-executable.
"""
import os
import stat
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOC = ROOT / "docs" / "research" / "ws5-premerge-gate.md"
SCRIPT = ROOT / "scripts" / "ws5-premerge-check.sh"


def test_gate_doc_exists_and_covers_key_gates():
    assert DOC.is_file(), f"missing gate doc: {DOC}"
    text = DOC.read_text(encoding="utf-8")
    for token in ("uvx pytest", "CP-1", "CP-2", "bench gate"):
        assert token in text, f"gate doc must mention {token!r}"


def test_premerge_script_exists_and_is_executable():
    assert SCRIPT.is_file(), f"missing self-check script: {SCRIPT}"
    mode = SCRIPT.stat().st_mode
    assert mode & stat.S_IXUSR, "script must have the owner-executable bit set"


def test_premerge_script_runs_host_gate_and_prints_marker():
    text = SCRIPT.read_text(encoding="utf-8")
    assert "uvx pytest" in text, "script must run the uvx pytest host gate"
    assert "WS-5 HOST GATE" in text, "script must print the WS-5 HOST GATE marker"
