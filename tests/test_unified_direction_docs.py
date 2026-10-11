"""Guards for the unified-direction documentation (ADR 0004).

These tests keep the documents honest in two ways: the decision record and its
cross-references exist, and every relative markdown link in the revised
documents resolves to a real file (a broken link looks like a citation).
"""

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ADR4 = ROOT / "docs/adr/0004-unified-direction.md"

REVISED_DOCS = [
    "README.md",
    "docs/architecture.md",
    "docs/adr/0001-runtime-model.md",
    "docs/adr/0004-unified-direction.md",
    "docs/adr/README.md",
    "docs/architecture/execution-backend.md",
    "docs/architecture/gpu-display-bridge.md",
    "docs/product-requirements.md",
    "docs/research/prior-art.md",
    "docs/PERFORMANCE.md",
    "docs/alr-execution-backend-spec.md",
    "docs/android-graphics-bridge-spec.md",
    "docs/plans/implementation-milestones.md",
    "docs/plans/parallel-workstreams.md",
    "docs/poc-roadmap.md",
    "runtime/alr/README.md",
    "runtime/alr/docs/adr/0006-raw-syscall-binaries.md",
]


def test_adr_0004_records_decisions_and_phases():
    text = ADR4.read_text(encoding="utf-8")
    assert "Accepted" in text
    for decision in ["D1.", "D2.", "D3.", "D4.", "D5.", "D6.", "D7.", "D8."]:
        assert f"### {decision}" in text
    for phase in ["P0", "P1", "P2", "P3", "P4"]:
        assert f"| {phase} |" in text
    # The honesty rules: what is unverified must stay visible.
    assert "미검증" in text
    assert "ALR_GPU_RING_FD" in text


def test_adr_0004_is_discoverable_from_entry_points():
    assert "docs/adr/0004-unified-direction.md" in (ROOT / "README.md").read_text(encoding="utf-8")
    assert "0004-unified-direction.md" in (ROOT / "docs/adr/README.md").read_text(encoding="utf-8")
    assert "ADR 0004" in (ROOT / "docs/adr/0001-runtime-model.md").read_text(encoding="utf-8")


def test_performance_numbers_carry_an_engine_label():
    text = (ROOT / "docs/PERFORMANCE.md").read_text(encoding="utf-8")
    assert "engine=legacy-loader" in text
    assert "engine=alr" in text


def test_relative_links_in_revised_docs_resolve():
    broken = []
    for rel in REVISED_DOCS:
        path = ROOT / rel
        text = path.read_text(encoding="utf-8")
        for match in re.finditer(r"\]\(([^)\s]+)\)", text):
            link = match.group(1)
            if link.startswith(("http://", "https://", "mailto:", "#")):
                continue
            target = link.split("#", 1)[0]
            if not target:
                continue
            if not (path.parent / target).resolve().exists():
                broken.append(f"{rel} -> {link}")
    assert not broken, broken
