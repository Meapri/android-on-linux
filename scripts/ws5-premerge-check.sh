#!/usr/bin/env sh
# WS-5 pre-merge HOST gate self-check (worktree-local).
# Runs the host-automatable gate (full pytest suite). On success, prints the
# WS-5 HOST GATE marker + a reminder of the MANUAL device gates that the
# INTEGRATION session must still satisfy per docs/research/ws5-premerge-gate.md.
# Propagates pytest's exit code on failure. Run from the worktree root.
set -eu

# Ensure uv's user-install bin dir is on PATH (host has no system pytest -> uvx).
PATH="$HOME/.local/bin:$PATH"
export PATH

uvx pytest tests/ -q

echo "WS-5 HOST GATE: PASS"
echo
echo "Reminder — MANUAL device gates (INTEGRATION session, per device-gated CP):"
echo "  - force-stop + cold start before capture (am force-stop, then cold launch)"
echo "  - python -m bench gate <report>  -> PASS (pcgate=1 interpose=1 traps=0 rewrites=0)"
echo "  - CP-1: bench.display_verify -> 1200x1920 @ 90Hz device-exact"
echo "  - CP-2: python -m bench gpu ... -> ratio >= 0.70 AND software=false"
echo "  - file evidence from docs/evidence/_TEMPLATE.md"
