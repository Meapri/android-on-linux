"""Aggregate the WS-5 device-evidence corpus (docs/evidence/*.md) into an index.

Pure functions + a frozen dataclass — no device, no network. Scans a directory of
device-evidence markdown files, parses a date/version/title out of each, and emits a
markdown index table.

The evidence corpus predates the standardized `_TEMPLATE.md`, so most existing docs do
NOT carry the recommended `## What changed` / `## Honest scope` sections. For that
reason `validate_evidence_doc` is an ADVISORY check only: it reports which recommended
sections are missing so new docs can be nudged toward the template, but it is never a
hard gate over the historical files.

Helper/non-evidence files are excluded: anything whose name starts with `_`
(e.g. `_TEMPLATE.md`) and `CAPTURE-RUNBOOK.md`.
"""
from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

# Recommended (not required) sections per the device-evidence template.
RECOMMENDED_SECTIONS: tuple[str, ...] = ("What changed", "Honest scope")

_DATE_RE = re.compile(r"^(\d{4}-\d{2}-\d{2})")
_VERSION_RE = re.compile(r"v\d+")


@dataclass(frozen=True)
class EvidenceDoc:
    """One device-evidence markdown file, with parsed metadata.

    `date` is the leading YYYY-MM-DD from the filename (or None), `version` is the first
    `v<digits>` token found in the filename or title (or None), `title` is the first
    `# ` H1 line (or the filename stem when no H1 is present).
    """

    filename: str
    title: str
    date: str | None
    version: str | None


def _parse_title(path: Path) -> str:
    """First `# ` H1 line (without the leading marker), else the filename stem."""
    try:
        for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
            line = raw.strip()
            if line.startswith("# "):
                return line[2:].strip()
    except OSError:
        pass
    return path.stem


def _parse_date(filename: str) -> str | None:
    m = _DATE_RE.match(filename)
    return m.group(1) if m else None


def _parse_version(filename: str, title: str) -> str | None:
    m = _VERSION_RE.search(filename)
    if m:
        return m.group(0)
    m = _VERSION_RE.search(title)
    return m.group(0) if m else None


def _is_excluded(name: str) -> bool:
    return name.startswith("_") or name == "CAPTURE-RUNBOOK.md"


def scan_evidence_dir(evidence_dir: str | Path) -> list[EvidenceDoc]:
    """Scan a directory of `*.md` device-evidence files into `EvidenceDoc`s.

    Skips helper files (name starts with `_`) and `CAPTURE-RUNBOOK.md`. Returns the list
    sorted by (date or "", filename) so undated docs sort first and ties break by name.
    """
    directory = Path(evidence_dir)
    docs: list[EvidenceDoc] = []
    for path in directory.glob("*.md"):
        name = path.name
        if _is_excluded(name):
            continue
        title = _parse_title(path)
        date = _parse_date(name)
        version = _parse_version(name, title)
        docs.append(EvidenceDoc(filename=name, title=title, date=date, version=version))
    docs.sort(key=lambda d: (d.date or "", d.filename))
    return docs


def _escape_cell(text: str) -> str:
    """Keep a value on one table cell: drop pipes/newlines that would break the row."""
    return text.replace("|", "\\|").replace("\n", " ").strip()


def build_index_markdown(docs: list[EvidenceDoc]) -> str:
    """Render the docs as a markdown index table (Date | Version | Title | File)."""
    lines = [
        "# Device Evidence Index",
        "",
        f"{len(docs)} evidence document(s).",
        "",
        "| Date | Version | Title | File |",
        "| --- | --- | --- | --- |",
    ]
    for d in docs:
        lines.append(
            "| {date} | {version} | {title} | {file} |".format(
                date=d.date or "",
                version=d.version or "",
                title=_escape_cell(d.title),
                file=_escape_cell(d.filename),
            )
        )
    return "\n".join(lines) + "\n"


def validate_evidence_doc(text: str) -> list[str]:
    """Advisory: return RECOMMENDED_SECTIONS not present in `text` (case-insensitive).

    Lenient substring match — a section counts as present if its name appears anywhere
    in the text (e.g. as a `## What changed` heading). Existing docs predate the
    template, so this is guidance for new docs, never a hard gate.
    """
    lowered = text.lower()
    return [s for s in RECOMMENDED_SECTIONS if s.lower() not in lowered]
