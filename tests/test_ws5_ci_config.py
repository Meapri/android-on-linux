"""WS-5 M5 host CI gate: assert the CI workflow and shared test entrypoint exist
and are wired correctly (host-runnable, no device interaction)."""

import os
import stat
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

WORKFLOW = ROOT / ".github" / "workflows" / "ws5-host-ci.yml"
RUN_SCRIPT = ROOT / "scripts" / "run-host-tests.sh"


def test_workflow_exists_and_runs_host_pytest():
    assert WORKFLOW.is_file(), f"missing CI workflow: {WORKFLOW}"
    text = WORKFLOW.read_text()
    for needle in ("uvx pytest", "tests/", "on:", "push", "pull_request"):
        assert needle in text, f"workflow missing expected token: {needle!r}"


def test_run_script_exists_and_is_executable():
    assert RUN_SCRIPT.is_file(), f"missing entrypoint script: {RUN_SCRIPT}"
    text = RUN_SCRIPT.read_text()
    assert "uvx pytest" in text, "run script must invoke uvx pytest"

    mode = RUN_SCRIPT.stat().st_mode
    is_executable = os.access(RUN_SCRIPT, os.X_OK) or bool(mode & stat.S_IXUSR)
    assert is_executable, f"run script not owner-executable: {oct(mode)}"
