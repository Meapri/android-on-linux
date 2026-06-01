"""WS-5 drift guard for the app-compat matrix's evidence citations.

`docs/research/alr-compat-matrix.md` is the WS-5-maintained app x result table.
Per its own update rule it cites device-evidence by name/version, so it can rot in
two ways: (A) it can grow a dangling `docs/...md` path that no longer resolves, or
(B) it can drift away from the real evidence corpus until it no longer actually
cites it. These two lenient tests pin both directions without being brittle about
the exact citation spelling.

Citation style note: the matrix cites evidence by the *short* version name
(e.g. `v79-dynamic-loader`, `v126-harfbuzz-fix-display-90hz`) rather than the full
dated stem (`2026-05-31-device-SM-X236N-v79-dynamic-loader`). Test B accounts for
this by matching on the distinctive tail of each stem (date / device-id prefix
stripped), so it stays grounded in the real corpus without forcing a rigid spelling.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MATRIX = ROOT / "docs" / "research" / "alr-compat-matrix.md"
EVIDENCE_DIR = ROOT / "docs" / "evidence"

# A YYYY-MM-DD- date prefix and the shared device-id segment are boilerplate that
# the matrix omits when citing; strip them to get the distinctive citation tail.
_DATE_PREFIX = re.compile(r"^\d{4}-\d{2}-\d{2}-")
_DEVICE_SEG = "device-SM-X236N-"

# `[\w./-]+\.md` — any .md-looking token (paths or bare names).
_MD_TOKEN = re.compile(r"[\w./-]+\.md")
# Path tokens we can resolve unambiguously: those rooted at the repo's docs/ tree.
_DOCS_PATH = re.compile(r"docs/[\w./-]+\.md")


def _matrix_text() -> str:
    assert MATRIX.is_file(), f"compat matrix missing: {MATRIX}"
    return MATRIX.read_text(encoding="utf-8")


def _evidence_stems() -> set[str]:
    return {p.stem for p in EVIDENCE_DIR.glob("*.md")}


def _cite_key(stem: str) -> str:
    """Distinctive citation tail of an evidence stem (date + device prefix stripped)."""
    key = _DATE_PREFIX.sub("", stem)
    if key.startswith(_DEVICE_SEG):
        key = key[len(_DEVICE_SEG):]
    return key


def test_no_dangling_docs_paths():
    """Test A: every `docs/...md` path-token in the matrix resolves to a real file.

    We only check tokens rooted at `docs/` (those are unambiguously repo paths);
    bare names like `v79-dynamic-loader` are short citations, not paths, and are
    covered by Test B instead. If the matrix carries zero such path-tokens this
    passes trivially -- Test B is then the meaningful floor.
    """
    text = _matrix_text()
    docs_tokens = sorted(set(_DOCS_PATH.findall(text)))
    missing = [t for t in docs_tokens if not (ROOT / t).is_file()]
    assert not missing, (
        f"compat matrix cites docs/ path(s) that do not exist under {ROOT}: {missing}"
    )

    # Belt-and-suspenders: any *other* .md token that explicitly names the
    # evidence/research dirs must also resolve (these are real paths, not short
    # citations). Short bare-name citations are deliberately skipped.
    for tok in sorted(set(_MD_TOKEN.findall(text))):
        if tok in docs_tokens:
            continue
        if "evidence/" in tok or "research/" in tok:
            assert (ROOT / tok).is_file(), (
                f"compat matrix references {tok!r} but no such file exists under {ROOT}"
            )


def test_matrix_is_grounded_in_evidence_corpus():
    """Test B: the matrix genuinely cites the evidence corpus, not nothing.

    A stem counts as cited if its distinctive tail (`_cite_key`) appears verbatim
    in the matrix, which captures the matrix's short-name citation style. At least
    5 distinct real evidence docs must be cited -- this is the non-negotiable floor
    proving the table is anchored to actual on-disk evidence.
    """
    text = _matrix_text()
    stems = _evidence_stems()
    assert stems, f"no evidence docs found under {EVIDENCE_DIR}"

    cited = set()
    for stem in stems:
        key = _cite_key(stem)
        # require a reasonably distinctive key so we don't match generic fragments
        if len(key) >= 6 and key in text:
            cited.add(stem)

    assert len(cited) >= 5, (
        "compat matrix appears ungrounded: expected >=5 real evidence stems cited, "
        f"found {len(cited)}: {sorted(cited)}"
    )
