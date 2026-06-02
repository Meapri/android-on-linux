"""ADR-003 §6 P1 — exec/clone decomposition parser (tests/exec_map_model.py) regression.

Pins the host-side classifier that turns the M-R4-execmap supervisor lines into
the "auto-mediated (clone) vs exec-wall (exec)" split ADR-003 §2/§5 needs, and
the /proc/self/exe gate (가정-3) that blocks (B-1) x0-rewrite implementation.
Pure / host-only — the inheritance effect is DEVICE-ONLY.
"""
from __future__ import annotations

from tests.exec_map_model import (
    BUCKET_OTHER_ABS,
    BUCKET_PROC_SELF_EXE,
    BUCKET_RELATIVE,
    BUCKET_ROOTFS_ABS,
    classify_exec_path,
    parse_exec_map,
    render_markdown,
)

ROOTFS = "/data/rootfs"


def test_classify_proc_self_exe_is_design_breaking():
    assert classify_exec_path("/proc/self/exe", ROOTFS) == BUCKET_PROC_SELF_EXE
    assert classify_exec_path("/proc/1234/exe", ROOTFS) == BUCKET_PROC_SELF_EXE


def test_classify_rootfs_internal_absolute():
    assert classify_exec_path("/data/rootfs/usr/lib/chromium/chrome", ROOTFS) == BUCKET_ROOTFS_ABS


def test_classify_rootfs_external_absolute():
    assert classify_exec_path("/usr/lib/chromium/chrome", ROOTFS) == BUCKET_OTHER_ABS


def test_classify_relative():
    assert classify_exec_path("chrome", ROOTFS) == BUCKET_RELATIVE
    assert classify_exec_path("", ROOTFS) == BUCKET_RELATIVE


def test_parse_clone_exec_counters_and_ratio():
    text = (
        "alr exec clone_events=9 exec_events=3\n"
        "alr exec x0=/data/rootfs/usr/lib/chromium/chrome\n"
        "alr exec x0=/data/rootfs/usr/lib/chromium/chrome\n"
        "alr exec x0=/data/rootfs/usr/lib/chromium/chrome\n"
    )
    em = parse_exec_map(text, ROOTFS)
    assert em.clone_events == 9
    assert em.exec_events == 3
    assert em.total_children == 12
    assert abs(em.auto_mediated_fraction - 9 / 12) < 1e-9
    # All three execs are rootfs-내 절대경로 → x0-rewrite unblocked.
    assert em.buckets.get(BUCKET_ROOTFS_ABS) == 3
    assert em.x0_rewrite_unblocked is True
    assert em.has_proc_self_exe is False


def test_proc_self_exe_blocks_x0_rewrite():
    text = (
        "alr exec clone_events=20 exec_events=2\n"
        "alr exec x0=/proc/self/exe\n"
        "alr exec x0=/data/rootfs/usr/lib/chromium/chrome\n"
    )
    em = parse_exec_map(text, ROOTFS)
    assert em.has_proc_self_exe is True
    # ADR-003 §4 가정-3: a single /proc/self/exe exec breaks the §3 design.
    assert em.x0_rewrite_unblocked is False


def test_single_process_has_no_exec_wall():
    """--single-process --no-zygote: exec_events=0 → x0-rewrite n/a, no wall."""
    text = "alr exec clone_events=0 exec_events=0\n"
    em = parse_exec_map(text, ROOTFS)
    assert em.exec_events == 0
    assert em.x0_rewrite_unblocked is False  # nothing to rewrite
    assert em.auto_mediated_fraction == 0.0


def test_missing_counter_line_is_tolerant():
    em = parse_exec_map("garbage line\n", ROOTFS)
    assert em.clone_events == 0 and em.exec_events == 0


def test_render_markdown_contains_buckets_and_verdict():
    text = (
        "alr exec clone_events=9 exec_events=3\n"
        "alr exec x0=/data/rootfs/usr/lib/chromium/chrome\n"
        "alr exec x0=/data/rootfs/usr/lib/chromium/chrome\n"
        "alr exec x0=/data/rootfs/usr/lib/chromium/chrome\n"
    )
    md = render_markdown(parse_exec_map(text, ROOTFS))
    assert "M-R4-execmap" in md
    assert "clone_events" in md and "exec_events" in md
    assert "rootfs_abs" in md
    assert "UNBLOCKED" in md
